#!/usr/bin/env bash
# Mutation check: each deliberate one-token RTL change must make the fuzz vectors FAIL.
#   sim/mutate.sh [fuzz_samples]   (default 200000)
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/iverilog/bin:$PATH"
SRC=src/fpga/core/synth/voice_engine.sv
T=sim/out/mut; mkdir -p $T sim/vectors
NS=${1:-200000}
./build/fuzz${EXE-.exe} sim/vectors/mut "$NS" 99
muts=(
  's/ENV_FLOOR = 32'"'"'sd64;/ENV_FLOOR = 32'"'"'sd65;/'
  's/pann\[0\] <= u + ((t - u) >>> 10);/pann[0] <= u + ((t - u) >>> 9);/'
  's/SVF_F_MAX = 32'"'"'sd52429;/SVF_F_MAX = 32'"'"'sd52428;/'
  's/x\[2\] <= (t + u) >>> 1;/x[2] <= (t + u) >> 1;/'
  's/if (t < SVF_Q_MIN) t = SVF_Q_MIN;/if (t < SVF_Q_MIN) t = SVF_Q_MIN + 1;/'
  's/else u = dust0\[1\] - (dust0\[1\] >>> 7);/else u = dust0[1] - (dust0[1] >>> 6);/'
  's/ksw\[1\] <= ksw\[1\] + 11'"'"'d1;/ksw[1] <= ksw[1] + 11'"'"'d2;/'
  's/ph0p\[1\] + (wave_c\[1\] << 17)/ph0p[1] + (wave_c[1] << 16)/'
  's/sin_rd = {ph\[31\], i\[0\], p\[7:0\], i1\[12:1\], i\[12:1\]};/sin_rd = {ph[31], i[0], p[7:0], i[12:1], i[12:1]};/'
  's/ma1 <= ux(incs1n\[1\]); mb1 <= sx(mm);/ma1 <= ux(incs0n[1]); mb1 <= sx(mm);/'
  's/.ctx({1'"'"'b0, ~kslo\[0\]\[0\], ksfr\[0\]})/.ctx({1'"'"'b0, kslo[0][0], ksfr[0]})/'
  's/qb1\[0\] <= lo32(p0 >>> 14);/qb1[0] <= lo32(p0 >>> 13);/'
  's/ .SH(gl)//'
  's/((hl + lh) <<< 17)/((hl + lh) <<< 16)/'
)
i=0
for m in "${muts[@]}"; do
  i=$((i + 1))
  sed "$m" $SRC > $T/ve_$i.sv
  if cmp -s $SRC $T/ve_$i.sv; then echo "DID NOT APPLY" > $T/r$i.txt; continue; fi
  ( iverilog -g2012 -s tb_voice_engine -o $T/m$i.vvp sim/tb/tb_voice_engine.sv $T/ve_$i.sv
    cd src/fpga && vvp -n ../../$T/m$i.vvp +writes=../../sim/vectors/mut_writes.txt \
      +bus=../../sim/vectors/mut_bus.txt | grep -E "^(PASS|FAIL)" > ../../$T/r$i.txt ) &
done
wait
caught=0
for j in $(seq 1 $i); do
  case "$(cat $T/r$j.txt)" in FAIL*) caught=$((caught + 1)); echo "mutation $j: caught";;
                              *) echo "mutation $j: MISSED ($(cat $T/r$j.txt))";; esac
done
echo "== mutations caught: $caught/$i"
[ $caught -eq $i ]
