/*
 * fldigi-mfsk-core test helpers (header-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MFSK_TEST_UTIL_H
#define MFSK_TEST_UTIL_H

#include "fldigi_mfsk.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures __attribute__((unused));

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        }                                                                                \
    } while (0)

/* The platform the goldens were made on (fldigi 4.2.13 bench, macOS arm64, Apple clang -O2
 * with its default FP contraction): there the port must reproduce fldigi bit for bit. The
 * sanitizer build (-O1) defines MFSK_NOT_REFERENCE. */
#if defined(__APPLE__) && defined(__aarch64__) && !defined(MFSK_NOT_REFERENCE)
#define REFERENCE_PLATFORM 1
#else
#define REFERENCE_PLATFORM 0
#endif

static inline void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

typedef struct {
    double *x;
    size_t n, cap;
} sbuf;

static inline void sbuf_add(sbuf *b, const double *x, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 1024;
        b->x = (double *)realloc(b->x, b->cap * sizeof(double));
    }
    memcpy(b->x + b->n, x, n * sizeof(double));
    b->n += n;
}

/* A whole over as fldigi's bench sends it: begin, the text, end; idle_before/after idle
 * steps around the text. output_rate 0 = the submode's rate. Returns a malloc'ed buffer
 * of *n samples. */
static inline double *render_over(const char *submode, const char *text, int idle_before,
                                  int idle_after, double carrier, int output_rate, size_t *n)
{
    fmfsk_tx_config c;
    fmfsk_tx *tx;
    sbuf b = {NULL, 0, 0};
    double *tmp;
    size_t bound, k, i;
    memset(&c, 0, sizeof c);
    c.submode = submode;
    c.carrier_hz = carrier;
    c.output_rate = output_rate;
    tx = fmfsk_tx_create(&c);
    if (!tx) {
        *n = 0;
        return NULL;
    }
    bound = fmfsk_tx_bound(tx);
    tmp = (double *)malloc(bound * sizeof(double));
    k = fmfsk_tx_begin(tx, tmp, bound);
    sbuf_add(&b, tmp, k);
    for (i = 0; i < (size_t)idle_before; i++) {
        k = fmfsk_tx_idle(tx, tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    for (i = 0; text[i]; i++) {
        k = fmfsk_tx_put(tx, (unsigned char)text[i], tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    for (i = 0; i < (size_t)idle_after; i++) {
        k = fmfsk_tx_idle(tx, tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    k = fmfsk_tx_end(tx, tmp, bound);
    sbuf_add(&b, tmp, k);
    free(tmp);
    fmfsk_tx_destroy(tx);
    *n = b.n;
    return b.x;
}

/* Decode x in chunks of `chunk` samples. Returns the malloc'ed text bytes (*np). */
static inline unsigned char *decode(fmfsk_rx *rx, const double *x, size_t n, size_t chunk,
                                    size_t *np)
{
    unsigned char *p = (unsigned char *)malloc(n / 8 + 4096);
    size_t off, gp = 0;
    for (off = 0; off < n; off += chunk) {
        size_t m = n - off < chunk ? n - off : chunk;
        gp += fmfsk_rx_process(rx, x + off, m, p + gp, 4096);
    }
    p[gp] = 0;
    *np = gp;
    return p;
}

static inline fmfsk_rx *make_rx(const char *submode, double carrier, int input_rate, int afc)
{
    fmfsk_rx_config c;
    memset(&c, 0, sizeof c);
    c.submode = submode;
    c.carrier_hz = carrier;
    c.input_rate = input_rate;
    c.afc = afc;
    return fmfsk_rx_create(&c);
}

/* Minimum edit distance between want and any substring of got. */
static inline size_t window_distance(const unsigned char *want, size_t nw,
                                     const unsigned char *got, size_t ng)
{
    size_t *prev = (size_t *)malloc((nw + 1) * sizeof(size_t));
    size_t *cur = (size_t *)malloc((nw + 1) * sizeof(size_t));
    size_t i, j, best;
    for (i = 0; i <= nw; i++)
        prev[i] = i;
    best = prev[nw];
    for (j = 1; j <= ng; j++) {
        cur[0] = 0;
        for (i = 1; i <= nw; i++) {
            size_t a = prev[i - 1] + (want[i - 1] != got[j - 1]);
            size_t b = prev[i] + 1, c = cur[i - 1] + 1;
            cur[i] = a < b ? (a < c ? a : c) : (b < c ? b : c);
        }
        if (cur[nw] < best)
            best = cur[nw];
        memcpy(prev, cur, (nw + 1) * sizeof(size_t));
    }
    free(prev);
    free(cur);
    return best;
}

static inline int contains(const unsigned char *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle), i;
    for (i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0)
            return 1;
    return 0;
}

#endif
