#!/usr/bin/env python3
"""Compare a render's octave-band spectral balance and stereo width with a reference clip.

    python tools/compare_reference.py reference/MOTHER.wav render.wav [--skip-ref 9] [--skip 10]

Band levels are relative to each file's total energy, so overall loudness doesn't matter.
"""
import argparse
import numpy as np
import soundfile as sf
from scipy import signal

BANDS = [(0, 40), (40, 80), (80, 160), (160, 320), (320, 640), (640, 1280), (1280, 2560),
         (2560, 5120), (5120, 10240), (10240, 24000)]


def profile(path, skip):
    x, fs = sf.read(path, always_2d=True)
    m = x.mean(1)[int(skip * fs):]
    f, p = signal.welch(m, fs=fs, nperseg=32768)
    tot = p.sum()
    lv = [10 * np.log10(p[(f >= lo) & (f < hi)].sum() / tot + 1e-20) for lo, hi in BANDS]
    corr = np.corrcoef(x[:, 0], x[:, 1])[0, 1] if x.shape[1] > 1 else 1.0
    return lv, corr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("render")
    ap.add_argument("--skip-ref", type=float, default=0)
    ap.add_argument("--skip", type=float, default=10)
    a = ap.parse_args()
    r, rc = profile(a.reference, a.skip_ref)
    o, oc = profile(a.render, a.skip)
    for (lo, hi), x, y in zip(BANDS, r, o):
        print(f"{lo:6d}-{hi:6d} Hz  ref {x:6.1f}  render {y:6.1f}  diff {y - x:+5.1f} dB")
    print(f"L/R correlation  ref {rc:.2f}  render {oc:.2f}")


if __name__ == "__main__":
    main()
