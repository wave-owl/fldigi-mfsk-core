# fldigi-mfsk-core

[![CI](https://github.com/wave-owl/fldigi-mfsk-core/actions/workflows/ci.yml/badge.svg)](https://github.com/wave-owl/fldigi-mfsk-core/actions/workflows/ci.yml)

A C99 library that receives and transmits **MFSK** exactly the way fldigi 4.2.13 does: audio
in, characters out, and characters in, audio out, for all eleven fldigi MFSK submodes
(MFSK4, 8, 11, 16, 22, 31, 32, 64, 128, 64L, 128L). It is a line-by-line port of
`src/mfsk/mfsk.cxx` with the soft start/stop of `src/trx/modem.cxx`, built on the shared
[fldigi-dsp-core](https://github.com/wave-owl/fldigi-dsp-core) (sliding DFT, FIR/Hilbert,
moving average, K=7 Viterbi and encoder, interleaver, MFSK varicode, Gray code, resampler).
On the reference build the transmitter reproduces fldigi's TX audio sample for sample and the
receiver prints fldigi's characters byte for byte, with and without AFC, and through
pictures (see [Verification](#verification)). It is the only MFSK implementation of
[wave-owl](https://github.com/wave-owl/wave-owl).

| | |
|---|---|
| **Is** | A single-channel **receiver** for every MFSK submode of fldigi 4.2.13: the bytes fldigi prints, fldigi's AFC (optional, off as in fldigi's bench), squelch, s/n and metric, and fldigi's picture state machine: after a `Pic:WxH[C][pN];` header the receiver demodulates the picture for its duration, as fldigi, so the text after it stays aligned; the rows come out of a bounded ring. |
| **Is** | The matching **transmitter** (capability tag `tx=1`): interleaver priming, preamble, framing (CR STX CR ... CR EOT CR), keyboard characters, idles, flush and soft start/stop, as fldigi's `tx_process`. Keyboard text can never start a picture. |
| **Is** | Rate conversion: a receiver can take 8 kHz (or 48 kHz) audio for the 11025 Hz submodes, a transmitter can render them at 8 kHz (fldigi-dsp-core's resampler). |
| **Is not** | A skimmer: each instance decodes one submode at one carrier. No picture transmitter (only an internal test helper), no picture files, no RSID, no ARQ/KISS. |
| | An I/O layer, threaded or allocating at run time: no sound card or file access, no threads, all memory allocated in `*_create()`, no global mutable state. |

## Contents

- [Signal chain: receiver](#signal-chain-receiver)
- [Signal chain: transmitter](#signal-chain-transmitter)
- [Submodes](#submodes)
- [Pictures](#pictures)
- [Differences from fldigi](#differences-from-fldigi)
- [API reference](#api-reference)
- [Usage example](#usage-example)
- [Contract and versioning](#contract-and-versioning)
- [Verification](#verification)
- [Performance and memory](#performance-and-memory)
- [Building](#building)
- [Maintenance and fldigi updates](#maintenance-and-fldigi-updates)
- [License and credits](#license-and-credits)

## Signal chain: receiver

All citations refer to fldigi tag `v4.2.13` (mirror `wave-owl/fldigi`, commit
`e7c3ab3709ad62a7d91829364eb48a05fa325c0d`); every C function names the lines it ports.
Settings are fldigi's defaults (`configuration.h`, `status.cxx`): reverse off, AFC and squelch
as configured (fldigi's bench: off), AFC time constant `mfsk_avg` 32, waterfall limits 0 ..
3000 Hz.

```
 audio x[n] at the submode rate (8000 or 11025 Hz; int16 / 32768 scale)
   [optional: fdsp_resampler from the input rate]
      v
 analytic signal: 37-tap Hilbert pair (C_FIR_filter::init_hilbert)     mfsk.cxx:864-866
      v
 mixer: carrier -> bin basetone + bandwidth / 2                        mfsk.cxx:690-703
      v
 127-tap band pass over the tones +- 2 spacings                        mfsk.cxx:322-329
      v
 sliding DFT, one symbol long, over numtones bins, into a two-symbol pipe   :873-875
      v  picture? (RX_STATE_PICTURE_START / _PICTURE: pixels, no symbols)   :877-895
      v  once per symbol (synccounter)
 harddecode: strongest tone, static-burst test, AFC metric             mfsk.cxx:709-738
 softdecode: CWI counters, Gray decode, soft bits 0..255               mfsk.cxx:597-688
 de-interleaver symbits x symbits x depth
 decodesymbol: Viterbi K=7 (0x6d, 0x4f), traceback 45, chunk 1; for 3 and 5 bits two
   decoders on both pair phases and a metric vote; squelch              mfsk.cxx:543-595
 recvbit -> MFSK varicode -> recvchar (picture header check)           mfsk.cxx:468-541
 synchronize (symbol timing from the previous tone's peak)             mfsk.cxx:755-784
 afc (optional), eval_s2n                                              mfsk.cxx:792-855
      v
 bytes as fldigi's put_rx_char() receives them (CR STX CR text CR EOT CR)
```

## Signal chain: transmitter

`fmfsk_tx_begin` / `_put` / `_idle` / `_end` are the steps of fldigi's `tx_process` state
machine (`mfsk.cxx:1079-1214`):

| Call | fldigi state | What is sent |
|---|---|---|
| `fmfsk_tx_begin` | `TX_STATE_PREAMBLE` + `TX_STATE_START` | `clearbits` (the interleaver is primed with `preamble` encoded zeros, nothing on air), `preamble / 3` zero bits (none for MFSK64L/128L), CR STX CR |
| `fmfsk_tx_put(c)` | `TX_STATE_DATA` with a character | varicode, K=7 encoder, interleaver, Gray code, one tone per `symbits` code bits |
| `fmfsk_tx_idle` | `TX_STATE_DATA`, no keyboard data | `sendidle`: NUL, a one, 32 zeros |
| `fmfsk_tx_end` | `TX_STATE_FLUSH` | CR EOT CR, `flushtx(preamble)`, the soft-stop ramp over its zero bits |

Each tone is `cos` of a phase that falls by `2 pi (carrier - bandwidth/2 + tone x spacing) /
rate` per sample (`sendsymbol`, `mfsk.cxx:941-959`); tone 0 is the lowest. The first and last
`((rate x 100 ms) / symlen + 1) x symlen` samples of an over are ramped as in
`modem::ModulateXmtr` (`modem.cxx:611-637`). The output is the signal before fldigi's TX
level; multiply by `10^(-12/20)` and clip at +-0.95 to get fldigi's sound-card samples.

## Submodes

| Name | fldigi id | Rate | symlen | bits | Interleaver | Tones | Spacing | Bandwidth | Baud | Preamble | Pictures |
|---|---|---|---|---|---|---|---|---|---|---|---|
| MFSK4 | 41 | 8000 | 2048 | 5 | 5x5x5 | 32 | 3.906 Hz | 121.1 Hz | 3.9 | 107 | |
| MFSK8 | 38 | 8000 | 1024 | 5 | 5x5x5 | 32 | 7.813 Hz | 242.2 Hz | 7.8 | 107 | |
| MFSK11 | 42 | 11025 | 1024 | 4 | 4x4x10 | 16 | 10.767 Hz | 161.5 Hz | 10.8 | 107 | |
| MFSK16 | 39 | 8000 | 512 | 4 | 4x4x10 | 16 | 15.625 Hz | 234.4 Hz | 15.6 | 107 | yes |
| MFSK22 | 43 | 11025 | 512 | 4 | 4x4x10 | 16 | 21.533 Hz | 323.0 Hz | 21.5 | 107 | |
| MFSK31 | 44 | 8000 | 256 | 3 | 3x3x10 | 8 | 31.25 Hz | 218.8 Hz | 31.3 | 107 | |
| MFSK32 | 40 | 8000 | 256 | 4 | 4x4x10 | 16 | 31.25 Hz | 468.8 Hz | 31.3 | 107 | yes |
| MFSK64 | 45 | 8000 | 128 | 4 | 4x4x10 | 16 | 62.5 Hz | 937.5 Hz | 62.5 | 180 | yes |
| MFSK128 | 46 | 8000 | 64 | 4 | 4x4x20 | 16 | 125 Hz | 1875 Hz | 125 | 214 | yes |
| MFSK64L | 47 | 8000 | 128 | 4 | 4x4x400 | 16 | 62.5 Hz | 937.5 Hz | 62.5 | 2500 | |
| MFSK128L | 48 | 8000 | 64 | 4 | 4x4x800 | 16 | 125 Hz | 1875 Hz | 125 | 5000 | |

Bandwidth is fldigi's `(numtones - 1) x spacing` (`mfsk.cxx:335`). Sources: `mfsk.cxx:177-295`,
`globals/globals.cxx:101-112`. The carrier (fldigi's "frequency") is the centre of the tones
and must lie in `bandwidth/2 .. 3000 - bandwidth/2` Hz (`modem::set_freq` with fldigi's
waterfall limits); a receiver or transmitter outside is refused at create.

## Pictures

fldigi sends a picture inside an MFSK over (MFSK16/32/64/128 offer it): the text header
`Pic:WxH;` (`C` for colour, `p4`/`p2` for 4 or 2 samples per pixel), a flush, a 352-sample
prologue, then the pixels as an analog FM tone (`carrier + bandwidth (pixel - 128) / 256`,
8 samples per pixel), a flush, and text again. fldigi's receiver (`mfsk.cxx:372-529`,
`:877-895`) watches every printed character in a 63-character window; after a valid header it
waits a mode-specific delay, stops symbol decoding for `spp x W x H (x 3)` samples, demodulates
the pixels with a phase discriminator and then resumes.

The core keeps this state machine exactly, so a picture over does not turn into garbage text
and the text after it is decoded as fldigi decodes it. Rows (one line of one colour channel)
go into a ring of 32 rows; `fmfsk_rx_poll_picture` takes the oldest, an unpolled ring drops
the oldest (`rows_dropped`). `fmfsk_rx_picture_info` reports pictures started and completed,
size, colour and samples per pixel. fldigi never receives the very last pixel (the counter
ends one pixel early); the last row is handed over with that pixel zero. With fldigi's MFSK64
start delay the picture lands one pixel late; the port keeps that.

The transmitter refuses `0x05` (fldigi's picture trigger) and any keyboard byte that would
complete a header that a receiver would accept after any `Pic:` in its window, so text can
never switch a receiver into picture mode. A picture transmitter exists only as an internal
test helper (`fmfsk__tx_picture`, `src/mfsk_internal.h`) that makes the picture test vectors.

## Differences from fldigi

None of these changes decoded text for a single receiver; each is named in the code.

- **Statics.** fldigi keeps softdecode's CWI counters (`mfsk.cxx:603`) and eval_s2n's s/n table
  (`:69-71`) in statics shared by all MFSK instances; here they belong to one receiver.
- **Viterbi metrics.** fldigi-dsp-core renormalises the path metrics on every call (fldigi's
  overflow after ~8.4 M symbol pairs with chunk size 1; same decisions).
- **Left out.** Scopes, waterfall, GUI status, picture viewer and PNG files, PSKmail s/n
  reports, the TX band-pass `xmt_filter` (off in fldigi, `mfsk.cxx:66`), reverse.
- **Carrier.** fldigi clamps a carrier outside the waterfall limits; create refuses it here.
  AFC moves the carrier inside the limits as fldigi does.

## API reference

All declarations are in `include/fldigi_mfsk.h`.

| Function | Description |
|---|---|
| `fmfsk_version()`, `fmfsk_abi()` | `"fldigi-mfsk-core 0.1.1 contract=mfsk-c1 abi=1 tx=1"`, 1 |
| `fmfsk_submode_count()`, `fmfsk_submode_at(i)`, `fmfsk_submode_find(name)` | The 11 submodes (table above), case-insensitive lookup. |
| `fmfsk_rx_create(const fmfsk_rx_config *)` | `submode`, `carrier_hz`, `input_rate` (0: the submode's rate; else converted inside), `afc`, `squelch` (0 off, else 0..100). NULL for an unknown submode, a carrier out of range, an unsupported rate. |
| `fmfsk_rx_process(rx, x, n, text, max)` | Feed samples; returns the bytes fldigi prints (overflow dropped and counted, `fmfsk_rx_dropped`). |
| `fmfsk_rx_snr_db`, `_metric`, `_offset_hz`, `_carrier_hz` | fldigi's s/n display (max of the last 100 symbols, -99 for none), metric 0..100, the AFC's carrier and its offset from the configured one. |
| `fmfsk_rx_set_carrier`, `_set_afc`, `_set_squelch`, `_reset`, `_destroy` | |
| `fmfsk_rx_picture_info(rx, &info)`, `fmfsk_rx_poll_picture(rx, &row, pixels, max)` | Picture state and the ring of rows (see [Pictures](#pictures)). |
| `fmfsk_rx_take_tones`, `fmfsk_rx_take_softbits` | Test taps: the hard tones and the de-interleaved soft bits since the last call (at most 1024 / 4096). |
| `fmfsk_tx_create(const fmfsk_tx_config *)` | `submode`, `carrier_hz`, `output_rate` (0: the submode's rate), `no_soft_edges`. |
| `fmfsk_tx_bound(tx)` | The most samples one of the calls below produces; give every call a buffer that large. |
| `fmfsk_tx_begin`, `_put(c)`, `_idle`, `_end` | The state steps above; each returns the samples written (0: buffer too small, out of order, or a refused byte). |
| `fmfsk_tx_refused(tx)` | Bytes `_put` refused (0x05, picture headers). |

## Usage example

```c
#include <stdio.h>
#include <stdlib.h>
#include "fldigi_mfsk.h"

int main(void)
{
    fmfsk_tx_config tc = {"MFSK16", 1500.0, 0, 0};
    fmfsk_rx_config rc = {"MFSK16", 1500.0, 0, 0, 0.0};
    fmfsk_tx *tx = fmfsk_tx_create(&tc);
    fmfsk_rx *rx = fmfsk_rx_create(&rc);
    size_t bound = fmfsk_tx_bound(tx), n, i;
    double *buf = malloc(bound * sizeof *buf);
    unsigned char out[512];
    const char *text = "CQ DE DG6FL K";

    n = fmfsk_tx_begin(tx, buf, bound);
    fmfsk_rx_process(rx, buf, n, out, sizeof out);
    for (i = 0; text[i]; i++) {
        n = fmfsk_tx_put(tx, (unsigned char)text[i], buf, bound);
        fwrite(out, 1, fmfsk_rx_process(rx, buf, n, out, sizeof out), stdout);
    }
    n = fmfsk_tx_end(tx, buf, bound);   /* the flush pushes the rest out */
    fwrite(out, 1, fmfsk_rx_process(rx, buf, n, out, sizeof out), stdout);
    free(buf);
    fmfsk_tx_destroy(tx);
    fmfsk_rx_destroy(rx);
    return 0;
}
```

wave-owl binds the library with ctypes in `src/mfsk_native.py`.

## Contract and versioning

`fmfsk_version()` reports `contract=mfsk-c1 abi=1` and the capability tag `tx=1`. The
contract changes only with an incompatible change of the structs or functions; additions keep
it and raise the minor version. wave-owl refuses a library with another contract or ABI.

## Verification

`make test` runs two programs:

- `tests/test_mfsk.c` (independent of fldigi): submode table and argument checks; TX -> RX
  loopback for all 11 submodes at the modem rate, at 8 kHz and (up to MFSK16) at 48 kHz
  through the resampler; chunk invariance of text and hard tones (random chunks of 1 to 4000
  samples); picture header refusal (0x05, `Pic:64x64;`, `Pic:10x10Cp4;`); the picture ring
  dropping the oldest rows; AFC pulling in a 3 Hz offset; s/n, metric, squelch, reset,
  retuning; two receivers interleaved give the result of one alone; idles.
- `tests/test_mfsk_fldigi.c`: goldens from the fldigi 4.2.13 bench build
  (`tests/golden/make_fldigi_golden.py`; headless fldigi with the TX extension of wave-owl's
  archived change `fix-psk-rtty-vs-fldigi`):
  - 13 overs (11 submodes with `CQ CQ DE DG6FL DG6FL K`, MFSK16 and MFSK22 pangrams) and 5
    AFC overs (fldigi TX 1 .. 6 Hz off the receiver, fldigi RX with `--benchmark-afc 1`). TX:
    the FNV-1a hash of the int16 output equals fldigi's, stored vectors match. RX: 54 decodes
    (clean, -5 dB, -10 dB in 2500 Hz; deterministic noise rebuilt bit for bit in C) equal
    fldigi's output byte for byte.
  - 8 picture overs (MFSK16/32/64/128, grey and colour; `tests/golden/make_picture_golden.sh`):
    fldigi's bench decodes the over made by the test helper; the port prints the same bytes,
    reports the picture and returns every row with the transmitted pixels (within the
    smearing of the receiver's band-pass).

| | Result (macOS arm64, Apple clang, `-O3` and the `-O1` sanitizer build) |
|---|---|
| TX overs identical to fldigi | 18 of 18 |
| RX decodes identical to fldigi | 54 of 54 (incl. 15 with AFC) |
| Picture overs: text identical to fldigi | 8 of 8 |

On other platforms (another libm, x86 without FMA) the results are not bit-identical; there
the clean decodes must stay within 1 + 5 % edit distance of fldigi's and the noisy ones in
total within 20 % + 10 errors. The reference build is reproduced bit for bit because the
library is compiled with `-ffp-contract=on` (fusing the same scalar expressions as fldigi's
Apple clang -O2 build), keeps `std::complex` products unfused and evaluates `sin`/`cos`
pairs with one `sincos` call. CI runs both programs on Ubuntu and macOS and again under
AddressSanitizer and UndefinedBehaviorSanitizer.

## Performance and memory

`make bench` (Apple M4, one core), multiples of real time:

| | MFSK4 | MFSK8 | MFSK11 | MFSK16 | MFSK22 | MFSK31 | MFSK32 | MFSK64 | MFSK128 | MFSK64L | MFSK128L |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Receiver | 721 | 1010 | 827 | 1125 | 821 | 1230 | 1087 | 1086 | 1068 | 996 | 642 |
| Receiver from 8 kHz | 943 | 967 | 718 | 1098 | 719 | 1186 | 1141 | 1111 | 1054 | 953 | 645 |
| Transmitter | 14 800 | 37 900 | 27 300 | 37 000 | 25 900 | 33 200 | 32 500 | 18 700 | 17 200 | 2 800 | 740 |

Memory per receiver: the two-symbol pipe (2 x symlen x 32 complex bins: 2 MiB for MFSK4,
0.5 MiB for MFSK16), the picture ring (132 KiB), two K=7 Viterbi trellises and the
interleaver; 0.45 .. 2.5 MiB in total.

## Building

```
git submodule update --init      # dsp/ = fldigi-dsp-core, pinned in dsp.lock
make                             # build/libfldigi_mfsk.{a,dylib|so}
make test                        # unit tests and fldigi goldens
make sanitize                    # the same under ASan + UBSan
make bench                       # CPU benchmark
make BUILD=/abs/dir shared       # out-of-tree build (wave-owl: make build-mfsk-core)
```

Plain make, C99, `-Wall -Wextra -Werror -pedantic`, no dependencies besides libm. The build
first checks the `dsp/` submodule against `dsp.lock` (version, commit, content checksum;
wave-owl R-920.3) and refuses to compile against anything else.

## Maintenance and fldigi updates

wave-owl's `scripts/fldigi_upstream_check.py` (skill `fldigi-upstream-sync`) checks every
`file:line` citation of this repository against a new fldigi tag. After a bump: rerun
`tests/golden/make_fldigi_golden.py` and `tests/golden/make_picture_golden.sh` with the new
bench build and `make test`.

## License and credits

GPL-3.0-or-later, as fldigi. MFSK by Dave Freese W1HKJ and John Douyere VK2ETA (the high
speed and L modes) in fldigi 4.2.13, after gmfsk by Tomi Manninen OH2BNS; MFSK varicode by
Nino Porcino IZ8BLY. The C port and the tests: Gerolf Ziegenhain DG6FL, 2026. See
`CHANGELOG.md`.
