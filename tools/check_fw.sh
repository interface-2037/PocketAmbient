#!/usr/bin/env bash
# Firmware check without RTL: the composer firmware on the RV32 instruction-set simulator
# (model/rvsim.c) must produce exactly the host render's audio, for every mode.
#   tools/check_fw.sh [seconds] [firmware]     (default 60 s, src/fpga/core/cpu/composer.hex)
#   PRESS=45:RIGHT,90:A tools/check_fw.sh ...   scripted buttons
#   SET=20:MODE=tides,40:RESEED=1 tools/check_fw.sh ...   scripted menu changes
set -uo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-60}
FW=${2:-src/fpga/core/cpu/composer.hex}
EXE=${EXE-}
mkdir -p build/fwcheck
PARGS=(); [ -n "${PRESS:-}" ] && PARGS=(--press "$PRESS")
[ -n "${SET:-}" ] && PARGS+=(--set "$SET")
for m in drift tides hush mother; do
  ( ./build/rvsim$EXE --fw "$FW" --mode $m --seed 123 --seconds "$SECS" "${PARGS[@]}" \
      --out build/fwcheck/$m.rv.raw > build/fwcheck/$m.log
    ./build/render$EXE --mode $m --seed 123 --seconds "$SECS" "${PARGS[@]}" --out build/fwcheck/$m.ref.raw --quiet ) &
done
wait
fail=0
for m in drift tides hush mother; do
  if cmp -s build/fwcheck/$m.rv.raw build/fwcheck/$m.ref.raw; then r=PASS; else r=FAIL; fail=1; fi
  echo "== $m: $r"; sed 's/^/   /' build/fwcheck/$m.log
done
rm -f build/fwcheck/*.raw
exit $fail
