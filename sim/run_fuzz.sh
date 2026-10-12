#!/usr/bin/env bash
# Randomised RTL verification (spec 8.4): N parallel fuzz streams vs the C model.
#   sim/run_fuzz.sh [jobs] [samples_per_job]      default 12 x 850000 = 10.2 M samples
#   TB=audio sim/run_fuzz.sh ...                   + random FX writes, checks the output too
#   SEED0=100 sim/run_fuzz.sh ...                  other seeds (stream j uses seed index SEED0 + j)
#   TAG=soak sim/run_fuzz.sh ...                   file prefix, so several runs can go at once
#   SIM=verilator sim/run_fuzz.sh ...              same testbench compiled with Verilator (--timing),
#                                                  ~30x faster than Icarus; Icarus stays the default
#                                                  (4-state: X in the RTL fails the !== compare)
set -uo pipefail
cd "$(dirname "$0")/.."
JOBS=${1:-12}; PER=${2:-850000}
SEED0=${SEED0:-0}; TAG=${TAG:-fz}
export PATH="/c/iverilog/bin:$PATH"
mkdir -p sim/vectors sim/out
SYN=src/fpga/core/synth
if [ "${TB:-voice}" = audio ]; then
  TOP=tb_audio; SRCS="sim/tb/tb_audio.sv $SYN/audio_engine.sv $SYN/voice_engine.sv $SYN/fx_engine.sv"
else
  TOP=tb_voice_engine; SRCS="sim/tb/tb_voice_engine.sv $SYN/voice_engine.sv"
fi
if [ "${SIM:-icarus}" = verilator ]; then
  verilator --binary --timing -j 0 -O3 --x-assign fast --x-initial fast -Wno-fatal -Wno-lint -Wno-style \
    -Wno-TIMESCALEMOD --top-module $TOP --Mdir sim/out/${TAG}_obj -CFLAGS "-O2" $SRCS \
    > sim/out/${TAG}_build.log 2>&1 || { tail -20 sim/out/${TAG}_build.log; exit 1; }
  RUN=(../../sim/out/${TAG}_obj/V$TOP)
else
  iverilog -g2012 -s $TOP -o sim/out/${TAG}_$TOP.vvp $SRCS || exit 1
  RUN=(vvp -n ../../sim/out/${TAG}_$TOP.vvp)
fi
for j in $(seq 1 "$JOBS"); do
  ( ./build/fuzz${EXE-.exe} sim/vectors/$TAG$j "$PER" $(((SEED0 + j) * 2654435761 % 4294967291)) ${TB:-voice}
    cd src/fpga && "${RUN[@]}" \
      +writes=../../sim/vectors/$TAG${j}_writes.txt +bus=../../sim/vectors/$TAG${j}_bus.txt > ../../sim/out/$TAG$j.log 2>&1
    rm -f ../../sim/vectors/$TAG${j}_*.txt ) &
done
wait
logs=$(seq -f "sim/out/$TAG%g.log" 1 "$JOBS")
grep -h -E "^(PASS|FAIL)" $logs
grep -h "MISMATCH" $logs | head -5
total=$(grep -h "^PASS" $logs | awk '{s += $3} END {print s+0}')
echo "== fuzz: $(grep -l '^PASS' $logs | wc -l)/$JOBS streams pass, $total bit-exact samples"
! grep -q "^FAIL" $logs
