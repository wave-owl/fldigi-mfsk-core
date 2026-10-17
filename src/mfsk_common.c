/*
 * fldigi-mfsk-core: version, submode table and the picture header parser shared by the
 * receiver and the transmitter.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 src/mfsk/mfsk.cxx (c) 2006-2009 Dave Freese W1HKJ, based on
 * gmfsk (c) 2001-2003 Tomi Manninen OH2BNS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mfsk_internal.h"

#include <ctype.h>
#include <string.h>

const char *fmfsk_version(void)
{
    return "fldigi-mfsk-core " FMFSK_VERSION_STRING " contract=mfsk-c1 abi=1 tx=1";
}

int fmfsk_abi(void)
{
    return FMFSK_ABI;
}

#define SM(name, id, sr, sl, bits, depth, base, tones, pre, pic)                          \
    {name, id, sr, sl, bits, depth, base, tones, pre, pic, (double)(sr) / (sl),         \
     ((tones) - 1) * ((double)(sr) / (sl)), (double)(sr) / (sl)}

/* mfsk::mfsk, mfsk.cxx:177-295: samplerate, symlen, symbits, depth, basetone, numtones,
 * preamble; CAP_IMG for MFSK16/32/64/128. tonespacing = samplerate / symlen (:298),
 * bandwidth = (numtones - 1) x tonespacing (:335). Trx_mode ids: globals.h:90-100. */
static const fmfsk_submode SUBMODES[] = {
    SM("MFSK4", 41, 8000, 2048, 5, 5, 256, 32, 107, 0),
    SM("MFSK8", 38, 8000, 1024, 5, 5, 128, 32, 107, 0),
    SM("MFSK11", 42, 11025, 1024, 4, 10, 93, 16, 107, 0),
    SM("MFSK16", 39, 8000, 512, 4, 10, 64, 16, 107, 1),
    SM("MFSK22", 43, 11025, 512, 4, 10, 46, 16, 107, 0),
    SM("MFSK31", 44, 8000, 256, 3, 10, 32, 8, 107, 0),
    SM("MFSK32", 40, 8000, 256, 4, 10, 32, 16, 107, 1),
    SM("MFSK64", 45, 8000, 128, 4, 10, 16, 16, 180, 1),
    SM("MFSK128", 46, 8000, 64, 4, 20, 8, 16, 214, 1),
    SM("MFSK64L", 47, 8000, 128, 4, 400, 16, 16, 2500, 0),
    SM("MFSK128L", 48, 8000, 64, 4, 800, 8, 16, 5000, 0),
};

int fmfsk_submode_count(void)
{
    return (int)(sizeof SUBMODES / sizeof SUBMODES[0]);
}

const fmfsk_submode *fmfsk_submode_at(int index)
{
    if (index < 0 || index >= fmfsk_submode_count())
        return NULL;
    return &SUBMODES[index];
}

const fmfsk_submode *fmfsk_submode_find(const char *name)
{
    int i;
    if (!name)
        return NULL;
    for (i = 0; i < fmfsk_submode_count(); i++) {
        const char *a = SUBMODES[i].name, *b = name;
        while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return &SUBMODES[i];
    }
    return NULL;
}

double fmfsk__clamp(double x, double lo, double hi)
{
    /* clamp / CLAMP, include/misc.h:53-56 */
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

void fmfsk__clear_picture_header(char *window)
{
    /* memset(picheader, ' ', PICHEADER - 1); picheader[PICHEADER - 1] = 0;
     * (mfsk.cxx:107-108, 510-511) */
    memset(window, ' ', MFSK_PICHEADER - 1);
    window[MFSK_PICHEADER - 1] = 0;
}

int fmfsk__parse_picture_header(const char *p, int *w, int *h, int *color, int *spp)
{
    /* the parser of mfsk::check_picture_header from "Pic:" on, mfsk.cxx:389-427 */
    int picW = 0, picH = 0, col = 0, rxspp;
    *w = *h = *color = 0;
    p += 4;
    if (*p == 0)
        return 0;
    while (*p && isdigit((unsigned char)*p))
        picW = (picW * 10) + (*p++ - '0');
    if (*p++ != 'x')
        return 0;
    while (*p && isdigit((unsigned char)*p))
        picH = (picH * 10) + (*p++ - '0');
    if (*p == 'C') {
        col = 1;
        p++;
    }
    *w = picW;
    *h = picH;
    *color = col;
    if (*p == ';') {
        if (picW == 0 || picH == 0 || picW > 4095 || picH > 4095)
            return 0;
        *spp = 8;
        return 1;
    }
    if (*p == 'p')
        p++;
    else
        return 0;
    if (!*p)
        return 0;
    rxspp = 8;
    if (*p == '4')
        rxspp = 4;
    if (*p == '2')
        rxspp = 2;
    p++;
    if (!*p)
        return 0;
    if (*p != ';')
        return 0;
    if (picW == 0 || picH == 0 || picW > 4095 || picH > 4095)
        return 0;
    *spp = rxspp;
    return 1;
}

void fmfsk__push_picture_header(char *window, int c)
{
    /* mfsk::check_picture_header, mfsk.cxx:376-379: printable characters enter the window */
    if (c >= ' ' && c <= 'z') {
        memmove(window, window + 1, MFSK_PICHEADER - 1);
        window[MFSK_PICHEADER - 2] = (char)c;
    }
}

int fmfsk__check_picture_header(char *window, int c, int *w, int *h, int *color, int *spp)
{
    /* mfsk::check_picture_header, mfsk.cxx:372-428: only the first "Pic:" counts */
    const char *p;
    fmfsk__push_picture_header(window, c);
    *w = *h = *color = 0;
    p = strstr(window, "Pic:");
    if (p == NULL)
        return 0;
    return fmfsk__parse_picture_header(p, w, h, color, spp);
}

int fmfsk__any_picture_header(const char *window)
{
    /* stricter than fldigi's receiver: a header after any "Pic:" in the window, because a
     * receiver that joined later may not have the earlier ones in its window */
    const char *p = window;
    int w, h, color, spp;
    while ((p = strstr(p, "Pic:")) != NULL) {
        if (fmfsk__parse_picture_header(p, &w, &h, &color, &spp))
            return 1;
        p++;
    }
    return 0;
}
