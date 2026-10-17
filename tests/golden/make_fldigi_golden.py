#!/usr/bin/env python3
"""Golden data from fldigi 4.2.13 itself for fldigi-mfsk-core (stdlib only).

Drives the headless fldigi bench build (configure --enable-benchmark plus the TX
extension of wave-owl's archived change fix-psk-rtty-vs-fldigi, bench/BUILD.md) for every
MFSK submode of fldigi 4.2.13 (src/globals/globals.cxx:101-112):

* TX: fldigi renders the text (bench-tx, txlevel -12 dB, PCM16, modem rate). The FNV-1a 64
  hash of every vector goes to tx_hashes.txt; a subset is kept as raw little-endian int16
  in tx/<name>.s16.
* RX: fldigi decodes 1 s silence + the TX vector + tail s silence, clean and buried in
  deterministic noise at -5 and -10 dB (2500 Hz reference bandwidth, see noisy()). The raw
  bytes fldigi hands to put_rx_char() go to rx/<name>_<snr>.txt. The tail is 12 s, and
  longer for the deep interleavers of MFSK64L/128L and the slow MFSK4/8.

    FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi python3 tests/golden/make_fldigi_golden.py

The C tests (tests/test_mfsk_fldigi.c) rebuild the same noisy inputs bit for bit.
SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLDIGI = os.environ.get("FLDIGI_BENCH_BIN", "fldigi")
CARRIER_HZ = 1500
TEXTS = {"cq": "CQ CQ DE DG6FL DG6FL K",
         "pangram": "the quick brown fox jumps over the lazy dog 0123456789 <test?> 73"}

# (stem, fldigi trx_mode id = index into mode_info[], sname, text key, tail seconds)
MFSK = [("mfsk8", 38, "MFSK8"), ("mfsk16", 39, "MFSK16"), ("mfsk32", 40, "MFSK32"),
        ("mfsk4", 41, "MFSK4"), ("mfsk11", 42, "MFSK11"), ("mfsk22", 43, "MFSK22"),
        ("mfsk31", 44, "MFSK31"), ("mfsk64", 45, "MFSK64"), ("mfsk128", 46, "MFSK128"),
        ("mfsk64l", 47, "MFSK64L"), ("mfsk128l", 48, "MFSK128L")]
TAIL_S = {"mfsk4": 30, "mfsk8": 20, "mfsk64l": 30, "mfsk128l": 30}
# AFC (--benchmark-afc 1, receiver at 1500 Hz): fldigi transmits at 1500 + offset Hz.
# (fldigi's --benchmark-frequency is an integer)
AFC = [("mfsk4_afc", 41, "MFSK4", 1), ("mfsk8_afc", 38, "MFSK8", 1),
       ("mfsk16_afc", 39, "MFSK16", 3), ("mfsk32_afc", 40, "MFSK32", -6),
       ("mfsk22_afc", 43, "MFSK22", 4)]
STORED = {"mfsk4", "mfsk11", "mfsk16", "mfsk31", "mfsk128", "mfsk64l"}
SNRS = ("clean", "m05", "m10")


def cases():
    out = [(stem, mid, sname, "cq") for stem, mid, sname in MFSK]
    out.append(("mfsk16_pangram", 39, "MFSK16", "pangram"))
    out.append(("mfsk22_pangram", 43, "MFSK22", "pangram"))
    return out


def afc_cases():
    return [(stem, mid, sname, "cq", off) for stem, mid, sname, off in AFC]


def tail_s(stem):
    return TAIL_S.get(stem.split("_")[0], 12)


def run_fldigi(mid, sname, args, env_extra=None, freq=CARRIER_HZ):
    cfg = tempfile.mkdtemp(prefix="fldigi-mfsk-golden-")
    Path(cfg, "fldigi_def.xml").write_text("<FLDIGI_DEFS>\n</FLDIGI_DEFS>\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLDIGI_BENCH_TX")}
    env.update(env_extra or {})
    try:
        p = subprocess.run([FLDIGI, "--config-dir", cfg, "--benchmark-modem", str(mid),
                            "--benchmark-frequency", str(freq), *args],
                           env=env, capture_output=True, text=True, timeout=3600)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if f"mode={sname} " not in p.stderr:
        raise RuntimeError(f"fldigi did not run {sname}: {p.stderr[-400:]}")
    return p.stderr


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        sr = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return sr, a


def write_wav(path, sr, pcm):
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(a.tobytes())


def fnv1a64(pcm):
    h = 0xCBF29CE484222325
    for v in pcm:
        u = v & 0xFFFF
        for byte in (u & 0xFF, u >> 8):
            h ^= byte
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lcg:
    """32-bit LCG (Numerical Recipes); the same in tests/test_mfsk_fldigi.c."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def uniform(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def noisy(pcm, sr, snr, seed, tail):
    """1 s + pcm + tail s, plus Irwin-Hall Gaussian noise at -5 or -10 dB SNR in 2500 Hz.

    Only +, -, *, / and sqrt (all correctly rounded), so C reproduces it bit for bit.
    SNR = mean square of the burst / (noise variance * 2500 / (sr / 2)).
    """
    lead = sr
    padded = [0.0] * lead + [float(v) for v in pcm] + [0.0] * (tail * sr)
    if snr == "clean":
        return [int(v) for v in padded]
    ps = 0.0
    for v in pcm:
        ps += float(v) * float(v)
    ps = ps / len(pcm)
    inv = 1.0 / math.sqrt(10.0) if snr == "m05" else 1.0 / 10.0
    s2 = ps / inv * (sr / 2.0) / 2500.0
    sd = math.sqrt(s2)
    gain = 3276.7 / math.sqrt(ps * len(pcm) / len(padded) + s2)
    rng = Lcg(seed)
    out = []
    for v in padded:
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        y = (v + sd * z) * gain
        q = math.floor(y + 0.5)
        out.append(max(-32768, min(32767, int(q))))
    return out


def one(idx, case):
    stem, mid, sname, key = case
    with tempfile.TemporaryDirectory() as td:
        wav = Path(td, stem + ".wav")
        run_fldigi(mid, sname, ["--benchmark-input", "1"],
                   {"FLDIGI_BENCH_TX_TEXT": TEXTS[key], "FLDIGI_BENCH_TX_WAV": str(wav)})
        sr, pcm = read_wav(wav)
        line = (f"{stem} {sname} {key} {idx} {sr} {len(pcm)} {tail_s(stem)} "
                f"{fnv1a64(pcm):016x}")
        if stem in STORED or key == "pangram":
            raw = array.array("h", pcm)
            if sys.byteorder == "big":
                raw.byteswap()
            Path(HERE, "tx", stem + ".s16").write_bytes(raw.tobytes())
        for i, snr in enumerate(SNRS):
            inp = Path(td, f"{stem}_{snr}.wav")
            write_wav(inp, sr, noisy(pcm, sr, snr, 1000 * idx + i, tail_s(stem)))
            out = Path(td, f"rx_{snr}.txt")
            run_fldigi(mid, sname, ["--benchmark-input", str(inp),
                                    "--benchmark-output", str(out)])
            text = out.read_bytes() if out.exists() else b""
            Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(text)
    print(line, flush=True)
    return line


def one_afc(idx, case):
    """TX at 1500 + offset Hz, RX with AFC at 1500 Hz; noise seed 1000 * idx + step."""
    stem, mid, sname, key, off = case
    with tempfile.TemporaryDirectory() as td:
        wav = Path(td, stem + ".wav")
        run_fldigi(mid, sname, ["--benchmark-input", "1"],
                   {"FLDIGI_BENCH_TX_TEXT": TEXTS[key], "FLDIGI_BENCH_TX_WAV": str(wav)},
                   freq=CARRIER_HZ + off)
        sr, pcm = read_wav(wav)
        raw = array.array("h", pcm)
        if sys.byteorder == "big":
            raw.byteswap()
        Path(HERE, "tx", stem + ".s16").write_bytes(raw.tobytes())
        line = (f"{stem} {sname} {key} {idx} {sr} {len(pcm)} {tail_s(stem)} {off} "
                f"{fnv1a64(pcm):016x}")
        for i, snr in enumerate(SNRS):
            inp = Path(td, f"{stem}_{snr}.wav")
            write_wav(inp, sr, noisy(pcm, sr, snr, 1000 * idx + i, tail_s(stem)))
            out = Path(td, f"rx_{snr}.txt")
            run_fldigi(mid, sname, ["--benchmark-input", str(inp), "--benchmark-afc", "1",
                                    "--benchmark-output", str(out)])
            text = out.read_bytes() if out.exists() else b""
            Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(text)
    print(line, flush=True)
    return line


def main():
    (HERE / "tx").mkdir(exist_ok=True)
    (HERE / "rx").mkdir(exist_ok=True)
    only = set(sys.argv[1:])
    todo = [(i, c) for i, c in enumerate(cases()) if not only or c[0] in only]
    with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "8"))) as ex:
        lines = list(ex.map(lambda a: one(*a), todo))
    base = len(cases())
    todo_afc = [(base + i, c) for i, c in enumerate(afc_cases()) if not only or c[0] in only]
    with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "8"))) as ex:
        afc_lines = list(ex.map(lambda a: one_afc(*a), todo_afc))
    if only:
        return 0
    (HERE / "afc.txt").write_text(
        "# stem fldigi-sname text index samplerate samples tail-s tx-offset-Hz fnv1a64 -- "
        "fldigi TX at 1500 + offset, fldigi RX at 1500 with --benchmark-afc 1\n"
        + "\n".join(afc_lines) + "\n")
    header = ("# stem fldigi-sname text index samplerate samples tail-s fnv1a64(int16 LE)"
              " -- fldigi 4.2.13 bench TX, txlevel -12 dB; noise seed = 1000 * index + snr"
              " step\n")
    (HERE / "tx_hashes.txt").write_text(header + "\n".join(lines) + "\n")
    (HERE / "texts.txt").write_text("".join(f"{k} {v}\n" for k, v in TEXTS.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
