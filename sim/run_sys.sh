#!/usr/bin/env bash
# Phase 4 full-system check (Verilator): cpu_sys + the real composer firmware vs model/render,
# bit-exact, one run per mode in parallel.
#   sim/run_sys.sh [seconds] [modes...]       (default 60 s, all four modes)
#   FAST=1 sim/run_sys.sh ...                  frame ticks as fast as the FIFO allows
#   PRESS=45:RIGHT,90:A sim/run_sys.sh ...      scripted buttons (render --press syntax)
#   SET=20:MODE=tides,40:RESEED=1 sim/run_sys.sh ...   scripted menu changes (model/setscript.h)
# Needs: make (build/render), make fw (src/fpga/core/cpu/composer.hex), verilator >= 5.
set -uo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-60}; shift || true
MODES=${*:-drift tides hush mother}
SEED=${SEED:-123}
RENDER=./build/render${EXE-}
[ -x "$RENDER" ] || RENDER=./build/render.exe
OBJ=sim/out/sys_obj
F=src/fpga/core
mkdir -p sim/out
verilator --cc --exe --build -j 0 -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint \
  -Wno-style -Wno-TIMESCALEMOD --top-module cpu_sys --Mdir $OBJ -CFLAGS "-O2 -march=native" \
  "$PWD/sim/tb/sim_sys.cpp" $F/cpu/cpu_sys.sv $F/ui/fb_video.sv $F/cpu/picorv32.v $F/synth/audio_engine.sv \
  $F/synth/voice_engine.sv $F/synth/fx_engine.sv > sim/out/sys_build.log 2>&1 || { tail -20 sim/out/sys_build.log; exit 1; }
PARGS=(); [ -n "${PRESS:-}" ] && PARGS=(--press "$PRESS")
[ -n "${SET:-}" ] && PARGS+=(--set "$SET")
#   FRAMES=2.5 sim/run_sys.sh ...                 also save the screen at those times (sim/out/<mode>_<ms>.ppm)
FARGS=(); [ -n "${FRAMES:-}" ] && FARGS=(--frames "$FRAMES")
for m in $MODES; do
  ( "$RENDER" --mode $m --seed $SEED --seconds "$SECS" "${PARGS[@]}" --out sim/out/sys_$m.ref.raw --quiet
    cd src/fpga && ../../$OBJ/Vcpu_sys --mode $m --seed $SEED --seconds "$SECS" "${PARGS[@]}" ${FAST:+--fast} \
      "${FARGS[@]}" --frame-prefix ../../sim/out/$m --out ../../sim/out/sys_$m.raw > ../../sim/out/sys_$m.log 2>&1 ) &
done
wait
fail=0
for m in $MODES; do
  if cmp -s sim/out/sys_$m.raw sim/out/sys_$m.ref.raw; then r=PASS; else r=FAIL; fail=1; fi
  echo "== $m: $r  $(tail -1 sim/out/sys_$m.log)"
  [ $r = PASS ] && rm -f sim/out/sys_$m.raw sim/out/sys_$m.ref.raw
done
exit $fail
