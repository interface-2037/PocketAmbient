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
| Voice parameter registers (32 voices × 32 words × 32 bits, one copy: D48, no shadow) | 4,096 |
| Voice state (32 voices × 32 words × 32 bits) + status (32 × 32 bits) | 4,224 |
| Font ROM (8×8, 128 glyphs) | 1,024 |
| Video (tile/text map, palettes, sprite list) | ~4,096 |
| FIFOs | ~512 |
| **Total** | **~148,000 (~38%)** |

**Firmware (measured):** RV32IMC at `-Os`. Phase 4: 38.2 KB image, 7.9 KB `.bss` (composer state, dominated by the 256-entry scheduler heap), 4 KB stack. With the UI (Phase 5, D52): 47.4 KB image, ~9.6 KB `.bss` (incl. the 1.5 KB UI coroutine stack), 4 KB main stack; everything ends at 0xE7D0, leaving ~6 KB of the 64 KB CPU RAM. Deepest stack use measured by `rvsim --ui` (all modes, menus, lock screen): 816 of 4,096 bytes on the main stack (TIDES; 512 to 560 in the other modes), 352 of 1,536 bytes on the UI coroutine stack. **Watch this:** more UI or music code may need the RAM grown to 96 KB or the tables moved to a ROM. Framebuffers (D52): 2 × 5,760 × 32 bits = 46 KB block RAM (~40 M10K). Without compressed instructions (RV32IM) the code alone is 52 KB, which would not leave enough room. Audio FIFO: 1024 × 32 bits (4 KB).
