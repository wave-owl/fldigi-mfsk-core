/*
 * fldigi-mfsk-core unit tests: submode table, argument checks, TX -> RX loopback for every
 * submode (at the modem rate and through the resampler), chunking, picture header refusal,
 * the picture ring, AFC, squelch, s/n, reset and instance independence.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include "test_util.h"
#include "picture_util.h"

static const char *TEXT = "CQ CQ DE DG6FL DG6FL K";

static int framed(const unsigned char *out, size_t n, const char *text)
{
    char want[256];
    snprintf(want, sizeof want, "\r\002\r%s\r\004\r", text);
    return contains(out, n, want);
}

static double *pad(const double *x, size_t n, int rate, size_t *nout)
{
    size_t lead = (size_t)rate, tail = 4 * (size_t)rate, i;
    double *y = (double *)calloc(lead + n + tail, sizeof(double));
    for (i = 0; i < n; i++)
        y[lead + i] = 0.25 * x[i];
    *nout = lead + n + tail;
    return y;
}

static void test_table(void)
{
    int i;
    CHECK(strcmp(fmfsk_version(), "fldigi-mfsk-core " FMFSK_VERSION_STRING
                                  " contract=mfsk-c1 abi=1 tx=1") == 0, "version %s",
          fmfsk_version());
    CHECK(fmfsk_abi() == 1, "abi");
    CHECK(fmfsk_submode_count() == 11, "count %d", fmfsk_submode_count());
    CHECK(fmfsk_submode_at(-1) == NULL && fmfsk_submode_at(11) == NULL, "at bounds");
    CHECK(fmfsk_submode_find("mfsk16") == fmfsk_submode_find("MFSK16"), "find case");
    CHECK(fmfsk_submode_find("MFSK17") == NULL && fmfsk_submode_find(NULL) == NULL, "find");
    for (i = 0; i < fmfsk_submode_count(); i++) {
        const fmfsk_submode *m = fmfsk_submode_at(i);
        CHECK(m->numtones == 1 << m->symbits || (m->symbits == 5 && m->numtones == 32),
              "%s tones", m->name);
        CHECK(fabs(m->tonespacing * m->symlen - m->samplerate) < 1e-9, "%s spacing", m->name);
        CHECK(fabs(m->bandwidth - (m->numtones - 1) * m->tonespacing) < 1e-9, "%s bw",
              m->name);
    }
    CHECK(fmfsk_submode_find("MFSK128L")->depth == 800 &&
          fmfsk_submode_find("MFSK128L")->preamble == 5000, "MFSK128L");
    CHECK(fmfsk_submode_find("MFSK31")->symbits == 3, "MFSK31");
}

static void test_arguments(void)
{
    fmfsk_rx_config rc;
    fmfsk_tx_config tc;
    memset(&rc, 0, sizeof rc);
    rc.submode = "MFSK16";
    rc.carrier_hz = 1500;
    CHECK(fmfsk_rx_create(NULL) == NULL, "rx NULL");
    rc.carrier_hz = 100;   /* below bandwidth / 2 */
    CHECK(fmfsk_rx_create(&rc) == NULL, "rx carrier low");
    rc.carrier_hz = 2950;  /* above 3000 - bandwidth / 2 */
    CHECK(fmfsk_rx_create(&rc) == NULL, "rx carrier high");
    rc.carrier_hz = 1500;
    rc.squelch = 101;
    CHECK(fmfsk_rx_create(&rc) == NULL, "rx squelch");
    rc.squelch = 0;
    rc.submode = "PSK31";
    CHECK(fmfsk_rx_create(&rc) == NULL, "rx submode");
    memset(&tc, 0, sizeof tc);
    tc.submode = "MFSK128";
    tc.carrier_hz = 2500;  /* 2500 + 937.5 > 3000 */
    CHECK(fmfsk_tx_create(&tc) == NULL, "tx carrier");
    tc.carrier_hz = 1500;
    {
        fmfsk_tx *tx = fmfsk_tx_create(&tc);
        double small[16];
        CHECK(tx != NULL, "tx create");
        CHECK(fmfsk_tx_put(tx, 'A', small, 16) == 0, "put before begin / short buffer");
        CHECK(fmfsk_tx_begin(tx, small, 16) == 0, "short buffer");
        fmfsk_tx_destroy(tx);
    }
}

/* AC-836.1: every submode decodes its own transmission bit for bit, at the modem rate and
 * through the resampler (8 kHz station rate for the 11025 Hz submodes, 48 kHz). */
static void test_loopback(void)
{
    int i, r;
    for (i = 0; i < fmfsk_submode_count(); i++) {
        const fmfsk_submode *m = fmfsk_submode_at(i);
        static const int rates[] = {0, 8000, 48000};
        for (r = 0; r < 3; r++) {
            size_t n, nx, got;
            int rate = rates[r] ? rates[r] : m->samplerate;
            double *audio, *x;
            unsigned char *out;
            fmfsk_rx *rx;
            if (r == 2 && m->symlen > 512)
                continue;   /* 48 kHz: the faster submodes are enough */
            audio = render_over(m->name, TEXT, 0, 0, 1500.0, rates[r], &n);
            CHECK(audio != NULL, "%s render", m->name);
            if (!audio)
                continue;
            x = pad(audio, n, rate, &nx);
            rx = make_rx(m->name, 1500.0, rates[r], 0);
            CHECK(rx != NULL, "%s rx", m->name);
            out = decode(rx, x, nx, 1000, &got);
            CHECK(framed(out, got, TEXT), "%s loopback at %d Hz: '%s'", m->name, rate, out);
            free(out);
            free(x);
            free(audio);
            fmfsk_rx_destroy(rx);
        }
    }
}

/* AC-830.3: the output does not depend on the chunking. */
static void test_chunking(void)
{
    size_t n, nx, g1, g2, off;
    double *audio = render_over("MFSK16", TEXT, 2, 2, 1500.0, 0, &n);
    double *x = pad(audio, n, 8000, &nx);
    fmfsk_rx *a = make_rx("MFSK16", 1500.0, 0, 1), *b = make_rx("MFSK16", 1500.0, 0, 1);
    unsigned char *o1 = decode(a, x, nx, nx, &g1);
    unsigned char *o2 = (unsigned char *)malloc(nx / 8 + 4096);
    int t1[1024], t2[1024];
    size_t nt1, nt2;
    uint32_t s = 12345;
    g2 = 0;
    for (off = 0; off < nx;) {
        size_t m;
        s = s * 1664525u + 1013904223u;
        m = 1 + s % 4000;
        if (m > nx - off)
            m = nx - off;
        g2 += fmfsk_rx_process(b, x + off, m, o2 + g2, 4096);
        off += m;
    }
    CHECK(g1 == g2 && memcmp(o1, o2, g1) == 0, "chunking changes the text");
    nt1 = fmfsk_rx_take_tones(a, t1, 1024);
    nt2 = fmfsk_rx_take_tones(b, t2, 1024);
    CHECK(nt1 == nt2 && memcmp(t1, t2, nt1 * sizeof(int)) == 0, "chunking changes tones");
    CHECK(framed(o1, g1, TEXT), "chunk text");
    free(o1);
    free(o2);
    free(x);
    free(audio);
    fmfsk_rx_destroy(a);
    fmfsk_rx_destroy(b);
}

/* AC-834.2: keyboard text never starts a picture. */
static void test_picture_refusal(void)
{
    fmfsk_tx_config c;
    fmfsk_tx *tx;
    sbuf b = {NULL, 0, 0};
    double *tmp, *x;
    size_t bound, k, nx, got;
    const char *typed = "Pic:64x64; and Pic:10x10Cp4; done";
    const char *p;
    unsigned char *out;
    fmfsk_rx *rx;
    fmfsk_picture_info info;
    memset(&c, 0, sizeof c);
    c.submode = "MFSK32";
    c.carrier_hz = 1500;
    tx = fmfsk_tx_create(&c);
    bound = fmfsk_tx_bound(tx);
    tmp = (double *)malloc(bound * sizeof(double));
    sbuf_add(&b, tmp, fmfsk_tx_begin(tx, tmp, bound));
    CHECK(fmfsk_tx_put(tx, 0x05, tmp, bound) == 0, "0x05 accepted");
    for (p = typed; *p; p++) {
        k = fmfsk_tx_put(tx, (unsigned char)*p, tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    sbuf_add(&b, tmp, fmfsk_tx_end(tx, tmp, bound));
    CHECK(fmfsk_tx_refused(tx) == 3, "refused %zu", fmfsk_tx_refused(tx));   /* 0x05, ; ; */
    x = pad(b.x, b.n, 8000, &nx);
    rx = make_rx("MFSK32", 1500.0, 0, 0);
    out = decode(rx, x, nx, 512, &got);
    fmfsk_rx_picture_info(rx, &info);
    CHECK(info.started == 0, "a picture started");
    CHECK(contains(out, got, "Pic:64x64 and Pic:10x10Cp4 done"), "text '%s'", out);
    free(out);
    free(x);
    free(tmp);
    free(b.x);
    fmfsk_tx_destroy(tx);
    fmfsk_rx_destroy(rx);
}

/* R-837.1: unpolled rows are dropped oldest first; a bounded ring. */
static void test_picture_ring(void)
{
    size_t n, nx, i, got, rows = 0;
    const int h = 16;   /* 48 colour rows > FMFSK_PIC_RING */
    double *audio = render_picture_over_wh("MFSK128", 1, PIC_W, h, &n), *x;
    unsigned char *out, line[FMFSK_PIC_MAXW];
    fmfsk_rx *rx = make_rx("MFSK128", 1500.0, 0, 0);
    fmfsk_picture_info info;
    fmfsk_picture_row row;
    int last = -1, ok = 1;
    x = pad(audio, n, 8000, &nx);
    for (i = 0; i < nx; i++)
        x[i] *= 4;   /* pad() scales by 0.25 */
    out = decode(rx, x, nx, 777, &got);
    fmfsk_rx_picture_info(rx, &info);
    CHECK(info.started == 1 && info.completed == 1, "picture %d/%d", info.started,
          info.completed);
    CHECK(info.rows_dropped == (size_t)h * 3 - FMFSK_PIC_RING, "dropped %zu",
          info.rows_dropped);
    while (fmfsk_rx_poll_picture(rx, &row, line, sizeof line)) {
        int seq = row.row * 3 + row.channel;
        ok &= seq > last;
        last = seq;
        rows++;
    }
    CHECK(ok && rows == FMFSK_PIC_RING && last == h * 3 - 1, "rows %zu in order %d", rows, ok);
    CHECK(contains(out, got, PIC_AFTER), "text after the picture");
    free(out);
    free(x);
    free(audio);
    fmfsk_rx_destroy(rx);
}

static void test_afc_snr_squelch(void)
{
    size_t n, nx, got;
    double *audio = render_over("MFSK16", TEXT, 0, 0, 1503.0, 0, &n), *x;
    unsigned char *out;
    fmfsk_rx *rx = make_rx("MFSK16", 1500.0, 0, 1);
    unsigned char *rest;
    size_t grest, sig;
    x = pad(audio, n, 8000, &nx);
    sig = (size_t)8000 + n - 2000;   /* up to the end of the over */
    out = decode(rx, x, sig, 512, &got);
    CHECK(fabs(fmfsk_rx_offset_hz(rx) - 3.0) < 0.6, "AFC offset %g",
          fmfsk_rx_offset_hz(rx));
    CHECK(fmfsk_rx_metric(rx) > 50, "metric %g", fmfsk_rx_metric(rx));
    rest = decode(rx, x + sig, nx - sig, 512, &grest);
    memcpy(out + got, rest, grest);
    got += grest;
    CHECK(framed(out, got, TEXT), "AFC text");
    free(rest);
    free(out);
    /* fldigi's s/n display (decayavg from -20 in silence, so measured without the lead) */
    fmfsk_rx_reset(rx);
    free(decode(rx, audio, n, 512, &grest));
    CHECK(fmfsk_rx_snr_db(rx) > 5, "s/n %g", fmfsk_rx_snr_db(rx));
    /* squelch at 100: nothing gets through */
    fmfsk_rx_reset(rx);
    fmfsk_rx_set_squelch(rx, 100);
    out = decode(rx, x, nx, 512, &got);
    CHECK(got == 0, "squelch let %zu bytes through", got);
    free(out);
    /* reset restores the state after create */
    fmfsk_rx_set_squelch(rx, 0);
    fmfsk_rx_set_afc(rx, 0);
    fmfsk_rx_set_carrier(rx, 1503.0);
    out = decode(rx, x, nx, 512, &got);
    CHECK(framed(out, got, TEXT) && fabs(fmfsk_rx_offset_hz(rx)) < 1e-9, "retuned");
    free(out);
    free(x);
    free(audio);
    fmfsk_rx_destroy(rx);
}

/* Instances share nothing (fldigi's statics are per instance here). */
static void test_instances(void)
{
    size_t n, nx, off, g1 = 0, g2 = 0, ga;
    double *audio = render_over("MFSK31", TEXT, 0, 0, 1500.0, 0, &n);
    double *x = pad(audio, n, 8000, &nx);
    fmfsk_rx *a = make_rx("MFSK31", 1500.0, 0, 0), *b = make_rx("MFSK31", 1500.0, 0, 0);
    fmfsk_rx *alone = make_rx("MFSK31", 1500.0, 0, 0);
    unsigned char *o1 = (unsigned char *)malloc(nx), *o2 = (unsigned char *)malloc(nx), *oa;
    double *noise = (double *)malloc(nx * sizeof(double));
    uint32_t s = 99;
    for (off = 0; off < nx; off++) {
        s = s * 1664525u + 1013904223u;
        noise[off] = ((double)s / 4294967296.0 - 0.5) * 0.3;
    }
    for (off = 0; off < nx; off += 300) {
        size_t m = nx - off < 300 ? nx - off : 300;
        g1 += fmfsk_rx_process(a, x + off, m, o1 + g1, nx - g1);
        g2 += fmfsk_rx_process(b, noise + off, m, o2 + g2, nx - g2);
    }
    oa = decode(alone, x, nx, 4096, &ga);
    CHECK(ga == g1 && memcmp(oa, o1, ga) == 0, "instances interfere");
    free(o1);
    free(o2);
    free(oa);
    free(noise);
    free(x);
    free(audio);
    fmfsk_rx_destroy(a);
    fmfsk_rx_destroy(b);
    fmfsk_rx_destroy(alone);
}

/* Idle steps between keyboard characters decode as nothing (sendidle sends NUL). */
static void test_idle(void)
{
    size_t n, nx, got;
    double *audio = render_over("MFSK64", TEXT, 5, 3, 1500.0, 0, &n);
    double *x = pad(audio, n, 8000, &nx);
    fmfsk_rx *rx = make_rx("MFSK64", 1500.0, 0, 0);
    unsigned char *out = decode(rx, x, nx, 512, &got);
    CHECK(framed(out, got, TEXT), "idle text '%s'", out);
    free(out);
    free(x);
    free(audio);
    fmfsk_rx_destroy(rx);
}

int main(void)
{
    test_table();
    test_arguments();
    test_loopback();
    test_chunking();
    test_picture_refusal();
    test_picture_ring();
    test_afc_snr_squelch();
    test_instances();
    test_idle();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fldigi-mfsk-core: all unit tests passed (%s)\n", fmfsk_version());
    return 0;
}
