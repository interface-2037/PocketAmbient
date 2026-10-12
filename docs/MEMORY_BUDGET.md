# Memory budget

## External SRAM (256 KB = 131,072 × 16-bit words)

Allocated in this order by `sm_init()`:

| Region | Words | ms @ 48 kHz |
|---|---:|---:|
| Reverb pre-delay (max 4800 + 1) | 4,801 | 100 |
| Allpass diffusers 142 / 107 / 379 / 277 | 905 | 19 |
| FDN lines 1601, 2207, 2999, 4111, 5639, 7717, 10589, 14519 (+18 on lines 0 and 5 for modulation) | 49,418 | 1,030 |
| Echo L (max 36,000 + 1) | 36,001 | 750 |
| Echo R (max 36,000 + 1) | 36,001 | 750 |
| **Total** | **127,126** | |
| Free | 3,946 | |

Echo is capped at 750 ms per side (the spec allows up to 800 ms if it fits). Echo times are per mode: 430 / 610 ms (TIDES, HUSH), 470 ms with tape wobble (DRIFT).

### SRAM access schedule per sample

| Block | Reads | Writes |
|---|---:|---:|
| Echo (interpolated, for tape wobble) | 4 | 2 |
| Pre-delay | 1 | 1 |
| Diffusers | 4 | 4 |
| FDN (2 extra reads on the modulated lines) | 10 | 8 |
| **Total** | **19** | **15** |

34 accesses per sample, measured by the renderer. At 74.25 MHz there are ~1,546 cycles per sample. Even at 4 cycles per async access, that's 136 cycles, ≈ 9% of the sample period (spec target: ≥ 20% slack). **Measured on hardware (v0.2.3, D47):** with the default 10-cycle reads / 8-cycle writes, the whole engine (voices + FX + SRAM) takes at most 684 cycles per sample (HUSH), 44% of the period, so 56% slack.

## BRAM (M10K, ~385 KB total): estimate

| Item | Bytes |
|---|---:|
| CPU code + data + stack | 65,536 |
| Sine quarter table (4098 × 16) | 8,196 |
| Saw + tri wavetables (2 × 10 × 1024 × 16) | 40,960 |
| Limiter table (1025 × 24) | 3,075 |
| Pluck delay lines (4 × 2048 × 16) | 16,384 |
| Voice parameter registers (32 × 32 × 32-bit, shadow + live) | 8,192 |
| Voice state (32 voices × ~16 words) | 2,048 |
| Font ROM (8×8, 128 glyphs) | 1,024 |
| Video (tile/text map, palettes, sprite list) | ~4,096 |
| FIFOs | ~512 |
| **Total** | **~150,000 (~39%)** |

**Firmware (Phase 4, measured):** RV32IMC at `-Os`, 38.2 KB image (code + read-only tables), 7.9 KB `.bss` (composer state, dominated by the 256-entry scheduler heap), 4 KB stack (1.1 KB used at most in the ISS runs). Everything ends at 0xC220, leaving ~15 KB of the 64 KB CPU RAM free. Without compressed instructions (RV32IM) the code alone is 52 KB, which would not leave enough room. Audio FIFO: 1024 × 32 bits (4 KB).
