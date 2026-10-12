#!/usr/bin/env python3
"""Recalibrate per-mode loudness trims to -20 LUFS from 1-hour renders.

    python tools/calibrate_trims.py [--minutes 60]

Renders each mode at default settings (seed 123), measures integrated loudness with
tools/analyze_audio.py, and rewrites `#define TRIM` in fw/composer/mode_*.c.
Rebuild afterwards.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENDER = os.path.join(ROOT, "build", "render.exe" if os.name == "nt" else "render")
FILES = {"drift": "fw/composer/mode_drift.c", "tides": "fw/composer/mode_tides.c",
         "hush": "fw/composer/mode_hush.c", "mother": "fw/composer/mode_mother.c"}
# MOTHER is sparse and peaky: -21.5 LUFS keeps its peaks out of the limiter (still -20 +/- 3)
TARGET = {"mother": -21.5}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=int, default=60)
    args = ap.parse_args()
    tmp = tempfile.mkdtemp()
    procs = []
    for m in FILES:
        procs.append(subprocess.Popen([RENDER, "--mode", m, "--seed", "123", "--minutes",
                                       str(args.minutes), "--out", os.path.join(tmp, f"{m}.wav"),
                                       "--quiet"]))
    for p in procs:
        if p.wait():
            sys.exit("render failed")
    rep = os.path.join(tmp, "rep")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "analyze_audio.py"), "--no-chroma",
                    "--report", rep] + [os.path.join(tmp, f"{m}.wav") for m in FILES],
                   stdout=subprocess.DEVNULL)
    summary = json.load(open(os.path.join(rep, "summary.json"), encoding="utf-8"))
    for m, path in FILES.items():
        lufs = summary[m]["integrated_lufs"]
        full = os.path.join(ROOT, path)
        src = open(full, encoding="utf-8").read()
        mt = re.search(r"(#define TRIM\s+)(\d+)", src)
        old = int(mt.group(2))
        new = round(old * 10 ** ((TARGET.get(m, -20.0) - lufs) / 20))
        src = src[:mt.start(2)] + str(new) + src[mt.end(2):]
        with open(full, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
        print(f"{m}: {lufs:.2f} LUFS, trim {old} -> {new}")
        os.remove(os.path.join(tmp, f"{m}.wav"))


if __name__ == "__main__":
    main()
