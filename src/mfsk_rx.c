/*
 * fldigi-mfsk-core: the MFSK receiver of fldigi 4.2.13 (src/mfsk/mfsk.cxx).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 src/mfsk/mfsk.cxx (c) 2006-2009 Dave Freese W1HKJ, John
 * Douyere VK2ETA, based on gmfsk (c) 2001-2003 Tomi Manninen OH2BNS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Line by line after fldigi, with fldigi's defaults (configuration.h, status.cxx): AFC and
 * squelch as configured (the fldigi bench: both off), reverse off, PSKmail s/n report off.
 * Deviations that do not change decoded text:
 * - softdecode's CWI counters and eval_s2n's s/n table are statics shared by every MFSK
 *   instance in fldigi (mfsk.cxx:69-71, 603); here they belong to one receiver, which
 *   equals fldigi for a single receiver;
 * - scopes, waterfall, GUI status and the picture viewer are left out: picture rows go to
 *   a bounded ring (fmfsk_rx_poll_picture) instead of updateRxPic/save_png;
 * - std::complex products are written as four separate products (fldigi's build never
 *   fuses them); every other expression stays one C expression so that a build with
 *   -ffp-contract=on fuses what fldigi's -O2 build fuses.
 */
#include "mfsk_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { RX_STATE_DATA, RX_STATE_PICTURE_START, RX_STATE_PICTURE };   /* mfsk.h:139-145 */

static double cabs_(fdsp_cmpx z)
{
    return hypot(z.re, z.im);   /* std::abs(std::complex<double>) */
}

static fdsp_cmpx cmul(fdsp_cmpx a, fdsp_cmpx b)
{
    double ac = a.re * b.re, bd = a.im * b.im, ad = a.re * b.im, bc = a.im * b.re;
    fdsp_cmpx z;
    z.re = ac - bd;
    z.im = ad + bc;
    return z;
}

static fdsp_cmpx conj_(fdsp_cmpx z)
{
    z.im = -z.im;
    return z;
}

static void emit(fmfsk_rx *rx, int c)
{
    if (rx->n < rx->max && rx->out)
        rx->out[rx->n++] = (unsigned char)c;
    else
        rx->dropped++;
}

/* modem::set_freq, trx/modem.cxx:319-333 (the waterfall limits clamp the carrier) */
static void set_freq(fmfsk_rx *rx, double freq)
{
    rx->frequency = fmfsk__clamp(freq, MFSK_LOW_CUTOFF + rx->m->bandwidth / 2,
                                 MFSK_HIGH_CUTOFF - rx->m->bandwidth / 2);
}

/* ---------------------------------------------------------------------------------- */
/* rx_init, reset_afc: mfsk.cxx:95-122, 786-790                                        */
/* ---------------------------------------------------------------------------------- */

static void reset_afc(fmfsk_rx *rx)
{
    /* mfsk::reset_afc, mfsk.cxx:786-790 */
    rx->freqerr = 0.0;
    fdsp_movavg_reset(&rx->syncfilter);
}

static void rx_init(fmfsk_rx *rx)
{
    /* mfsk::rx_init, mfsk.cxx:95-122 */
    int i, n;
    rx->rxstate = RX_STATE_DATA;
    rx->synccounter = 0;
    rx->symcounter = 0;
    rx->met1 = 0.0;
    rx->met2 = 0.0;
    rx->counter = 0;
    rx->RXspp = 8;
    for (i = 0; i < 2 * rx->m->symlen; i++)
        memset(rx->pipe[i].vector, 0, sizeof rx->pipe[i].vector);
    reset_afc(rx);
    fmfsk__clear_picture_header(rx->picheader);
    fdsp_movavg_reset(&rx->syncfilter);
    rx->staticburst = 0;
    rx->s2n = 0.0;
    for (n = 0; n < 100; n++)
        rx->s2nvals[n] = -20;
}

/* ---------------------------------------------------------------------------------- */
/* pictures: recvpic, recvchar (mfsk.cxx:430-529)                                       */
/* ---------------------------------------------------------------------------------- */

static void ring_push(fmfsk_rx *rx, int row, int channel)
{
    size_t slot;
    fmfsk_picture_row *h;
    if (rx->ring_count == FMFSK_PIC_RING) {
        /* the oldest row goes (R-837.1) */
        rx->ring_head = (rx->ring_head + 1) % FMFSK_PIC_RING;
        rx->ring_count--;
        rx->pic.rows_dropped++;
    }
    slot = (rx->ring_head + rx->ring_count) % FMFSK_PIC_RING;
    h = &rx->ringhdr[slot];
    h->picture = rx->pic.started;
    h->width = rx->picW;
    h->height = rx->picH;
    h->color = rx->color;
    h->row = row;
    h->channel = channel;
    h->n = rx->picW;
    memcpy(rx->ring + slot * FMFSK_PIC_MAXW, rx->picrow, (size_t)rx->picW);
    rx->ring_count++;
}

static void store_pixel(fmfsk_rx *rx, int byte)
{
    /* updateRxPic(byte, pixelnbr) (mfsk-pic.cxx:83-94): colour pixels arrive per row as
     * all red, all green, all blue; grey pixels set R, G and B of one pixel. */
    if (rx->color) {
        rx->picrow[rx->col] = (unsigned char)byte;
        if (++rx->col == rx->picW) {
            ring_push(rx, rx->row / (3 * rx->picW), rx->rgb);
            rx->col = 0;
            if (++rx->rgb == 3) {
                rx->rgb = 0;
                rx->row += 3 * rx->picW;
            }
        }
    } else {
        int x = (rx->pixelnbr / 3) % rx->picW;
        rx->picrow[x] = (unsigned char)byte;
        rx->pixelnbr += 3;
        if (x == rx->picW - 1)
            ring_push(rx, (rx->pixelnbr / 3 - 1) / rx->picW, -1);
    }
    rx->pic.pixels++;
}

static void flush_partial_row(fmfsk_rx *rx)
{
    /* fldigi ends the picture one pixel early (the last pixel would fall on counter 0,
     * mfsk.cxx:888-893): hand the incomplete last row over with the rest zeroed */
    int x;
    if (rx->color) {
        if (rx->col == 0)
            return;
        memset(rx->picrow + rx->col, 0, (size_t)(rx->picW - rx->col));
        ring_push(rx, rx->row / (3 * rx->picW), rx->rgb);
        return;
    }
    x = (rx->pixelnbr / 3) % rx->picW;
    if (x == 0)
        return;
    memset(rx->picrow + x, 0, (size_t)(rx->picW - x));
    ring_push(rx, (rx->pixelnbr / 3) / rx->picW, -1);
}

static void recvpic(fmfsk_rx *rx, fdsp_cmpx z)
{
    /* mfsk::recvpic, mfsk.cxx:430-466 */
    int byte;
    fdsp_cmpx d = cmul(conj_(rx->prevz), z);
    rx->picf += atan2(d.im, d.re) * rx->m->samplerate / MFSK_TWOPI;
    rx->prevz = z;
    if ((rx->counter % rx->RXspp) == 0) {
        double basefreq = 1.0 * rx->m->samplerate * rx->m->basetone / rx->m->symlen;
        rx->picf = 256 * (rx->picf / rx->RXspp - basefreq) / rx->m->bandwidth;
        byte = (int)fmfsk__clamp(rx->picf, 0.0, 255.0);
        store_pixel(rx, byte);
        rx->picf = 0.0;
    }
}

static void recvchar(fmfsk_rx *rx, int c)
{
    /* mfsk::recvchar, mfsk.cxx:468-529 */
    int w, h, color, spp = rx->RXspp;
    const fmfsk_submode *m = rx->m;
    if (c == -1 || c == 0)
        return;
    emit(rx, c);
    if (fmfsk__check_picture_header(rx->picheader, c, &w, &h, &color, &spp)) {
        rx->picW = w;
        rx->picH = h;
        rx->color = color;
        rx->RXspp = spp;
        rx->counter = MFSK_TRACE_DELAY;
        switch (m->fldigi_id) {
        case MFSK_ID_MFSK16:
        case MFSK_ID_MFSK32:
            if (rx->symbolbit == m->symbits)
                rx->counter += m->symlen;
            break;
        case MFSK_ID_MFSK64:
            rx->counter = 4956;
            if (rx->symbolbit % 2 == 0)
                rx->counter += m->symlen;
            break;
        case MFSK_ID_MFSK128:
            rx->counter = 1824;
            if (rx->symbolbit % 2 == 0)
                rx->counter += m->symlen;
            break;
        case MFSK_ID_MFSK31:
            rx->counter = 5216;
            if (rx->symbolbit == m->symbits)
                rx->counter += m->symlen;
            break;
        default:
            break;
        }
        rx->rxstate = RX_STATE_PICTURE_START;
        rx->picturesize = rx->RXspp * rx->picW * rx->picH * (rx->color ? 3 : 1);
        rx->pixelnbr = 0;
        rx->col = 0;
        rx->row = 0;
        rx->rgb = 0;
        fmfsk__clear_picture_header(rx->picheader);
        rx->pic.active = 1;
        rx->pic.started++;
        rx->pic.width = rx->picW;
        rx->pic.height = rx->picH;
        rx->pic.color = rx->color;
        rx->pic.spp = rx->RXspp;
        rx->pic.pixels = 0;
        return;
    }
    rx->counter = 0;
}

/* ---------------------------------------------------------------------------------- */
/* recvbit, decodesymbol, softdecode: mfsk.cxx:531-688                                 */
/* ---------------------------------------------------------------------------------- */

static void recvbit(fmfsk_rx *rx, int bit)
{
    /* mfsk::recvbit, mfsk.cxx:531-541 */
    int c;
    rx->datashreg = (rx->datashreg << 1) | !!bit;
    if ((rx->datashreg & 7) == 1) {
        c = fdsp_mfsk_varidec(rx->datashreg >> 1);
        recvchar(rx, c);
        rx->datashreg = 1;
    }
}

static void decodesymbol(fmfsk_rx *rx, unsigned char symbol)
{
    /* mfsk::decodesymbol, mfsk.cxx:543-595 */
    int c, met;
    const int symbits = rx->m->symbits;
    rx->symbolpair[0] = rx->symbolpair[1];
    rx->symbolpair[1] = symbol;
    rx->symcounter = rx->symcounter ? 0 : 1;
    /* only modes with odd number of symbits need a vote */
    if (symbits == 5 || symbits == 3) {
        if (rx->symcounter) {
            if ((c = fdsp_viterbi_decode(&rx->dec1, rx->symbolpair, &met)) == -1)
                return;
            rx->met1 = fdsp_decayavg(rx->met1, met, 50);
            if (rx->met1 < rx->met2)
                return;
            rx->metric = rx->met1;
        } else {
            if ((c = fdsp_viterbi_decode(&rx->dec2, rx->symbolpair, &met)) == -1)
                return;
            rx->met2 = fdsp_decayavg(rx->met2, met, 50);
            if (rx->met2 < rx->met1)
                return;
            rx->metric = rx->met2;
        }
    } else {
        if (rx->symcounter)
            return;
        if ((c = fdsp_viterbi_decode(&rx->dec2, rx->symbolpair, &met)) == -1)
            return;
        rx->met2 = fdsp_decayavg(rx->met2, met, 50);
        rx->metric = rx->met2;
    }
    /* re-scale the metric (display_metric) */
    rx->metric -= 60.0;
    rx->metric *= 0.5;
    rx->metric = fmfsk__clamp(rx->metric, 0.0, 100.0);
    if (rx->sql_on && rx->metric < rx->squelch)
        return;
    recvbit(rx, c);
}

static void tap_soft(fmfsk_rx *rx, unsigned char s)
{
    rx->soft[(rx->soft_head + rx->soft_count) % MFSK_TAP_SOFT] = s;
    if (rx->soft_count < MFSK_TAP_SOFT)
        rx->soft_count++;
    else
        rx->soft_head = (rx->soft_head + 1) % MFSK_TAP_SOFT;
}

static void softdecode(fmfsk_rx *rx, const fdsp_cmpx *bins)
{
    /* mfsk::softdecode, mfsk.cxx:597-688 (reverse off) */
    const int symbits = rx->m->symbits, numtones = rx->m->numtones;
    double binmag, sum = 0, avg = 0, b[5];
    unsigned char symbols[5];
    int i, j, k, CWIsymbol;
    int *CWIcounter = rx->cwicounter;

    for (i = 0; i < symbits; i++)
        b[i] = 0.0;
    /* calculate the average signal, ignoring CWI tones */
    for (i = 0; i < numtones; i++) {
        if (CWIcounter[i] < MFSK_CWI_MAXCOUNT)
            sum += cabs_(bins[i]);
    }
    avg = sum / numtones;
    /* avoid divide by zero later */
    if (sum < 1e-10)
        sum = 1e-10;
    /* dynamic CWI avoidance: use the harddecode() result for CWI detection */
    CWIsymbol = rx->currsymbol;
    /* add or subtract the CWI counters, avoiding tone #0 by starting at 1 */
    for (i = 1; i < numtones; i++) {
        k = i;
        if (k == CWIsymbol)
            CWIcounter[k]++;
        else
            CWIcounter[k]--;
        if (CWIcounter[k] < 0)
            CWIcounter[k] = 0;
        if (CWIcounter[k] > MFSK_CWI_MAXCOUNT)
            CWIcounter[k] = MFSK_CWI_MAXCOUNT + 1;
    }
    /* Gray decode and form soft decision samples */
    for (i = 0; i < numtones; i++) {
        j = fdsp_gray_code((uint8_t)i);   /* graydecode, misc.cxx:139-143 */
        k = i;
        if (CWIcounter[k] > MFSK_CWI_MAXCOUNT)
            binmag = avg;   /* soft-puncture to the average signal level */
        else if (CWIsymbol == k)
            binmag = 2.0f * cabs_(bins[k]);   /* give harddecode() a vote */
        else
            binmag = cabs_(bins[k]);
        for (k = 0; k < symbits; k++)
            b[k] += (j & (1 << (symbits - k - 1))) ? binmag : -binmag;
    }
    /* shift to range 0...255 */
    for (i = 0; i < symbits; i++) {
        unsigned char softbits;
        if (rx->staticburst)
            softbits = 128;   /* puncturing */
        else
            softbits = (unsigned char)fmfsk__clamp(128.0 + (b[i] / (sum) * 256.0), 0, 255);
        symbols[i] = softbits;
    }
    fdsp_interleave_symbols(&rx->rxinlv, symbols);
    for (i = 0; i < symbits; i++) {
        tap_soft(rx, symbols[i]);
        rx->symbolbit = i + 1;
        decodesymbol(rx, symbols[i]);
        if (rx->counter)
            return;
    }
}

/* ---------------------------------------------------------------------------------- */
/* mixer, harddecode, synchronize, afc, eval_s2n: mfsk.cxx:690-855                      */
/* ---------------------------------------------------------------------------------- */

static fdsp_cmpx mixer(fmfsk_rx *rx, fdsp_cmpx in, double f)
{
    /* mfsk::mixer, mfsk.cxx:690-703 */
    const fmfsk_submode *m = rx->m;
    fdsp_cmpx z, nco;
    double s, c;
    /* basetone is a nominal 1000 Hz */
    f -= m->tonespacing * m->basetone + m->bandwidth / 2;
    fdsp_sincos(rx->phaseacc, &s, &c);
    nco.re = c;
    nco.im = s;
    z = cmul(in, nco);
    rx->phaseacc -= MFSK_TWOPI * f / m->samplerate;
    if (rx->phaseacc < 0)
        rx->phaseacc += MFSK_TWOPI;
    return z;
}

static int harddecode(fmfsk_rx *rx, const fdsp_cmpx *in)
{
    /* mfsk::harddecode, mfsk.cxx:709-738 */
    const int numtones = rx->m->numtones;
    double x, max = 0.0, avg = 0.0;
    int i, symbol = 0, burstcount = 0;
    for (i = 0; i < numtones; i++)
        avg += cabs_(in[i]);
    avg /= numtones;
    if (avg < 1e-20)
        avg = 1e-20;
    for (i = 0; i < numtones; i++) {
        x = cabs_(in[i]);
        if (x > max) {
            max = x;
            symbol = i;
        }
        if (x > 2.0 * avg)
            burstcount++;
    }
    rx->staticburst = (burstcount == numtones);
    if (!rx->staticburst)
        rx->afcmetric = 0.95 * rx->afcmetric + 0.05 * (2 * max / avg);
    else
        rx->afcmetric = 0.0;
    return symbol;
}

static void synchronize(fmfsk_rx *rx)
{
    /* mfsk::synchronize, mfsk.cxx:755-784 */
    const int symlen = rx->m->symlen;
    int i, j;
    double syn = -1;
    double val, max = 0.0;
    if (rx->currsymbol == rx->prev1symbol)
        return;
    if (rx->prev1symbol == rx->prev2symbol)
        return;
    j = (int)rx->pipeptr;
    for (i = 0; i < 2 * symlen; i++) {
        val = cabs_(rx->pipe[j].vector[rx->prev1symbol]);
        if (val > max) {
            max = val;
            syn = i;
        }
        j = (j + 1) % (2 * symlen);
    }
    syn = fdsp_movavg_run(&rx->syncfilter, syn);
    rx->synccounter += (int)floor((syn - symlen) / rx->m->numtones + 0.5);
}

static void afc(fmfsk_rx *rx)
{
    /* mfsk::afc, mfsk.cxx:792-831 (sigsearch never set without the waterfall) */
    const fmfsk_submode *m = rx->m;
    fdsp_cmpx z, prevvector;
    double f, f1;
    double ts = m->tonespacing / 4;
    if (rx->afcmetric < 3.0)
        return;
    if (rx->metric < rx->squelch)
        return;
    if (rx->currsymbol != rx->prev1symbol)
        return;
    if (rx->staticburst || !rx->afc_on)
        return;
    if (rx->pipeptr == 0)
        prevvector = rx->pipe[2 * m->symlen - 1].vector[rx->currsymbol];
    else
        prevvector = rx->pipe[rx->pipeptr - 1].vector[rx->currsymbol];
    z = cmul(conj_(prevvector), rx->currvector);
    f = atan2(z.im, z.re) * m->samplerate / MFSK_TWOPI;
    f1 = m->tonespacing * (m->basetone + rx->currsymbol);
    if (fabs(f1 - f) < ts) {
        rx->freqerr = fdsp_decayavg(rx->freqerr, (f1 - f), MFSK_AVG);
        set_freq(rx, rx->frequency - rx->freqerr);
    }
}

static void eval_s2n(fmfsk_rx *rx)
{
    /* mfsk::eval_s2n, mfsk.cxx:833-855 (the status text is left out) */
    rx->sig = cabs_(rx->pipe[rx->pipeptr].vector[rx->currsymbol]);
    rx->noise = (rx->m->numtones - 1) * cabs_(rx->pipe[rx->pipeptr].vector[rx->prev2symbol]);
    if (rx->noise > 0)
        rx->s2n = fdsp_decayavg(rx->s2n, rx->sig / rx->noise, 64);
    else
        rx->s2n = -20;
    rx->s2nvals[rx->s2nptr] = rx->s2n;
    ++rx->s2nptr;
    if (rx->s2nptr == 100)
        rx->s2nptr = 0;
}

static void tap_tone(fmfsk_rx *rx, int tone)
{
    rx->tones[(rx->tone_head + rx->tone_count) % MFSK_TAP_TONES] = tone;
    if (rx->tone_count < MFSK_TAP_TONES)
        rx->tone_count++;
    else
        rx->tone_head = (rx->tone_head + 1) % MFSK_TAP_TONES;
}

static void rx_sample(fmfsk_rx *rx, double x)
{
    /* one pass of the loop of mfsk::rx_process, mfsk.cxx:857-925 */
    const fmfsk_submode *m = rx->m;
    fdsp_cmpx z, *bins;
    /* create analytic signal */
    z.re = x;
    z.im = x;
    fdsp_fir_run(&rx->hbfilt, z, &z);
    /* shift in frequency to the base freq */
    z = mixer(rx, z, rx->frequency);
    /* bandpass filter around the shifted center frequency with required bandwidth */
    fdsp_fir_run(&rx->bpfilt, z, &z);
    /* copy current vector to the pipe */
    fdsp_sfft_run(&rx->binsfft, z, rx->pipe[rx->pipeptr].vector, 1);
    bins = rx->pipe[rx->pipeptr].vector;

    if (rx->rxstate == RX_STATE_PICTURE_START) {
        if (--rx->counter == 0) {
            rx->counter = rx->picturesize;
            rx->rxstate = RX_STATE_PICTURE;
        }
    }
    if (rx->rxstate == RX_STATE_PICTURE) {
        if (--rx->counter == 0) {
            rx->rxstate = RX_STATE_DATA;
            flush_partial_row(rx);
            rx->pic.active = 0;
            rx->pic.completed++;
            rx_init(rx);
        } else {
            recvpic(rx, z);
        }
        return;
    }

    if (--rx->synccounter <= 0) {
        rx->synccounter = m->symlen;
        rx->currsymbol = harddecode(rx, bins);
        rx->currvector = bins[rx->currsymbol];
        tap_tone(rx, rx->currsymbol);
        softdecode(rx, bins);
        /* symbol sync */
        synchronize(rx);
        /* frequency tracking */
        afc(rx);
        eval_s2n(rx);
        rx->prev2symbol = rx->prev1symbol;
        rx->prev2vector = rx->prev1vector;
        rx->prev1symbol = rx->currsymbol;
        rx->prev1vector = rx->currvector;
    }
    rx->pipeptr = (rx->pipeptr + 1) % (unsigned int)(2 * m->symlen);
}

/* ---------------------------------------------------------------------------------- */
/* public API                                                                          */
/* ---------------------------------------------------------------------------------- */

static int carrier_ok(const fmfsk_submode *m, double carrier)
{
    return carrier >= MFSK_LOW_CUTOFF + m->bandwidth / 2 &&
           carrier <= MFSK_HIGH_CUTOFF - m->bandwidth / 2;
}

static void rx_free(fmfsk_rx *rx)
{
    fdsp_fir_free(&rx->hbfilt);
    fdsp_fir_free(&rx->bpfilt);
    fdsp_sfft_free(&rx->binsfft);
    fdsp_viterbi_free(&rx->dec1);
    fdsp_viterbi_free(&rx->dec2);
    fdsp_interleave_free(&rx->rxinlv);
    fdsp_resampler_free(&rx->rs);
    free(rx->pipe);
    free(rx->picrow);
    free(rx->ring);
    free(rx);
}

static void rx_construct(fmfsk_rx *rx)
{
    /* the state mfsk::mfsk (mfsk.cxx:300-359) and mfsk::init (:125-145) leave behind */
    rx->phaseacc = 0;
    rx->pipeptr = 0;
    rx->metric = 0;
    rx->prev1symbol = rx->prev2symbol = 0;
    rx->currsymbol = 0;
    rx->symbolpair[0] = rx->symbolpair[1] = 0;
    rx->afcmetric = 0.0;
    rx->datashreg = 1;
    rx->symbolbit = 0;
    rx->prevz.re = rx->prevz.im = 0;
    rx->picf = 0;
    rx->currvector.re = rx->currvector.im = 0;
    rx->prev1vector = rx->prev2vector = rx->currvector;
    rx->s2nptr = 0;
    memset(rx->cwicounter, 0, sizeof rx->cwicounter);
    memset(&rx->pic, 0, sizeof rx->pic);
    rx->ring_head = rx->ring_count = 0;
    rx->tone_head = rx->tone_count = 0;
    rx->soft_head = rx->soft_count = 0;
    rx->dropped = 0;
    set_freq(rx, rx->carrier0);
    rx_init(rx);
}

fmfsk_rx *fmfsk_rx_create(const fmfsk_rx_config *cfg)
{
    const fmfsk_submode *m;
    fmfsk_rx *rx;
    double bw, cf, flo, fhi, basefreq;
    if (!cfg)
        return NULL;
    m = fmfsk_submode_find(cfg->submode);
    if (!m || !carrier_ok(m, cfg->carrier_hz) || cfg->squelch < 0 || cfg->squelch > 100 ||
        cfg->input_rate < 0 || cfg->input_rate > 192000)
        return NULL;
    rx = (fmfsk_rx *)calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    rx->m = m;
    rx->carrier0 = cfg->carrier_hz;
    rx->afc_on = cfg->afc ? 1 : 0;
    rx->squelch = cfg->squelch;
    rx->sql_on = cfg->squelch > 0;
    if (cfg->input_rate && cfg->input_rate != m->samplerate) {
        rx->resample = 1;
        if (fdsp_resampler_init(&rx->rs, cfg->input_rate, m->samplerate,
                                MFSK_RESAMPLE_TAPS) != 0) {
            rx_free(rx);
            return NULL;
        }
    }
    /* mfsk::mfsk, mfsk.cxx:297-333 */
    basefreq = 1.0 * m->samplerate * m->basetone / m->symlen;
    bw = (m->numtones - 1) * m->tonespacing;
    cf = basefreq + bw / 2.0;
    flo = (cf - bw / 2 - 2 * m->tonespacing) / m->samplerate;
    fhi = (cf + bw / 2 + 2 * m->tonespacing) / m->samplerate;
    rx->pipe = (mfsk_rxpipe *)calloc((size_t)(2 * m->symlen), sizeof(mfsk_rxpipe));
    rx->picrow = (unsigned char *)calloc(FMFSK_PIC_MAXW, 1);
    rx->ring = (unsigned char *)calloc((size_t)FMFSK_PIC_RING * FMFSK_PIC_MAXW, 1);
    if (!rx->pipe || !rx->picrow || !rx->ring ||
        fdsp_sfft_init(&rx->binsfft, m->symlen, m->basetone, m->basetone + m->numtones) ||
        fdsp_fir_init_hilbert(&rx->hbfilt, 37, 1) ||
        fdsp_movavg_init(&rx->syncfilter, 8) ||
        fdsp_viterbi_init(&rx->dec1, MFSK_K, MFSK_POLY1, MFSK_POLY2) ||
        fdsp_viterbi_init(&rx->dec2, MFSK_K, MFSK_POLY1, MFSK_POLY2) ||
        fdsp_viterbi_set_traceback(&rx->dec1, MFSK_TRACE) ||
        fdsp_viterbi_set_traceback(&rx->dec2, MFSK_TRACE) ||
        fdsp_viterbi_set_chunksize(&rx->dec1, 1) ||
        fdsp_viterbi_set_chunksize(&rx->dec2, 1) ||
        fdsp_interleave_init(&rx->rxinlv, m->symbits, m->depth, FDSP_INTERLEAVE_REV) ||
        fdsp_fir_init_bandpass(&rx->bpfilt, 127, 1, flo, fhi)) {
        rx_free(rx);
        return NULL;
    }
    rx_construct(rx);
    return rx;
}

void fmfsk_rx_destroy(fmfsk_rx *rx)
{
    if (rx)
        rx_free(rx);
}

void fmfsk_rx_reset(fmfsk_rx *rx)
{
    const fmfsk_submode *m = rx->m;
    double basefreq = 1.0 * m->samplerate * m->basetone / m->symlen;
    double bw = (m->numtones - 1) * m->tonespacing, cf = basefreq + bw / 2.0;
    fdsp_fir_free(&rx->hbfilt);
    fdsp_fir_free(&rx->bpfilt);
    fdsp_fir_init_hilbert(&rx->hbfilt, 37, 1);
    fdsp_fir_init_bandpass(&rx->bpfilt, 127, 1, (cf - bw / 2 - 2 * m->tonespacing) / m->samplerate,
                           (cf + bw / 2 + 2 * m->tonespacing) / m->samplerate);
    fdsp_sfft_reset(&rx->binsfft);
    fdsp_viterbi_reset(&rx->dec1);
    fdsp_viterbi_reset(&rx->dec2);
    fdsp_interleave_flush(&rx->rxinlv);
    fdsp_movavg_reset(&rx->syncfilter);
    if (rx->resample)
        fdsp_resampler_reset(&rx->rs);
    rx_construct(rx);
}

size_t fmfsk_rx_process(fmfsk_rx *rx, const double *x, size_t n, unsigned char *text,
                        size_t max_text)
{
    size_t i;
    rx->out = text;
    rx->max = max_text;
    rx->n = 0;
    if (!rx->resample) {
        for (i = 0; i < n; i++)
            rx_sample(rx, x[i]);
    } else {
        /* modem-rate samples in blocks that fit rsbuf */
        size_t chunk = sizeof rx->rsbuf / sizeof rx->rsbuf[0];
        size_t step = (chunk - 2) * (size_t)rx->rs.down / (size_t)rx->rs.up;
        if (step < 1)
            step = 1;
        for (i = 0; i < n; i += step) {
            size_t m = n - i < step ? n - i : step, k, j;
            k = fdsp_resampler_run(&rx->rs, x + i, m, rx->rsbuf);
            for (j = 0; j < k; j++)
                rx_sample(rx, rx->rsbuf[j]);
        }
    }
    rx->out = NULL;
    rx->max = 0;
    return rx->n;
}

double fmfsk_rx_snr_db(const fmfsk_rx *rx)
{
    /* the displayed value of eval_s2n, mfsk.cxx:847-854 */
    double maxs2n = -20;
    int n;
    for (n = 0; n < 100; n++)
        if (rx->s2nvals[n] > maxs2n)
            maxs2n = rx->s2nvals[n];
    return maxs2n > 0 ? 20 * log10(maxs2n) : -99.0;
}

double fmfsk_rx_metric(const fmfsk_rx *rx)
{
    return rx->metric;
}

double fmfsk_rx_offset_hz(const fmfsk_rx *rx)
{
    return rx->frequency - rx->carrier0;
}

double fmfsk_rx_carrier_hz(const fmfsk_rx *rx)
{
    return rx->frequency;
}

void fmfsk_rx_set_carrier(fmfsk_rx *rx, double carrier_hz)
{
    set_freq(rx, carrier_hz);
    rx->carrier0 = rx->frequency;
}

void fmfsk_rx_set_afc(fmfsk_rx *rx, int on)
{
    rx->afc_on = on ? 1 : 0;
}

void fmfsk_rx_set_squelch(fmfsk_rx *rx, double level)
{
    rx->squelch = fmfsk__clamp(level, 0, 100);
    rx->sql_on = rx->squelch > 0;
}

size_t fmfsk_rx_dropped(const fmfsk_rx *rx)
{
    return rx->dropped;
}

void fmfsk_rx_picture_info(const fmfsk_rx *rx, fmfsk_picture_info *out)
{
    *out = rx->pic;
}

int fmfsk_rx_poll_picture(fmfsk_rx *rx, fmfsk_picture_row *row, unsigned char *pixels,
                          size_t max)
{
    size_t n;
    if (!rx->ring_count)
        return 0;
    *row = rx->ringhdr[rx->ring_head];
    n = (size_t)row->n < max ? (size_t)row->n : max;
    if (pixels && n)
        memcpy(pixels, rx->ring + rx->ring_head * FMFSK_PIC_MAXW, n);
    row->n = (int)n;
    rx->ring_head = (rx->ring_head + 1) % FMFSK_PIC_RING;
    rx->ring_count--;
    return 1;
}

size_t fmfsk_rx_take_tones(fmfsk_rx *rx, int *out, size_t max)
{
    size_t k = 0;
    while (rx->tone_count && k < max) {
        out[k++] = rx->tones[rx->tone_head];
        rx->tone_head = (rx->tone_head + 1) % MFSK_TAP_TONES;
        rx->tone_count--;
    }
    return k;
}

size_t fmfsk_rx_take_softbits(fmfsk_rx *rx, unsigned char *out, size_t max)
{
    size_t k = 0;
    while (rx->soft_count && k < max) {
        out[k++] = rx->soft[rx->soft_head];
        rx->soft_head = (rx->soft_head + 1) % MFSK_TAP_SOFT;
        rx->soft_count--;
    }
    return k;
}
