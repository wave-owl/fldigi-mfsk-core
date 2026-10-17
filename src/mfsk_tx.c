/*
 * fldigi-mfsk-core: the MFSK transmitter of fldigi 4.2.13 (src/mfsk/mfsk.cxx) with the
 * soft start/stop of modem::ModulateXmtr (src/trx/modem.cxx).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 src/mfsk/mfsk.cxx (c) 2006-2009 Dave Freese W1HKJ, John
 * Douyere VK2ETA, and src/trx/modem.cxx (c) 2006-2010 Dave Freese W1HKJ, based on gmfsk
 * (c) 2001-2003 Tomi Manninen OH2BNS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * fldigi's defaults: softMFSK on, SoftStart 100 ms, TxOffset 0, reverse off, the TX band-pass
 * xmt_filter off (mfsk.cxx:66). The output is what fldigi hands to the sound card before
 * the TX level. Keyboard text cannot start a picture (fmfsk_tx_put); the picture
 * transmitter exists only as the internal test helper fmfsk__tx_picture.
 */
#include "mfsk_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------- */
/* modem::ModulateXmtr soft start/stop, trx/modem.cxx:611-637                          */
/* ---------------------------------------------------------------------------------- */

static void transmit(fmfsk_tx *tx, double *buf, int len)
{
    /* mfsk::transmit (mfsk.cxx:932-938, xmt_filter off) -> modem::ModulateXmtr */
    int i;
    const int samplerate = tx->m->samplerate, symlen = tx->m->symlen;
    int num = ((samplerate * MFSK_SOFTSTART_MS / 1000) / symlen + 1) * symlen;
    if (tx->sig_start && tx->sig_smpl < num) {
        for (i = 0; i < len; i++) {
            double coeff = (0.5 * (1.0 - cos(M_PI * tx->sig_smpl / num)));
            buf[i] *= coeff;
            ++tx->sig_smpl;
            if (tx->sig_smpl >= num) {
                tx->sig_smpl = 0;
                tx->sig_start = 0;
                break;
            }
        }
    }
    if (tx->sig_stop && tx->sig_smpl < num) {
        for (i = 0; i < len; i++) {
            double coeff = 1.0 - (0.5 * (1.0 - cos(M_PI * tx->sig_smpl / num)));
            buf[i] *= coeff;
            ++tx->sig_smpl;
            if (tx->sig_smpl >= num) {
                tx->sig_smpl = 0;
                tx->sig_stop = 0;
                break;
            }
        }
    }
    tx->n += (size_t)len;
}

static double *outbuf(fmfsk_tx *tx, int len)
{
    /* the next len samples of this call (bounded by fmfsk_tx_bound) */
    if (tx->n + (size_t)len > tx->native_cap)
        return NULL;
    return tx->native + tx->n;
}

/* ---------------------------------------------------------------------------------- */
/* sendsymbol ... flushtx, clearbits: mfsk.cxx:941-1007, 1038-1077                     */
/* ---------------------------------------------------------------------------------- */

static void sendsymbol(fmfsk_tx *tx, int sym)
{
    /* mfsk::sendsymbol, mfsk.cxx:941-959 */
    const fmfsk_submode *m = tx->m;
    double f, phaseincr;
    double *buf = outbuf(tx, m->symlen);
    int i;
    if (!buf)
        return;
    f = tx->frequency - m->bandwidth / 2;
    sym = fdsp_binary_code((uint8_t)(sym & (m->numtones - 1)));   /* grayencode */
    phaseincr = MFSK_TWOPI * (f + sym * m->tonespacing) / m->samplerate;
    for (i = 0; i < m->symlen; i++) {
        buf[i] = cos(tx->phaseacc);
        tx->phaseacc -= phaseincr;
        if (tx->phaseacc < 0)
            tx->phaseacc += MFSK_TWOPI;
    }
    transmit(tx, buf, m->symlen);
}

static void sendbit(fmfsk_tx *tx, int bit)
{
    /* mfsk::sendbit, mfsk.cxx:961-975 */
    int data = fdsp_encoder_encode(&tx->enc, bit);
    int i;
    for (i = 0; i < 2; i++) {
        tx->bitshreg = (tx->bitshreg << 1) | ((data >> i) & 1);
        tx->bitstate++;
        if (tx->bitstate == tx->m->symbits) {
            fdsp_interleave_bits(&tx->txinlv, &tx->bitshreg);
            sendsymbol(tx, (int)tx->bitshreg);
            tx->bitstate = 0;
            tx->bitshreg = 0;
        }
    }
}

static void sendchar(fmfsk_tx *tx, unsigned char c)
{
    /* mfsk::sendchar, mfsk.cxx:977-983 */
    const char *code = fdsp_mfsk_varienc(c);
    while (*code)
        sendbit(tx, *code++ - '0');
}

static void sendidle(fmfsk_tx *tx)
{
    /* mfsk::sendidle, mfsk.cxx:985-993 */
    int i;
    sendchar(tx, 0);
    sendbit(tx, 1);
    /* extended zero bit stream */
    for (i = 0; i < 32; i++)
        sendbit(tx, 0);
}

static void flushtx(fmfsk_tx *tx, int nbits)
{
    /* mfsk::flushtx, mfsk.cxx:995-1007 */
    int i;
    /* flush the varicode decoder at the other end */
    sendbit(tx, 1);
    /* flush the convolutional encoder and interleaver */
    for (i = 0; i < nbits; i++)
        sendbit(tx, 0);
    tx->bitstate = 0;
}

static void clearbits(fmfsk_tx *tx)
{
    /* mfsk::clearbits, mfsk.cxx:1061-1077 */
    int data = fdsp_encoder_encode(&tx->enc, 0);
    int k, i;
    for (k = 0; k < tx->m->preamble; k++) {
        for (i = 0; i < 2; i++) {
            tx->bitshreg = (tx->bitshreg << 1) | ((data >> i) & 1);
            tx->bitstate++;
            if (tx->bitstate == tx->m->symbits) {
                fdsp_interleave_bits(&tx->txinlv, &tx->bitshreg);
                tx->bitstate = 0;
                tx->bitshreg = 0;
            }
        }
    }
}

static int softstop_pairs(const fmfsk_submode *m)
{
    /* (samplerate * SoftStart / 1000 / symlen) + 1, mfsk.cxx:1159 */
    return (m->samplerate * MFSK_SOFTSTART_MS / 1000 / m->symlen) + 1;
}

/* ---------------------------------------------------------------------------------- */
/* picture transmitter (test helper only): sendpic, flush_xmt_filter, mfsk.cxx:1009-1056 */
/* ---------------------------------------------------------------------------------- */

static void sendpic(fmfsk_tx *tx, const unsigned char *data, int len)
{
    /* mfsk::sendpic, mfsk.cxx:1009-1034 (reverse off) */
    const fmfsk_submode *m = tx->m;
    double *ptr = outbuf(tx, MFSK_TXSPP * len), *start = ptr;
    double f;
    int i, j;
    if (!ptr)
        return;
    for (i = 0; i < len; i++) {
        f = tx->frequency + m->bandwidth * (data[i] - 128) / 256.0;
        for (j = 0; j < MFSK_TXSPP; j++) {
            *ptr++ = cos(tx->phaseacc);
            tx->phaseacc += MFSK_TWOPI * f / m->samplerate;
            if (tx->phaseacc > MFSK_TWOPI)
                tx->phaseacc -= MFSK_TWOPI;
        }
    }
    transmit(tx, start, MFSK_TXSPP * len);
}

static void send_prologue(fmfsk_tx *tx)
{
    /* send_prologue -> flush_xmt_filter(tracepair.delay), mfsk.cxx:1038-1054 */
    const fmfsk_submode *m = tx->m;
    double f1 = tx->frequency - m->bandwidth / 2.0;
    double *buf = outbuf(tx, MFSK_TRACE_DELAY);
    int i;
    if (!buf)
        return;
    for (i = 0; i < MFSK_TRACE_DELAY; i++) {
        buf[i] = cos(tx->phaseacc);
        tx->phaseacc += MFSK_TWOPI * f1 / m->samplerate;
        if (tx->phaseacc > MFSK_TWOPI)
            tx->phaseacc -= MFSK_TWOPI;
    }
    transmit(tx, buf, MFSK_TRACE_DELAY);
}

/* ---------------------------------------------------------------------------------- */
/* public API                                                                          */
/* ---------------------------------------------------------------------------------- */

static size_t max_code_bits(void)
{
    size_t mx = 0;
    int c;
    for (c = 0; c < 256; c++) {
        size_t l = strlen(fdsp_mfsk_varienc(c));
        if (l > mx)
            mx = l;
    }
    return mx;
}

static size_t bits_to_samples(const fmfsk_submode *m, size_t bits)
{
    /* every bit gives two code bits; up to symbits - 1 code bits may wait from before */
    return ((2 * bits + (size_t)m->symbits - 1) / (size_t)m->symbits + 1) * (size_t)m->symlen;
}

fmfsk_tx *fmfsk_tx_create(const fmfsk_tx_config *cfg)
{
    const fmfsk_submode *m;
    fmfsk_tx *tx;
    size_t code, bits, b;
    if (!cfg)
        return NULL;
    m = fmfsk_submode_find(cfg->submode);
    if (!m || cfg->carrier_hz < MFSK_LOW_CUTOFF + m->bandwidth / 2 ||
        cfg->carrier_hz > MFSK_HIGH_CUTOFF - m->bandwidth / 2 || cfg->output_rate < 0 ||
        cfg->output_rate > 192000)
        return NULL;
    tx = (fmfsk_tx *)calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->m = m;
    tx->frequency = cfg->carrier_hz;
    tx->soft = cfg->no_soft_edges ? 0 : 1;
    fmfsk__clear_picture_header(tx->picheader);
    code = max_code_bits();
    /* the largest call: begin (preamble / 3 + CR STX CR) or end (CR EOT CR, flush of
     * preamble bits, soft-stop bits) */
    bits = (size_t)m->preamble / 3 + 3 * code;
    b = 3 * code + 1 + (size_t)m->preamble + 2 * (size_t)softstop_pairs(m);
    if (b > bits)
        bits = b;
    if (code + 33 > bits)
        bits = code + 33;
    tx->native_cap = bits_to_samples(m, bits);
    tx->native = (double *)malloc(tx->native_cap * sizeof(double));
    if (!tx->native || fdsp_encoder_init(&tx->enc, MFSK_K, MFSK_POLY1, MFSK_POLY2) ||
        fdsp_interleave_init(&tx->txinlv, m->symbits, m->depth, FDSP_INTERLEAVE_FWD)) {
        fmfsk_tx_destroy(tx);
        return NULL;
    }
    tx->bound = tx->native_cap;
    if (cfg->output_rate && cfg->output_rate != m->samplerate) {
        tx->resample = 1;
        if (fdsp_resampler_init(&tx->rs, m->samplerate, cfg->output_rate,
                                MFSK_RESAMPLE_TAPS) != 0) {
            fmfsk_tx_destroy(tx);
            return NULL;
        }
        tx->bound = fdsp_resampler_max_out(&tx->rs, tx->native_cap);
    }
    return tx;
}

void fmfsk_tx_destroy(fmfsk_tx *tx)
{
    if (!tx)
        return;
    fdsp_encoder_free(&tx->enc);
    fdsp_interleave_free(&tx->txinlv);
    fdsp_resampler_free(&tx->rs);
    free(tx->native);
    free(tx);
}

size_t fmfsk_tx_bound(const fmfsk_tx *tx)
{
    return tx->bound;
}

size_t fmfsk_tx_refused(const fmfsk_tx *tx)
{
    return tx->refused;
}

static int call_start(fmfsk_tx *tx, double *out, size_t out_max, int need_state)
{
    if (!out || out_max < tx->bound || tx->state != need_state)
        return 0;
    tx->out = out;
    tx->max = out_max;
    tx->n = 0;
    return 1;
}

static size_t call_end(fmfsk_tx *tx)
{
    size_t n = tx->n;
    if (tx->resample)
        n = fdsp_resampler_run(&tx->rs, tx->native, tx->n, tx->out);
    else
        memcpy(tx->out, tx->native, tx->n * sizeof(double));
    tx->out = NULL;
    tx->n = 0;
    return n;
}

size_t fmfsk_tx_begin(fmfsk_tx *tx, double *out, size_t out_max)
{
    int i;
    if (!call_start(tx, out, out_max, 0))
        return 0;
    /* TX_STATE_PREAMBLE, mfsk.cxx:1105-1113 */
    clearbits(tx);
    if (tx->soft)
        tx->sig_start = 1;
    if (tx->m->fldigi_id != MFSK_ID_MFSK64L && tx->m->fldigi_id != MFSK_ID_MFSK128L)
        for (i = 0; i < tx->m->preamble / 3; i++)
            sendbit(tx, 0);
    /* TX_STATE_START, mfsk.cxx:1114-1119 */
    sendchar(tx, '\r');
    sendchar(tx, 2);   /* STX */
    sendchar(tx, '\r');
    tx->state = 1;
    return call_end(tx);
}

size_t fmfsk_tx_put(fmfsk_tx *tx, unsigned char c, double *out, size_t out_max)
{
    char window[MFSK_PICHEADER];
    if (!call_start(tx, out, out_max, 1))
        return 0;
    /* INVARIANT(INV-832 of wave-owl): keyboard text never starts a picture: 0x05 is
     * fldigi's picture trigger (mfsk.cxx:1130), and a byte that completes a header that
     * check_picture_header (mfsk.cxx:372-428) accepts would switch every fldigi receiver
     * into picture mode -- also one that joined late and has only part of the text. */
    memcpy(window, tx->picheader, sizeof window);
    fmfsk__push_picture_header(window, c);
    if (c == 0x05 || fmfsk__any_picture_header(window)) {
        tx->refused++;
        tx->out = NULL;
        return 0;
    }
    memcpy(tx->picheader, window, sizeof window);
    /* TX_STATE_DATA with a character, mfsk.cxx:1121-1149 */
    sendchar(tx, c);
    return call_end(tx);
}

size_t fmfsk_tx_idle(fmfsk_tx *tx, double *out, size_t out_max)
{
    if (!call_start(tx, out, out_max, 1))
        return 0;
    sendidle(tx);   /* GET_TX_CHAR_NODATA, mfsk.cxx:1145-1146 */
    return call_end(tx);
}

size_t fmfsk_tx_end(fmfsk_tx *tx, double *out, size_t out_max)
{
    int n;
    if (!call_start(tx, out, out_max, 1))
        return 0;
    /* TX_STATE_FLUSH, mfsk.cxx:1151-1166 */
    sendchar(tx, '\r');
    sendchar(tx, 4);   /* EOT */
    sendchar(tx, '\r');
    flushtx(tx, tx->m->preamble);
    if (tx->soft)
        tx->sig_stop = 1;
    for (n = 0; n < softstop_pairs(tx->m); n++) {
        sendbit(tx, 0);
        sendbit(tx, 0);
    }
    tx->state = 2;
    return call_end(tx);
}

size_t fmfsk__tx_picture_bound(const fmfsk_tx *tx, const char *header, size_t npixels)
{
    size_t bits = strlen(header) * max_code_bits() + 2 * (size_t)tx->m->preamble + 2;
    size_t native = bits_to_samples(tx->m, bits) + MFSK_TRACE_DELAY + MFSK_TXSPP * npixels;
    return tx->resample ? fdsp_resampler_max_out(&tx->rs, native) : native;
}

size_t fmfsk__tx_picture(fmfsk_tx *tx, const char *header, const unsigned char *pixels,
                         size_t npixels, double *out, size_t out_max)
{
    size_t i, native = fmfsk__tx_picture_bound(tx, header, npixels);
    double *save = tx->native;
    size_t save_cap = tx->native_cap, n;
    if (tx->state != 1 || out_max < native)
        return 0;
    tx->native = (double *)malloc(native * sizeof(double));
    tx->native_cap = native;
    tx->out = out;
    tx->n = 0;
    /* TX_STATE_DATA with 0x05 (mfsk.cxx:1130-1139): the header, flushtx(preamble) */
    for (i = 0; header[i]; i++)
        sendchar(tx, (unsigned char)header[i]);
    flushtx(tx, tx->m->preamble);
    /* TX_STATE_PICTURE_START, TX_STATE_PICTURE (mfsk.cxx:1168-1207) */
    send_prologue(tx);
    for (i = 0; i < npixels; i += 128)
        sendpic(tx, pixels + i, (int)(npixels - i < 128 ? npixels - i : 128));
    flushtx(tx, tx->m->preamble);
    n = call_end(tx);
    free(tx->native);
    tx->native = save;
    tx->native_cap = save_cap;
    return n;
}
