/*
 * fldigi-mfsk-core internals (not installed).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MFSK_INTERNAL_H
#define MFSK_INTERNAL_H

#include "fldigi_mfsk.h"
#include "fldigi_dsp.h"

#define MFSK_MAX_SYMBOLS 32      /* MAX_SYMBOLS, mfsk.h:46 */
#define MFSK_PICHEADER 64        /* PICHEADER, mfsk.h:47 */
#define MFSK_K 7                 /* NASA_K, POLY1, POLY2: mfsk.h:51-53 */
#define MFSK_POLY1 0x6d
#define MFSK_POLY2 0x4f
#define MFSK_TRACE 45            /* tracepair(45, 352), mfsk.cxx:58-64 */
#define MFSK_TRACE_DELAY 352
#define MFSK_CWI_MAXCOUNT 6      /* mfsk.cxx:604 */
#define MFSK_AVG 32              /* progdefaults.mfsk_avg, configuration.h:1004-1006 */
#define MFSK_SOFTSTART_MS 100    /* progdefaults.SoftStart, configuration.h:222-224 */
#define MFSK_LOW_CUTOFF 0        /* progdefaults.LowFreqCutoff, configuration.h:204-206 */
#define MFSK_HIGH_CUTOFF 3000    /* progdefaults.HighFreqCutoff, configuration.h:207-209 */
#define MFSK_TXSPP 8             /* txSPP, mfsk-pic.cxx:48 */
#define MFSK_RESAMPLE_TAPS 32    /* resampler taps per phase (station rate <-> modem rate) */
#define MFSK_TAP_TONES 1024
#define MFSK_TAP_SOFT 4096

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MFSK_TWOPI (2.0 * M_PI)  /* TWOPI, modem.h:45 */

/* fldigi modes with special picture start delays (recvchar, mfsk.cxx:476-499) */
#define MFSK_ID_MFSK8 38
#define MFSK_ID_MFSK16 39
#define MFSK_ID_MFSK32 40
#define MFSK_ID_MFSK31 44
#define MFSK_ID_MFSK64 45
#define MFSK_ID_MFSK128 46
#define MFSK_ID_MFSK64L 47
#define MFSK_ID_MFSK128L 48

/* fldigi's picture header parser on a 64-byte window (check_picture_header,
 * mfsk.cxx:372-428). window holds PICHEADER - 1 characters and a NUL. Returns 1 for a
 * complete header and sets w, h, color, spp. */
int fmfsk__check_picture_header(char *window, int c, int *w, int *h, int *color, int *spp);
void fmfsk__push_picture_header(char *window, int c);
int fmfsk__parse_picture_header(const char *p, int *w, int *h, int *color, int *spp);
int fmfsk__any_picture_header(const char *window);
void fmfsk__clear_picture_header(char *window);
double fmfsk__clamp(double x, double lo, double hi);

typedef struct {
    fdsp_cmpx vector[MFSK_MAX_SYMBOLS];   /* struct rxpipe, mfsk.h:116-118 */
} mfsk_rxpipe;

struct fmfsk_rx {
    const fmfsk_submode *m;
    double carrier0;         /* configured carrier */
    double frequency;        /* modem::frequency (moves with AFC) */
    int afc_on;
    double squelch;
    int sql_on;
    /* resampler to the modem rate (input_rate != samplerate) */
    int resample;
    fdsp_resampler rs;
    double rsbuf[512];
    /* front end */
    double phaseacc;
    fdsp_fir hbfilt;
    fdsp_fir bpfilt;
    fdsp_sfft binsfft;
    mfsk_rxpipe *pipe;       /* 2 x symlen */
    unsigned int pipeptr;
    fdsp_movavg syncfilter;
    int synccounter;
    /* rx state (mfsk.h) */
    int rxstate;
    int counter;
    int symcounter;
    int symbolbit;
    double met1, met2, metric;
    fdsp_cmpx currvector, prev1vector, prev2vector;
    int currsymbol, prev1symbol, prev2symbol;
    double afcmetric;
    int staticburst;
    double freqerr;
    double s2n, sig, noise;
    double s2nvals[100];     /* static in fldigi (mfsk.cxx:69-71), here per instance */
    int s2nptr;
    int cwicounter[MFSK_MAX_SYMBOLS];   /* function static in softdecode (mfsk.cxx:603) */
    fdsp_viterbi dec1, dec2;
    fdsp_interleave rxinlv;
    unsigned char symbolpair[2];
    unsigned int datashreg;
    /* pictures */
    char picheader[MFSK_PICHEADER];
    int picW, picH, color, RXspp;
    int picturesize;
    fdsp_cmpx prevz;
    double picf;
    int row, col, rgb, pixelnbr;
    fmfsk_picture_info pic;
    unsigned char *picrow;   /* line being assembled: 3 x FMFSK_PIC_MAXW (colour) */
    unsigned char *ring;     /* FMFSK_PIC_RING x FMFSK_PIC_MAXW pixel bytes */
    fmfsk_picture_row ringhdr[FMFSK_PIC_RING];
    size_t ring_head, ring_count;
    /* test taps */
    int tones[MFSK_TAP_TONES];
    size_t tone_head, tone_count;
    unsigned char soft[MFSK_TAP_SOFT];
    size_t soft_head, soft_count;
    /* output of the current process() call */
    unsigned char *out;
    size_t max, n, dropped;
};

struct fmfsk_tx {
    const fmfsk_submode *m;
    double frequency;        /* tx_frequency */
    int soft;
    int state;               /* 0 new, 1 data, 2 ended */
    double phaseacc;
    unsigned int bitshreg;
    int bitstate;
    fdsp_encoder enc;
    fdsp_interleave txinlv;
    char picheader[MFSK_PICHEADER];   /* the receiver's header window (R-834.2) */
    size_t refused;
    /* modem::ModulateXmtr soft start/stop (modem.cxx:611-637) */
    int sig_start, sig_stop, sig_smpl;
    /* resampler to the output rate */
    int resample;
    fdsp_resampler rs;
    double *native;          /* one call's samples at the modem rate */
    size_t native_cap;
    size_t bound;
    /* output of the current call (modem rate) */
    double *out;
    size_t n, max;
};

/* Test helper (not API): renders a picture the way fldigi transmits one after 0x05
 * (TX_STATE_DATA -> PICTURE_START -> PICTURE, mfsk.cxx:1130-1139, 1176-1207): the header
 * text, flushtx(preamble), the 352-sample prologue, the pixel tones at TXspp = 8 and
 * flushtx(preamble). pixels: grey width x height bytes, or colour 3 x width x height in
 * fldigi's send order (per row: all red, all green, all blue). */
size_t fmfsk__tx_picture(fmfsk_tx *tx, const char *header, const unsigned char *pixels,
                         size_t npixels, double *out, size_t out_max);
size_t fmfsk__tx_picture_bound(const fmfsk_tx *tx, const char *header, size_t npixels);

#endif
