#!/usr/bin/env bash
# Randomised RTL verification (spec 8.4): N parallel fuzz streams vs the C model.
#   sim/run_fuzz.sh [jobs] [samples_per_job]      default 12 x 850000 = 10.2 M samples
#   TB=audio sim/run_fuzz.sh ...                   + random FX writes, checks the output too
set -uo pipefail
cd "$(dirname "$0")/.."
JOBS=${1:-12}; PER=${2:-850000}
export PATH="/c/iverilog/bin:$PATH"
mkdir -p sim/vectors sim/out
SYN=src/fpga/core/synth
if [ "${TB:-voice}" = audio ]; then
  TOP=tb_audio; SRCS="sim/tb/tb_audio.sv $SYN/audio_engine.sv $SYN/voice_engine.sv $SYN/fx_engine.sv"
else
  TOP=tb_voice_engine; SRCS="sim/tb/tb_voice_engine.sv $SYN/voice_engine.sv"
fi
iverilog -g2012 -s $TOP -o sim/out/$TOP.vvp $SRCS || exit 1
for j in $(seq 1 "$JOBS"); do
  ( ./build/fuzz${EXE-.exe} sim/vectors/fz$j "$PER" $((j * 2654435761 % 4294967291)) ${TB:-voice}
    cd src/fpga && vvp -n ../../sim/out/$TOP.vvp \
      +writes=../../sim/vectors/fz${j}_writes.txt +bus=../../sim/vectors/fz${j}_bus.txt > ../../sim/out/fz$j.log 2>&1
    rm -f ../../sim/vectors/fz${j}_*.txt ) &
done
wait
grep -h -E "^(PASS|FAIL)" sim/out/fz*.log
grep -h "MISMATCH" sim/out/fz*.log | head -5
total=$(grep -h "^PASS" sim/out/fz*.log | awk '{s += $3} END {print s+0}')
echo "== fuzz: $(grep -l '^PASS' sim/out/fz*.log | wc -l)/$JOBS streams pass, $total bit-exact samples"
! grep -q "^FAIL" sim/out/fz*.log
