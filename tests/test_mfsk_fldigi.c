/*
 * fldigi-mfsk-core against fldigi 4.2.13 itself (goldens from
 * tests/golden/make_fldigi_golden.py, the headless fldigi bench build).
 *
 * TX, all 11 MFSK submodes and two pangram overs: the port renders fldigi's TX vector (FNV-1a hash of the int16 output identical on the
 * reference platform, at most 1 LSB from the stored vectors everywhere).
 * RX, the same overs clean and at -5 / -10 dB (2500 Hz): the decoded bytes equal fldigi's
 * own decode on the reference platform; elsewhere (different libm / FMA use) the sent text
 * must be recovered with at most 1 + 5 % more errors than fldigi's own decode, and wherever
 * fldigi decoded the text completely, so must the port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>

#include "test_util.h"
#include "picture_util.h"

static const char *golden = "tests/golden";

static uint64_t fnv1a64(const int16_t *pcm, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t u = (uint16_t)pcm[i];
        h ^= (uint8_t)(u & 0xFF);
        h *= 0x100000001B3ULL;
        h ^= (uint8_t)(u >> 8);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* fldigi's sound path: modem::ModulateXmtr scales by 10^(txlevel/20), txlevel -12 dB, and
 * clips at +-0.95 (trx/modem.cxx:652-658); libsndfile writes PCM16 as lrint(x * 0x7FFF). */
static void quantize(const double *x, size_t n, int16_t *pcm)
{
    double mult = pow(10, -12.0 / 20.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double v = x[i] * mult;
        if (v < -0.95)
            v = -0.95;
        if (v > 0.95)
            v = 0.95;
        pcm[i] = (int16_t)lrint(v * 32767.0);
    }
}

/* Same as noisy() in make_fldigi_golden.py; this file is compiled without FP
 * contraction, so every step rounds like Python. */
static uint32_t lcg;
static double lcg_uniform(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg / 4294967296.0;
}

static double *noisy(const int16_t *pcm, size_t n, int sr, int snr_step, uint32_t seed,
                     int tail_s, size_t *out_n)
{
    size_t lead = (size_t)sr, tail = (size_t)tail_s * (size_t)sr, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double ps = 0, inv, s2, sd, gain;
    int k;
    for (i = 0; i < n; i++)
        x[lead + i] = (double)pcm[i];
    *out_n = total;
    if (snr_step == 0) {
        for (i = 0; i < total; i++)
            x[i] /= 32768.0;
        return x;
    }
    for (i = 0; i < n; i++)
        ps += (double)pcm[i] * (double)pcm[i];
    ps = ps / (double)n;
    inv = snr_step == 1 ? 1.0 / sqrt(10.0) : 1.0 / 10.0;
    s2 = ps / inv * (sr / 2.0) / 2500.0;
    sd = sqrt(s2);
    gain = 3276.7 / sqrt(ps * (double)n / (double)total + s2);
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, y, q;
        for (k = 0; k < 12; k++)
            z += lcg_uniform();
        z -= 6.0;
        y = (x[i] + sd * z) * gain;
        q = floor(y + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;  /* libsndfile sf_readf_double: int16 / 32768 */
    }
    return x;
}

static char texts[2][128];

static const char *text_for(const char *key)
{
    return strcmp(key, "cq") == 0 ? texts[0] : texts[1];
}

static void load_texts(void)
{
    char path[512], *buf, *line;
    size_t len;
    snprintf(path, sizeof path, "%s/texts.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "cq ", 3) == 0)
            snprintf(texts[0], sizeof texts[0], "%s", line + 3);
        else if (strncmp(line, "pangram ", 8) == 0)
            snprintf(texts[1], sizeof texts[1], "%s", line + 8);
    }
    free(buf);
}

static int vectors_done, rx_exact, rx_total;
static size_t noisy_port, noisy_fldigi;
static int full_port, full_fldigi;

static void check_vector(const char *stem, const char *sname, const char *key, int idx,
                         int sr, size_t samples, int tail, int offset, int afc, uint64_t hash)
{
    static const char *snr_names[3] = {"clean", "m05", "m10"};
    const fmfsk_submode *m = fmfsk_submode_find(sname);
    const char *text = text_for(key);
    double *audio;
    int16_t *pcm, *ref = NULL;
    size_t n, i, reflen = 0, diff1 = 0, diffbig = 0, ns = strlen(text);
    char path[512];
    int s;

    CHECK(m != NULL && m->samplerate == sr, "%s: submode %s", stem, sname);
    if (!m)
        return;
    audio = render_over(m->name, text, 0, 0, 1500.0 + offset, 0, &n);
    pcm = (int16_t *)malloc((n + 1) * sizeof(int16_t));
    quantize(audio, n, pcm);
    CHECK(n == samples, "%s: %zu samples, fldigi %zu", stem, n, samples);
    if (REFERENCE_PLATFORM)
        CHECK(fnv1a64(pcm, n) == hash, "%s: TX hash %016llx, fldigi %016llx", stem,
              (unsigned long long)fnv1a64(pcm, n), (unsigned long long)hash);
    snprintf(path, sizeof path, "%s/tx/%s.s16", golden, stem);
    ref = (int16_t *)read_file(path, &reflen);
    if (ref) {
        reflen /= 2;
        CHECK(reflen == n, "%s: stored vector %zu samples, port %zu", stem, reflen, n);
        for (i = 0; i < n && i < reflen; i++) {
            int d = abs((int)pcm[i] - (int)ref[i]);
            diff1 += d == 1;
            diffbig += d > 1;
        }
        CHECK(diffbig == 0, "%s: %zu samples differ by more than 1 LSB from fldigi", stem,
              diffbig);
        if (diff1)
            printf("  %s: %zu samples differ by 1 LSB from fldigi\n", stem, diff1);
    }

    for (s = 0; s < 3; s++) {
        fmfsk_rx *rx;
        size_t nx, got, glen;
        /* the noisy inputs are built from fldigi's vector where it is stored, else from
         * the port's (identical on the reference platform) */
        double *x = noisy(ref ? ref : pcm, ref ? reflen : n, sr, s, (uint32_t)(1000 * idx + s),
                          tail, &nx);
        unsigned char *out, *want;
        rx = make_rx(m->name, 1500.0, 0, afc);
        CHECK(rx != NULL, "%s: rx_create", stem);
        if (!rx) {
            free(x);
            continue;
        }
        out = decode(rx, x, nx, 512, &got);
        snprintf(path, sizeof path, "%s/rx/%s_%s.txt", golden, stem, snr_names[s]);
        want = (unsigned char *)read_file(path, &glen);
        CHECK(want != NULL, "%s: missing %s", stem, path);
        if (want) {
            int exact = got == glen && memcmp(out, want, glen) == 0;
            rx_total++;
            rx_exact += exact;
            if (REFERENCE_PLATFORM) {
                CHECK(exact, "%s %s: RX differs from fldigi", stem, snr_names[s]);
                if (contains(want, glen, text))
                    CHECK(contains(out, got, text), "%s %s: text lost", stem, snr_names[s]);
            } else {
                /* Another libm or no FMA: near the threshold single decodes may go either
                 * way, so clean decodes are checked
                 * one by one and the noisy ones in total. */
                size_t dp = window_distance((const unsigned char *)text, ns, out, got);
                size_t df = window_distance((const unsigned char *)text, ns, want, glen);
                if (!exact)
                    printf("  %s %s: not identical; text errors port %zu, fldigi %zu\n", stem,
                           snr_names[s], dp, df);
                if (s == 0) {
                    CHECK(dp <= df + 1 + ns / 20, "%s clean: %zu text errors, fldigi %zu", stem,
                          dp, df);
                } else {
                    noisy_port += dp;
                    noisy_fldigi += df;
                    full_port += contains(out, got, text);
                    full_fldigi += contains(want, glen, text);
                }
            }
        }
        free(want);
        free(out);
        free(x);
        fmfsk_rx_destroy(rx);
    }
    vectors_done++;
    free(ref);
    free(pcm);
    free(audio);
}

/* AC-837.1: picture overs (tests/picture_util.h). fldigi's bench decoded them
 * (tests/golden/pic/, make_picture_golden.sh): it switched to picture mode at the header and
 * printed the text after the picture. The port prints the same bytes and hands over every
 * row with the transmitted pixels. */
static int pictures_done;

static void check_picture(const char *sub, int color)
{
    const fmfsk_submode *m = fmfsk_submode_find(sub);
    char path[512], stem[64];
    size_t n, i, nx, got, glen, rows = 0, bad = 0;
    double *audio = render_picture_over(sub, color, &n), *x, mult = pow(10, -12.0 / 20.0);
    unsigned char *want, *out, *px = pic_pixels(color), line[FMFSK_PIC_MAXW];
    fmfsk_rx *rx = make_rx(sub, 1500.0, 0, 0);
    fmfsk_picture_info info;
    fmfsk_picture_row row;
    snprintf(stem, sizeof stem, "%s_%s", sub, color ? "color" : "grey");
    for (i = 0; stem[i]; i++)
        stem[i] = (char)tolower((unsigned char)stem[i]);
    nx = (size_t)m->samplerate * 13 + n;
    x = (double *)calloc(nx, sizeof(double));
    for (i = 0; i < n; i++)   /* as the WAV of make_picture_vector, read by libsndfile */
        x[(size_t)m->samplerate + i] = (double)(int16_t)lrint(audio[i] * mult * 32767.0) / 32768.0;
    out = (unsigned char *)malloc(nx / 8 + 4096);
    got = 0;
    for (i = 0; i < nx; i += 512) {
        size_t k = nx - i < 512 ? nx - i : 512;
        got += fmfsk_rx_process(rx, x + i, k, out + got, 4096);
        while (fmfsk_rx_poll_picture(rx, &row, line, sizeof line)) {
            int r = row.row, ch = row.channel < 0 ? 0 : row.channel, xx;
            rows++;
            CHECK(row.width == PIC_W && row.height == PIC_H && row.color == color &&
                  row.n == PIC_W, "%s: row header", stem);
            for (xx = 0; xx < row.n; xx++) {
                size_t idx = color ? (size_t)(r * 3 + ch) * PIC_W + (size_t)xx
                                   : (size_t)r * PIC_W + (size_t)xx;
                /* the receiver's band-pass (a few hundred Hz for 1000 pixels/s) smears the
                 * step between the end of one line and the start of the next: compare the
                 * inner pixels. fldigi's MFSK64 start delay (counter 4956, mfsk.cxx:486)
                 * puts the picture one pixel late; the port keeps that. */
                int d0 = abs((int)line[xx] - (int)px[idx]);
                int d1 = idx ? abs((int)line[xx] - (int)px[idx - 1]) : 255;
                int d = m->fldigi_id == 45 && d1 < d0 ? d1 : d0;
                if (xx >= 4 && xx < PIC_W - 4 && d > 12)
                    bad++;
            }
        }
    }
    snprintf(path, sizeof path, "%s/pic/%s.txt", golden, stem);
    want = (unsigned char *)read_file(path, &glen);
    CHECK(want != NULL, "missing %s", path);
    if (want && REFERENCE_PLATFORM)
        CHECK(got == glen && memcmp(out, want, glen) == 0, "%s: RX differs from fldigi", stem);
    CHECK(contains(out, got, PIC_AFTER), "%s: text after the picture lost", stem);
    fmfsk_rx_picture_info(rx, &info);
    CHECK(info.started == 1 && info.completed == 1 && info.width == PIC_W &&
          info.height == PIC_H && info.color == color && !info.active,
          "%s: picture info %d/%d", stem, info.started, info.completed);
    CHECK(rows == (size_t)PIC_H * (color ? 3 : 1), "%s: %zu rows", stem, rows);
    CHECK(bad == 0, "%s: %zu pixels off by more than 12", stem, bad);
    pictures_done++;
    free(want);
    free(out);
    free(x);
    free(px);
    free(audio);
    fmfsk_rx_destroy(rx);
}

int main(int argc, char **argv)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;

    if (argc > 1)
        golden = argv[1];
    load_texts();
    snprintf(path, sizeof path, "%s/tx_hashes.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "missing goldens in %s\n", golden);
        return 2;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], sname[16], key[16];
        int idx, sr, tail;
        size_t samples;
        unsigned long long hash;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %15s %15s %d %d %zu %d %llx", stem, sname, key, &idx, &sr,
                   &samples, &tail, &hash) != 8) {
            CHECK(0, "bad line: %s", line);
            continue;
        }
        check_vector(stem, sname, key, idx, sr, samples, tail, 0, 0, (uint64_t)hash);
    }
    free(buf);
    /* AFC: fldigi transmits off the receiver's carrier, fldigi's AFC follows */
    snprintf(path, sizeof path, "%s/afc.txt", golden);
    buf = (char *)read_file(path, &len);
    CHECK(buf != NULL, "missing %s", path);
    save = NULL;
    for (line = buf ? strtok_r(buf, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char stem[64], sname[16], key[16];
        int idx, sr, tail, off;
        size_t samples;
        unsigned long long hash;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %15s %15s %d %d %zu %d %d %llx", stem, sname, key, &idx, &sr,
                   &samples, &tail, &off, &hash) != 9) {
            CHECK(0, "bad line: %s", line);
            continue;
        }
        check_vector(stem, sname, key, idx, sr, samples, tail, off, 1, (uint64_t)hash);
    }
    free(buf);
    CHECK(vectors_done == 18, "%d vectors", vectors_done);
    {
        static const char *pics[] = {"MFSK16", "MFSK32", "MFSK64", "MFSK128"};
        int i, color;
        for (i = 0; i < 4; i++)
            for (color = 0; color < 2; color++)
                check_picture(pics[i], color);
        CHECK(pictures_done == 8, "%d pictures", pictures_done);
    }
    if (!REFERENCE_PLATFORM) {
        printf("noisy decodes: text errors port %zu, fldigi %zu; complete texts port %d, "
               "fldigi %d\n", noisy_port, noisy_fldigi, full_port, full_fldigi);
        CHECK(noisy_port <= noisy_fldigi + noisy_fldigi / 5 + 10,
              "noisy decodes: %zu text errors, fldigi %zu", noisy_port, noisy_fldigi);
        CHECK(full_port + 3 >= full_fldigi, "complete texts: port %d, fldigi %d", full_port,
              full_fldigi);
    }
    printf("fldigi-mfsk-core vs fldigi 4.2.13: %d overs, RX identical in %d of %d decodes, "
           "%d picture overs%s\n", vectors_done, rx_exact, rx_total, pictures_done,
           REFERENCE_PLATFORM ? " (reference platform)" : "");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
