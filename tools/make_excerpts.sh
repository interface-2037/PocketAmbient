#!/usr/bin/env bash
# Listening-review material (spec 8.5): for each mode, 3 x 10-minute excerpts taken
# at the start, 2 h and 6 h into the same seed-123 session, plus a mode-switch montage.
#   tools/make_excerpts.sh [outdir] [jobs]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-renders/listening}
JOBS=${2:-9}
RENDER=./build/render${EXE-.exe}
[ -x "$RENDER" ] || RENDER=./build/render
mkdir -p "$OUT"
{
  for m in drift tides hush mother; do
    echo "--mode $m --seed 123 --minutes 10 --out $OUT/${m}_0h.wav"
    echo "--mode $m --seed 123 --minutes 10 --from 7200 --out $OUT/${m}_2h.wav"
    echo "--mode $m --seed 123 --minutes 10 --from 21600 --out $OUT/${m}_6h.wav"
  done
  # 3 min montage: DRIFT -> TIDES at 45 s -> HUSH at 90 s -> MOTHER at 135 s, stop at 175 s
  echo "--mode drift --seed 123 --seconds 180 --press 45:RIGHT,90:RIGHT,135:RIGHT,175:A --out $OUT/montage.wav"
} | xargs -P "$JOBS" -L 1 "$RENDER" --quiet
ls -la "$OUT"
