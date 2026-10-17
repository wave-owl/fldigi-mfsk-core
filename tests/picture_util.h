/*
 * fldigi-mfsk-core: the picture overs of the picture goldens (tests/golden/pic/), shared by
 * tests/make_picture_vector.c and tests/test_mfsk_fldigi.c.
 *
 * An over in fldigi's order: begin, "before ", the picture as fldigi transmits it after
 * 0x05 (header text, flush, prologue, pixel tones, flush; internal helper
 * fmfsk__tx_picture), "AFTER PICTURE DE DG6FL", end. Pixels: a ramp along each line,
 * 30 + 7 x + 10 (line mod 6), smooth enough for the receiver's band-pass.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MFSK_PICTURE_UTIL_H
#define MFSK_PICTURE_UTIL_H

#include "../src/mfsk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIC_W 24
#define PIC_H 6
#define PIC_BEFORE "before "
#define PIC_AFTER "AFTER PICTURE DE DG6FL"

static inline unsigned char *pic_pixels_wh(int color, int w, int h)
{
    size_t n = (size_t)w * h * (color ? 3 : 1), i;
    unsigned char *px = (unsigned char *)malloc(n);
    for (i = 0; i < n; i++)
        px[i] = (unsigned char)(30 + 7 * (i % (size_t)w) % 175 + 10 * ((i / (size_t)w) % 6));
    return px;
}

static inline unsigned char *pic_pixels(int color)
{
    return pic_pixels_wh(color, PIC_W, PIC_H);
}

static inline double *render_picture_over_wh(const char *submode, int color, int w, int h,
                                             size_t *n_out)
{
    fmfsk_tx_config c;
    fmfsk_tx *tx;
    char hdr[64];
    size_t bound, pb, n = 0, k, np = (size_t)w * h * (color ? 3 : 1), cap;
    unsigned char *px = pic_pixels_wh(color, w, h);
    double *all, *tmp;
    const char *p;
    memset(&c, 0, sizeof c);
    c.submode = submode;
    c.carrier_hz = 1500;
    tx = fmfsk_tx_create(&c);
    if (!tx) {
        free(px);
        *n_out = 0;
        return NULL;
    }
    /* fldigi's header, mfsk-pic.cxx:200, 241 */
    snprintf(hdr, sizeof hdr, "\nSending Pic:%dx%d%s;", w, h, color ? "C" : "");
    bound = fmfsk_tx_bound(tx);
    pb = fmfsk__tx_picture_bound(tx, hdr, np);
    cap = bound * (strlen(PIC_BEFORE) + strlen(PIC_AFTER) + 2) + pb;
    all = (double *)malloc(cap * sizeof(double));
    tmp = (double *)malloc((bound > pb ? bound : pb) * sizeof(double));
    k = fmfsk_tx_begin(tx, tmp, bound);
    memcpy(all + n, tmp, k * sizeof(double));
    n += k;
    for (p = PIC_BEFORE; *p; p++) {
        k = fmfsk_tx_put(tx, (unsigned char)*p, tmp, bound);
        memcpy(all + n, tmp, k * sizeof(double));
        n += k;
    }
    k = fmfsk__tx_picture(tx, hdr, px, np, tmp, pb);
    memcpy(all + n, tmp, k * sizeof(double));
    n += k;
    for (p = PIC_AFTER; *p; p++) {
        k = fmfsk_tx_put(tx, (unsigned char)*p, tmp, bound);
        memcpy(all + n, tmp, k * sizeof(double));
        n += k;
    }
    k = fmfsk_tx_end(tx, tmp, bound);
    memcpy(all + n, tmp, k * sizeof(double));
    n += k;
    free(tmp);
    free(px);
    fmfsk_tx_destroy(tx);
    *n_out = n;
    return all;
}

static inline double *render_picture_over(const char *submode, int color, size_t *n_out)
{
    return render_picture_over_wh(submode, color, PIC_W, PIC_H, n_out);
}

#endif
