# Changelog

All notable changes to fldigi-mfsk-core. Versions follow semantic versioning; the contract
tag `mfsk-c1` changes only with an incompatible API change.

## 0.1.1 - 2026-10-17

- `rx_init` and `reset_afc` cite the fldigi lines they port (wave-owl's citation gate,
  R-830.2). No functional change.

## 0.1.0 - 2026-10-17

First release, for wave-owl change `add-fldigi-mfsk-core` (umbrella
`add-fldigi-native-modes`).

- Receiver and transmitter for all 11 MFSK submodes of fldigi 4.2.13 (MFSK4, 8, 11, 16, 22,
  31, 32, 64, 128, 64L, 128L), ported from `mfsk.cxx`: Hilbert front end, mixer, band pass,
  sliding DFT, hard and soft decisions with CWI suppression, de-interleaver, K=7 Viterbi with
  the metric vote of the odd-bit modes, MFSK varicode, symbol timing, optional AFC, squelch,
  s/n and metric; fldigi's TX framing, idles, flush and soft start/stop. Capability tag
  `tx=1`.
- fldigi's picture receive state machine with a bounded ring of rows; keyboard TX refuses
  0x05 and picture headers.
- Rate conversion (8 kHz station audio for MFSK11/22) through fldigi-dsp-core's resampler.
- Built on fldigi-dsp-core 0.3.0 (`dsp/`, pinned by `dsp.lock`).
- Goldens from the fldigi 4.2.13 bench: 18 TX overs and 54 RX decodes (15 with AFC) identical
  to fldigi on the reference platform, also in the sanitizer build; 8 picture overs decoded
  as fldigi decodes them; unit tests; CPU benchmark.
