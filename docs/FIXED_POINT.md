# Fixed-point conventions

The C model (`model/synth_model.c`) is the definition. The RTL must match it bit for bit.

## General rules

- All arithmetic is integer. Products are formed at full width (64-bit in C, DSP-block width in RTL) and then shifted.
- `>>` on a signed value is an **arithmetic** shift (floor), the same as SystemVerilog `>>>`. GCC/Clang guarantee this on every host we target.
- Every bus summation **saturates** (`sat()` in the model). Nothing wraps except phase accumulators (u32, mod 2³²) and memory indices.
- Register fields are masked to their documented width on read (`& 0xFFFF` etc.), so the upper bits are don't-care.

## Formats

| Signal | Format | Notes |
|---|---|---|
| Audio samples (voice, buses) | Q1.23 in int32 | full scale ±2²³; buses have 8 bits of headroom before saturation |
| SVF states | Q1.23, saturate at ±2²⁷ | |
| Envelope level, mod envelope | Q1.30 | 1.0 = 2³⁰; attack target 1.25 |
| Phase | u32 | 2³² = one cycle |
| Sine LUT | 4096+2 quarter-wave int16 (32767 peak) | 12-bit index + 8-bit linear interp → Q1.23 |
| Wavetables | 10 × 1024 int16 per shape | 10-bit index + 8-bit linear interp → Q1.23 |
| Rate code | `exp[15:12] mant[11:0]` | coef = `(0x1000 \| mant) << exp`; step = `(target - level) * coef >> 32` |
| `VR_CUTOFF` | U0.16 | f = 2 sin(π fc / 96000), clamped ≤ 52429 (0.8) |
| `VR_RESO` | U2.14 | damping q; clamped [1024, 32767] |
| `VR_AMP`, sends, `VR_WAVE`, `VR_SUSTAIN` | U1.15 | 32768 = 1.0 |
| `VR_PAN` | 0…32768 | equal-power: gR = sin(pan·π/2/32768), gL = cos |
| `VR_FM_INDEX`, `VR_FM_SUS` | U4.12 | 1.0 = phase deviation of one full cycle (2π rad) |
| `VR_KS_LEN` | U16.16 samples | clamped [2, 2040] |
| `VR_PENV` | u32 | effective inc0 = PINC0 + (PENV × mod_env) >> 30 |
| Slewed params | value << 8 | `s += ((target << 8) - s) >> 10` (τ ≈ 21 ms) |
| Reverb/echo memory | int16 (Q1.15) | written as `round(x23 / 256)`, saturated |
| Reverb line gains, damping | U0.16 | |
| HPF state | int64, value << 16 | coefficient 14055215 = 2π·25/48000 · 2³² |
| Master fade gain | Q1.23 | linear ramp of `FX_MASTER_STEP` per sample, applied squared |
| Trim | U2.14 | 16384 = 1.0 |
| Limiter table | 1025 × int32 Q1.23 | index `(|x| - knee) >> 15`, 8-bit interp |
| Output | int16 | `(x + tpdf + 128) >> 8`, saturated to ±32767 |

## Per-sample order (one sample)

1. Voices 0…31 in order: gate/trigger → slews → envelope → mod envelope → source → phase advance → SVF ×2 → `× env` → `× amp` → pan/sends → saturating accumulate into dry L/R, reverb send, echo send.
2. Echo: read L/R → damp → write (ping-pong) → outputs.
3. Reverb: input = rev bus + echo mono × echo_to_rev → pre-delay → 4 allpasses → 8 lines (read → damp → gain) → Householder → write.
4. Master: dry·DRY + rev·WET + echo·WET → HPF → trim → fade² → limiter → dither → int16.

## PRNGs inside the engine

- Per-voice xorshift32 (seed `0x9E3779B9 × (v+1) | 1`), advanced once per sample by NOISE and KS voices, and twice on a PAD trigger from IDLE (start phases).
- Dither: one xorshift32 (seed `0x2545F491`) advanced once per sample; bytes 0+1 → L, 2+3 → R.
