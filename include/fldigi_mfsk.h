/*
 * fldigi-mfsk-core -- the MFSK modem of fldigi 4.2.13 (MFSK4 ... MFSK128L), receiver and
 * transmitter, ported to C99.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 (https://github.com/wave-owl/fldigi, tag v4.2.13, commit
 * e7c3ab3709ad62a7d91829364eb48a05fa325c0d): src/mfsk/mfsk.cxx, src/include/mfsk.h,
 * src/mfsk/mfsk-pic.cxx (picture header and receive timing) and the soft start/stop of
 * src/trx/modem.cxx; (c) 2006-2020 Dave Freese W1HKJ, John Douyere VK2ETA and the fldigi
 * authors, based on gmfsk (c) 2001-2003 Tomi Manninen OH2BNS.
 *
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this
 * program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef FLDIGI_MFSK_H
#define FLDIGI_MFSK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMFSK_VERSION_STRING "0.1.1"
#define FMFSK_ABI 1

/* "fldigi-mfsk-core 0.1.1 contract=mfsk-c1 abi=1 tx=1". Static string. The contract tag
 * changes only with an incompatible API change; tx=1 announces the transmitter. */
const char *fmfsk_version(void);
int fmfsk_abi(void);                    /* FMFSK_ABI */

/* ------------------------------------------------------------------------------------
 * Submodes: every MFSK mode of fldigi 4.2.13 (globals/globals.cxx:101-112; parameters
 * mfsk/mfsk.cxx:177-295).
 * ------------------------------------------------------------------------------------ */
typedef struct fmfsk_submode {
    const char *name;        /* "MFSK4" ... "MFSK128L" (fldigi's short name) */
    int fldigi_id;           /* trx_mode id of fldigi 4.2.13 (index into mode_info[]) */
    int samplerate;          /* modem rate: 8000 or 11025 Hz */
    int symlen;              /* samples per symbol */
    int symbits;             /* bits per symbol (3, 4 or 5) */
    int depth;               /* interleaver depth (symbits x symbits x depth) */
    int basetone;            /* DFT bin of the lowest tone at the mixer output */
    int numtones;            /* 8, 16 or 32 */
    int preamble;            /* fldigi's preamble/flush length in bits */
    int picture;             /* 1: fldigi offers pictures in this submode (CAP_IMG) */
    double tonespacing;      /* samplerate / symlen, Hz */
    double bandwidth;        /* fldigi's (numtones - 1) x tonespacing, Hz */
    double baud;             /* samplerate / symlen */
} fmfsk_submode;

int fmfsk_submode_count(void);                      /* 11 */
const fmfsk_submode *fmfsk_submode_at(int index);   /* NULL outside 0 .. count-1 */
/* Case-insensitive lookup by name; NULL when unknown. */
const fmfsk_submode *fmfsk_submode_find(const char *name);

/* ------------------------------------------------------------------------------------
 * Receiver: mfsk::rx_process (mfsk.cxx:857-925) with fldigi's defaults.
 * ------------------------------------------------------------------------------------ */
typedef struct fmfsk_rx_config {
    const char *submode;     /* see fmfsk_submode_find */
    double carrier_hz;       /* audio centre of the signal (fldigi "frequency"), 1500 */
    int input_rate;          /* 0: the submode's rate; else the rate of the samples given
                                to fmfsk_rx_process (e.g. 8000 for MFSK11/22), converted
                                inside with fldigi-dsp-core's resampler */
    int afc;                 /* 1: fldigi's AFC (progStatus.afconoff); 0: off (bench) */
    double squelch;          /* 0: squelch off (fldigi bench default); else metric
                                threshold 0..100 (progStatus.sldrSquelchValue) */
} fmfsk_rx_config;

typedef struct fmfsk_rx fmfsk_rx;

/* NULL for an unknown submode, a carrier outside fldigi's range (bandwidth/2 ..
 * 3000 Hz - bandwidth/2, modem::set_freq with the default waterfall limits), an
 * unsupported input rate or out of memory. */
fmfsk_rx *fmfsk_rx_create(const fmfsk_rx_config *cfg);
void fmfsk_rx_destroy(fmfsk_rx *rx);
/* Back to the state after create. */
void fmfsk_rx_reset(fmfsk_rx *rx);
/* Feed n samples at the input rate (fldigi's sound card scale: int16 / 32768). The bytes
 * fldigi hands to put_rx_char() go to text (NUL never, CR/STX/EOT included, as fldigi
 * prints them); returns their number. Bytes beyond max_text are dropped and counted
 * (fmfsk_rx_dropped). text may be NULL with max_text 0. */
size_t fmfsk_rx_process(fmfsk_rx *rx, const double *x, size_t n, unsigned char *text,
                        size_t max_text);
/* fldigi's s/n display: 20 log10 of the largest s/n of the last 100 symbols (eval_s2n,
 * mfsk.cxx:847-854); -99 while there is none (fldigi shows nothing). */
double fmfsk_rx_snr_db(const fmfsk_rx *rx);
double fmfsk_rx_metric(const fmfsk_rx *rx);        /* fldigi's metric 0..100 */
/* Current carrier (moves with AFC) minus the configured carrier, Hz. */
double fmfsk_rx_offset_hz(const fmfsk_rx *rx);
double fmfsk_rx_carrier_hz(const fmfsk_rx *rx);
void fmfsk_rx_set_carrier(fmfsk_rx *rx, double carrier_hz);   /* clamped like fldigi */
void fmfsk_rx_set_afc(fmfsk_rx *rx, int on);
void fmfsk_rx_set_squelch(fmfsk_rx *rx, double level);       /* 0 = off */
size_t fmfsk_rx_dropped(const fmfsk_rx *rx);

/* Pictures (R-837): fldigi's receive state machine (mfsk.cxx:372-466, 877-895). After a
 * "Pic:WxH[C][pN];" header the receiver stops decoding symbols for the picture's
 * duration and demodulates pixels, exactly as fldigi; the text after the picture stays
 * aligned. Rows go to a bounded ring (FMFSK_PIC_RING rows); when nobody polls, the oldest
 * rows are dropped. */
#define FMFSK_PIC_MAXW 4095
#define FMFSK_PIC_RING 32
typedef struct fmfsk_picture_info {
    int active;              /* 1 while a picture is being received */
    int started;             /* pictures started since create/reset */
    int completed;           /* pictures that ran to their end */
    int width, height;       /* of the current or last picture */
    int color;               /* 1: colour (R, G, B rows), 0: grey */
    int spp;                 /* receive samples per pixel (8, 4 or 2) */
    long pixels;             /* pixel bytes received of the current or last picture */
    size_t rows_dropped;     /* rows lost because the ring was full */
} fmfsk_picture_info;
void fmfsk_rx_picture_info(const fmfsk_rx *rx, fmfsk_picture_info *out);

typedef struct fmfsk_picture_row {
    int picture;             /* 1-based number of the picture (fmfsk_picture_info.started) */
    int width, height, color;
    int row;                 /* 0 .. height - 1 */
    int channel;             /* colour: 0 red, 1 green, 2 blue; grey: -1 */
    int n;                   /* pixel bytes written to pixels (= width) */
} fmfsk_picture_row;
/* Take the oldest complete row: copies at most max pixel bytes (the row's width is at
 * most FMFSK_PIC_MAXW) and returns 1, or 0 when there is none. */
int fmfsk_rx_poll_picture(fmfsk_rx *rx, fmfsk_picture_row *row, unsigned char *pixels,
                          size_t max);

/* Test taps (R-830.3): the hard tones (harddecode, before Gray decoding) and the soft
 * bits after the de-interleaver of the symbols decided since the last call, oldest
 * first, at most the last 1024 tones / 4096 soft bits. Return the numbers copied. */
size_t fmfsk_rx_take_tones(fmfsk_rx *rx, int *out, size_t max);
size_t fmfsk_rx_take_softbits(fmfsk_rx *rx, unsigned char *out, size_t max);

/* ------------------------------------------------------------------------------------
 * Transmitter: mfsk::tx_process (mfsk.cxx:1079-1214), one call per state step. Output:
 * the samples fldigi hands to modem::ModulateXmtr after its soft start/stop ramp
 * (modem.cxx:611-637), before the TX level (multiply by 10^(txlevel/20), fldigi's
 * -12 dB, to reproduce fldigi's WAV).
 * ------------------------------------------------------------------------------------ */
typedef struct fmfsk_tx_config {
    const char *submode;
    double carrier_hz;       /* audio centre (fldigi tx frequency), 1500 */
    int output_rate;         /* 0: the submode's rate; else e.g. 8000 for MFSK11/22 */
    int no_soft_edges;       /* 1: no softMFSK ramps (fldigi default on) */
} fmfsk_tx_config;

typedef struct fmfsk_tx fmfsk_tx;

fmfsk_tx *fmfsk_tx_create(const fmfsk_tx_config *cfg);
void fmfsk_tx_destroy(fmfsk_tx *tx);
/* Largest number of samples one begin/put/idle/end call can produce. */
size_t fmfsk_tx_bound(const fmfsk_tx *tx);
/* Each call returns the samples written to out, or 0 (nothing done) when out_max <
 * fmfsk_tx_bound(tx) or the call is out of order (begin once, then put/idle, then end). */
/* TX_STATE_PREAMBLE + TX_STATE_START: prime the interleaver (clearbits), preamble/3 zero
 * bits (none for MFSK64L/128L), CR STX CR. */
size_t fmfsk_tx_begin(fmfsk_tx *tx, double *out, size_t out_max);
/* TX_STATE_DATA with one keyboard byte. Refused (returns 0 and counts it in
 * fmfsk_tx_refused) for 0x05, which starts a picture in fldigi, and for a byte that
 * would complete a picture header "Pic:WxH[C][pN];" as fldigi's receiver parses it, after
 * any "Pic:" of the last 63 printable characters (R-834.2): keyboard text can never start
 * a picture, not even in a receiver that joined late. */
size_t fmfsk_tx_put(fmfsk_tx *tx, unsigned char c, double *out, size_t out_max);
/* TX_STATE_DATA without keyboard data: sendidle (NUL, a one bit, 32 zero bits). */
size_t fmfsk_tx_idle(fmfsk_tx *tx, double *out, size_t out_max);
/* TX_STATE_FLUSH: CR EOT CR, flushtx(preamble), soft stop and its zero bits. */
size_t fmfsk_tx_end(fmfsk_tx *tx, double *out, size_t out_max);
size_t fmfsk_tx_refused(const fmfsk_tx *tx);

#ifdef __cplusplus
}
#endif

#endif /* FLDIGI_MFSK_H */
