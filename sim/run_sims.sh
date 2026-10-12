#!/usr/bin/env bash
# RTL regression with Icarus Verilog: replays C-model golden vectors.
#   sim/run_sims.sh [seconds] [modes...]     (default 2 s, all modes, run in parallel)
#   TB=audio sim/run_sims.sh ...              voice engine + FX/master, checks the output too
set -uo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-2}; shift || true
MODES=${*:-drift tides hush mother}
export PATH="/c/iverilog/bin:$PATH"
RENDER=./build/render${EXE-.exe}
[ -x "$RENDER" ] || RENDER=./build/render
mkdir -p sim/vectors sim/out
SYN=src/fpga/core/synth
if [ "${TB:-voice}" = audio ]; then
  TOP=tb_audio; SRCS="sim/tb/tb_audio.sv $SYN/audio_engine.sv $SYN/voice_engine.sv $SYN/fx_engine.sv"
else
  TOP=tb_voice_engine; SRCS="sim/tb/tb_voice_engine.sv $SYN/voice_engine.sv"
fi
iverilog -g2012 -s $TOP -o sim/out/$TOP.vvp $SRCS || exit 1
for m in $MODES; do
  ( "$RENDER" --mode $m --seed 77 --seconds "$SECS" --vectors "sim/vectors/$m" --quiet
    # tables load relative to src/fpga, like Quartus
    cd src/fpga && vvp -n ../../sim/out/$TOP.vvp \
      +writes=../../sim/vectors/${m}_writes.txt +bus=../../sim/vectors/${m}_bus.txt > ../../sim/out/$m.log 2>&1 ) &
done
wait
fail=0
for m in $MODES; do
  echo "== $m: $(grep -hE '^(PASS|FAIL)' sim/out/$m.log)"
  grep -h "MISMATCH" sim/out/$m.log | head -3
  grep -q "^PASS" sim/out/$m.log || fail=1
done
exit $fail
