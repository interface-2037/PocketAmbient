#!/usr/bin/env bash
# Full section 8.3 render + analysis matrix. Usage:
#   tools/run_validation.sh [quick|full] [jobs]
# quick: 10 min per mode/corner/seed (CI per-push); full: 8 h per mode, 1 h corners/seeds (nightly).
# WAVs are deleted after analysis; reports/<date>-<level>/ keeps JSON summaries, plots and traces.
set -euo pipefail
cd "$(dirname "$0")/.."
LEVEL=${1:-quick}
JOBS=${2:-8}
PY=${PYTHON:-python}
RENDER=./build/render${EXE-.exe}
[ -x "$RENDER" ] || RENDER=./build/render
MODES=${MODES:-drift tides hush mother}
OUT=reports/$(date +%Y-%m-%d)-$LEVEL${TAG:+-$TAG}
WORK=$OUT/work
mkdir -p "$WORK"

if [ "$LEVEL" = full ]; then LONG="--hours 8"; SHORT="--hours 1"; else LONG="--minutes 10"; SHORT="--minutes 10"; fi

jobs_file=$WORK/jobs.txt
: > "$jobs_file"
for m in $MODES; do
  echo "$m-default --mode $m --seed 123 $LONG" >> "$jobs_file"
  for c in "1 1" "1 5" "5 1" "5 5"; do
    set -- $c
    echo "$m-d$1b$2 --mode $m --seed 456 --density $1 --brightness $2 $SHORT" >> "$jobs_file"
  done
  for s in 1 2 3 4 5; do
    echo "$m-seed$s --mode $m --seed $((s * 7919)) $SHORT" >> "$jobs_file"
  done
  # counter overflow: start 10 s before 2^32 and 2^31 ticks
  echo "$m-wrap32 --mode $m --seed 5 --minutes 10 --tick-start 4294957296" >> "$jobs_file"
  echo "$m-wrap31 --mode $m --seed 5 --minutes 10 --tick-start 2147473648" >> "$jobs_file"
done
# switching stress: mode switch / reseed every 5 s for 30 min (spec 8.3 clicks row)
presses=""
for ((t = 5; t < 1800; t += 5)); do
  b=$([ $((t / 5 % 3)) = 0 ] && echo B || echo RIGHT)
  presses="$presses${presses:+,}$t:$b"
done
[ "$MODES" = "drift tides hush mother" ] && echo "switch-stress --mode drift --seed 9 --minutes 30 --density 5 --press $presses" >> "$jobs_file"

run_one() {
  name=$1; shift
  "$RENDER" "$@" --out "$WORK/$name.wav" --events "$WORK/$name.csv" --quiet
  extra=""; [ "$name" = switch-stress ] && extra=--stress
  "$PY" tools/analyze_audio.py "$WORK/$name.wav" --report "$OUT/$name" --plots $extra > "$OUT/$name.txt" 2>&1 \
    && echo "PASS $name" || echo "FAIL $name"
  rm -f "$WORK/$name.wav"
}
export -f run_one
export RENDER PY OUT WORK
xargs -P "$JOBS" -L 1 bash -c 'run_one "$@"' _ < "$jobs_file" | tee "$OUT/results.txt"

# determinism: two renders with the same seed are byte-identical
"$RENDER" --mode tides --seed 42 --minutes 3 --out "$WORK/det1.raw" --quiet
"$RENDER" --mode tides --seed 42 --minutes 3 --out "$WORK/det2.raw" --quiet
if cmp -s "$WORK/det1.raw" "$WORK/det2.raw"; then echo "PASS determinism" | tee -a "$OUT/results.txt";
else echo "FAIL determinism" | tee -a "$OUT/results.txt"; fi
rm -f "$WORK"/det*.raw

# cross-mode loudness at default settings
for m in $MODES; do grep -h "integrated_lufs" "$OUT/$m-default.txt"; done | tee -a "$OUT/results.txt"
echo "== $(grep -c PASS "$OUT/results.txt") passed, $(grep -c FAIL "$OUT/results.txt" || true) failed  ($OUT)"
! grep -q FAIL "$OUT/results.txt"
