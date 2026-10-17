/*
 * Writes the picture over of tests/picture_util.h as a WAV (1 s silence + over + 12 s
 * silence, fldigi's TX level -12 dB, PCM16) for tests/golden/make_picture_golden.sh.
 *   make_picture_vector MFSK16 0|1 out.wav
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "picture_util.h"

#include <math.h>
#include <stdint.h>

static void put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    fwrite(b, 1, 2, f);
}

int main(int argc, char **argv)
{
    size_t n, i, total;
    double *x, mult = pow(10, -12.0 / 20.0);
    const fmfsk_submode *m;
    FILE *f;
    if (argc != 4 || !(m = fmfsk_submode_find(argv[1]))) {
        fprintf(stderr, "usage: make_picture_vector SUBMODE 0|1 out.wav\n");
        return 2;
    }
    x = render_picture_over(m->name, atoi(argv[2]), &n);
    total = (size_t)m->samplerate * 13 + n;
    f = fopen(argv[3], "wb");
    if (!x || !f)
        return 1;
    fwrite("RIFF", 1, 4, f);
    put32(f, (uint32_t)(36 + 2 * total));
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, (uint32_t)m->samplerate);
    put32(f, (uint32_t)m->samplerate * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)(2 * total));
    for (i = 0; i < total; i++) {
        size_t j = i - (size_t)m->samplerate;
        double v = (i >= (size_t)m->samplerate && j < n) ? x[j] * mult : 0.0;
        put16(f, (uint16_t)(int16_t)lrint(v * 32767.0));
    }
    fclose(f);
    free(x);
    return 0;
}
