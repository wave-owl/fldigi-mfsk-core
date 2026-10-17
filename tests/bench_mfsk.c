/*
 * fldigi-mfsk-core CPU benchmark: receiver and transmitter speed per submode, in multiples
 * of real time on one core, at the modem rate and (receiver) from 8 kHz through the
 * resampler; the receiver's heap per instance.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include "test_util.h"

#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double rx_speed(const fmfsk_submode *m, const double *y, size_t len, int rate)
{
    fmfsk_rx *rx = make_rx(m->name, 1500.0, rate, 0);
    size_t np;
    double t0 = now(), t;
    unsigned char *out = decode(rx, y, len, 512, &np);
    t = now() - t0;
    free(out);
    fmfsk_rx_destroy(rx);
    return (double)len / rate / t;
}

int main(void)
{
    int i;
    printf("%-9s %6s %10s %12s %10s %8s\n", "submode", "rate", "RX x real", "RX@8k x real",
           "TX x real", "heap KiB");
    for (i = 0; i < fmfsk_submode_count(); i++) {
        const fmfsk_submode *m = fmfsk_submode_at(i);
        size_t n, n8, k, len = (size_t)m->samplerate * 30, len8 = 8000 * 30;
        double *x, *x8, *y, *y8, t0, t_tx;
        t0 = now();
        x = render_over(m->name, "CQ CQ DE DG6FL DG6FL K", 0, 0, 1500.0, 0, &n);
        t_tx = now() - t0;
        x8 = render_over(m->name, "CQ CQ DE DG6FL DG6FL K", 0, 0, 1500.0, 8000, &n8);
        y = (double *)calloc(len, sizeof(double));
        y8 = (double *)calloc(len8, sizeof(double));
        for (k = 0; k < len; k++)
            y[k] = 0.25 * x[k % n];
        for (k = 0; k < len8; k++)
            y8[k] = 0.25 * x8[k % n8];
        printf("%-9s %6d %10.0f %12.0f %10.0f %8.0f\n", m->name, m->samplerate,
               rx_speed(m, y, len, m->samplerate), rx_speed(m, y8, len8, 8000),
               (double)n / m->samplerate / t_tx,
               /* pipe, picture ring and row, interleaver, two Viterbi decoders, sfft */
               (2.0 * m->symlen * 32 * 16 + (FMFSK_PIC_RING + 1) * FMFSK_PIC_MAXW +
                (double)m->symbits * m->symbits * m->depth + 2 * 2 * 256 * 64 * 4 +
                3.0 * m->symlen * 16) / 1024.0);
        free(x);
        free(x8);
        free(y);
        free(y8);
    }
    return 0;
}
