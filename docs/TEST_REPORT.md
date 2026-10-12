# Test report

## v0.4.1: fixes, timing margin, Phase 6 soak (simulation, 2026-10-12)

No Quartus in the cloud environment (see below), so the fit and timing are measured by the PC build. Everything here is simulation. Hardware checklist: `docs/HW_TEST_NEXT.md`.

**A. Title-screen readouts / stopped mode change (D53)**

| Check | Result |
|---|---|
| Instrumented host render (ASan + UBSan, voice index checked in `hal_vwrite`): stop, then 1–2 mode changes in the fade-out, with and without A again in the fade; every mode, 4 seeds, at 20 / 45 / 90 s | **before: 246 of 260 runs fail** (out-of-range role voice indices, e.g. HUSH → TIDES writes voice 301; up to 1,966,097); **after: 0 of 288** |
| Music unchanged: v0.4.0 vs v0.4.1 host render, 4 modes × 2 seeds × {autostart only; switches, stop/start, reseed, menu changes}, 120 s each | **16 / 16 byte-identical** |
| Firmware on the ISS == host render (`tools/check_fw.sh 60`), and 70 s with 13 presses including stop + switch + restart in the fade | **bit-identical**, all modes |
| Title screens after a stopped mode change (ISS frames, `docs/ui/title_v0.4.1.png`) | HUSH `---` BPM, `BAR --`, no chords; DRIFT / TIDES / MOTHER readouts `--` |
| Unit tests (`make test`) incl. new `test_stop_switch` (4 modes × 2 directions × with / without restart) | 495,008 checks, 0 failed |

**B. Timing changes (D55): bit-exactness and cycles**

| Check | Result |
|---|---|
| Voice vectors, Icarus, 0.5 s per mode | **pass** (bit-exact) |
| Audio (voice + FX) vectors, Icarus, 0.25 s per mode | **pass**, max 457–537 cycles/sample |
| Audio vectors, Verilator, 6 s per mode (288,000 samples each), v0.4.0 vs v0.4.1 FX engine | both bit-exact; worst cycles/sample DRIFT 447 → 497, TIDES 437 → 487, HUSH 527 → 577, MOTHER 507 → 557 (+50 everywhere) |
| FX fuzz (`TB=audio sim/run_fuzz.sh`): Verilator 4 × 250,000 + Icarus 4 × 15,000 | **1,060,000 samples bit-exact**; random-stimulus worst 767 cycles/sample (50 % of 1,546) |
| `tb_settings`, `tb_fb_video` | **pass** |
| `tb_ap_top` (Icarus, pin-level 55 ns SRAM, CPU + firmware, menu read-back) | **pass**: 480 samples bit-exact, CPU max 230,967 cycles/tick, frame gap 1546..1547 |
| Full system, Verilator (`sim/run_sys.sh`): CI script (2 s, menu mode / reseed / density) | **bit-identical**, all modes |
| Full system, 20 s per mode, presses `6:A,6.5:RIGHT,7:A,13:A,13.4:LEFT,17:A` (stop + switch + restart in the fade, twice) | **bit-identical**, all modes; CPU max 230,912–230,967 cycles/tick; 0 underruns |
| Full system, 60 s per mode, seed 456 (2.88 M samples and 2.1–2.4 G cycles per mode) | **bit-identical**, all modes; CPU max 231,199–231,254 cycles/tick; 0 underruns; no trap or bus error |

Expected on hardware: engine worst case ~734 of 1,546 cycles (v0.4.0: 684). Timing: every one of the 200 worst v0.4.0 paths from `fx_engine|Mult0` (+0.209 … +1.782 ns) and `cpu_sys|tick` (+1.063 / +1.359 ns) is cut by a register, so the worst path should move to the voice engine's `ve_mul|p -> Add45` (+1.398 ns in v0.4.0, unchanged logic), about **+1.4 ns**. Fitter seed noise can shift that by a few hundred ps.

**C. Phase 6 soak: randomized stimulus on the re-timed voice engine**

| Check | Result |
|---|---|
| `SIM=verilator TAG=soakA SEED0=1000 sim/run_fuzz.sh 12 850000` (`model/fuzz.c`, 12 new seeds) | **12 / 12 streams pass, 10,200,000 samples bit-exact**, 185,661 random register writes (spec: ≥ 10 M) |
| Same bench under Icarus (4-state), 4 × 40,000, seeds 2000+ | **160,000 samples bit-exact** |
| Total randomized voice-engine samples to date | ~12.2 M (1.8 M earlier + 10.36 M now), plus 1.06 M through voice + FX |
| Mutation check (`SIM=verilator sim/mutate.sh 200000`): 14 one-token RTL bugs in `voice_engine.sv` | **14 / 14 caught** |

The Verilator option (`SIM=verilator`) runs the same testbench ~30× faster than Icarus. It is 2-state (X becomes 0), so Icarus stays the default and is still run on part of each batch.

**E. Quartus in the cloud:** not possible in this environment. The egress proxy refuses `downloads.intel.com`, `www.intel.com` and `cdrdv2.intel.com` (HTTP 403, organization policy). The machine itself would be enough (4 cores, 15 GB RAM, ~28 GB free disk for Lite + Cyclone V). Allowing those hosts in the environment's network policy would make an install worth trying. Nothing was added to CI.

## Phases 4–5: v0.4.0 on hardware (2026-10-11)

Build: Quartus Lite 25.1std on Windows (`build/fpga_v040.log`, 15 min). **Fit:** 11,740 ALMs (64 %), 15,581 registers, 301 / 308 M10K (98 %), 26 DSP, 224 / 224 pins. **Timing met in every corner:** worst setup slack +0.209 ns (`clk_74a`, slow 1100 mV 0 °C; +0.848 ns at 85 °C), worst hold +0.062 ns. Tested as the **dev** package (`AmbientPocket-0.4.0-dev.zip`: firmware loaded from `Assets/ambient/common/composer.bin`, 47,452 bytes).

Result from the user: **everything looks and sounds great**, apart from two HUSH notes below (preferences, not failures).

| Check | Result |
|---|---|
| Firmware loaded from the SD data slot (D50): slot location and bridge byte order | **pass**: core boots and plays, no red (CPU fault) screen |
| All four modes play, with their screens (D52) | **pass** (HUSH and MOTHER screenshots taken) |
| Info overlay (Y) after 7:59 of play, MOTHER | CPU MAX **231,138** cycles (sim estimate 231k), FIFO MIN **513** of 1024 (never below the 512-frame prime), UNDERRUN **0**, FRAME **1546–1547** |

- **Phase 3 tick question settled:** on hardware the LRCK-derived frame tick is spaced 1546–1547 `clk_74a` cycles, exactly as simulated, and the FIFO never underran. The saturated overrun counter in v0.2.3 was not caused by irregular ticks; that build's overrun logic is gone with `engine_test.sv`.
- **Feedback for later (HUSH):**
  1. **BPM readout overflows when stopped** (fixed in v0.4.1, D53): it showed "11566", overlapping the "BPM" label. Cause: changing mode while stopped (`do_switch`, `fw/composer/composer.c`) only changes `c->mode`; the per-mode state is a union (`c->m`), so the HUSH screen reads the previous mode's state as `hush_t` until HUSH starts. Display only. Fix options: blank the mode-specific readouts on the title screen in `fw/ui/ui.c`, or clear `c->m` on a stopped mode switch (composer change: check `rvsim` vs `render` stays bit-identical).
  2. **Audible crackle:** the user believes it is the intended vinyl crackle (D27, thinned in D34 to ~4 pops/s), and 0 underruns supports that. Revisit its level in a music pass.

## Phase 5: menu and screens, simulation (2026-10-12)

| Check | Result |
|---|---|
| Firmware with the UI on the ISS (`rvsim --ui`): audio vs host render | **bit-identical** (the UI only reads composer state) |
| ISS screens (`--frames`), all modes | match the D42 mockups (`build/frames`, see `docs/ui/`) |
| `tb_fb_video`: 5,760 random words, 400 pixel writes, read-back, palette, swap at vsync, two full scanned frames vs a model, unrelated clocks | **pass** |
| Full system in Verilator with the UI drawing (DRIFT, TIDES 3 s): audio vs render; scanned-out frames | **bit-identical**; frames match the ISS frames |
| UI timing (ISS estimate): a full frame ~0.9 M cycles spread over ~12 ms, ~30 frames/s; composer's worst tick 231k cycles in RTL (incl. finishing the drawing primitive in progress) | FIFO lead needed ~64 frames of 512 |

## Phase 4: CPU integration, simulation (2026-10-12)

Firmware: `make fw` (clang 18 / lld, RV32IMC `-Os`), 38.2 KB image. Hardware build v0.3.0 (`sys_test.sv`) not yet tested on the Pocket.

| Check | Result |
|---|---|
| Firmware on the ISS vs host render (`model/rvsim.c`), 60 s per mode, seed 123 | **bit-identical**, all four modes |
| Same, 10 min per mode, seed 7, 19 scripted presses (mode switches, stop / start, reseed, every button) | **bit-identical**, all four modes |
| 64-bit division / memcpy / memset helpers (`fw/rv/rt.c`) vs native, 20 M random + edge cases | 0 failures |
| Full system in Verilator (`sim/run_sys.sh`): PicoRV32 + firmware + voice/FX engine, SRAM model with sram_ctrl latency, real 48 kHz pacing, DRIFT 1 s | **bit-identical**; worst tick 180,601 CPU cycles (boot); FIFO never below the 512-frame prime level; 0 underruns |
| Full system, fast pacing, 60 s per mode | **bit-identical**, all four modes (2.88 M samples each, ~2 G cycles, ~27 min per mode at 1.24 MHz). Worst tick measured: DRIFT 180,601, TIDES 180,559, HUSH **205,586**, MOTHER 180,578 CPU cycles; no CPU trap or bus error |
| `sys_test` (v0.3.0 top) in Icarus through the pin-level 55 ns SRAM model: boot, SRAM clear, A press via `cont1_key`, 5 ms | **pass**: samples bit-identical, screen shows DRIFT / PLAY, frame gap 1546–1547 |
| Unit tests (`make test`) after the firmware changes | 494,960 checks, 0 failed |
| Firmware from the SD data slot (D50): `sys_test` with an empty CPU RAM, firmware streamed over the bridge before Reset Exit | **pass**, bit-identical |
| Core menu (D51): `tb_settings` unit test (pre-boot persisted writes, live reads, APF echo ignored, button/menu race, clamps, lock-seed latch, reseed action) | **pass** |
| Menu changes during play: `--set` (mode ×2, reseed, density, reverb, brightness) + a button press, full-system Verilator, DRIFT and HUSH 3 s; ISS 4 s all modes | **bit-identical** |
| `sys_test` with persisted menu values written before boot (Lock seed, Seed 123); menu reads the live seed back | **pass** |

**CPU load** (ISS estimate; RTL measured where noted): average ~2.5–4k cycles per 1 ms tick (74,250 available), so the CPU is idle > 90 % of the time. Heavy ticks: boot (`composer_init` + tick 0) ~180k cycles and a HUSH tick at 205,586 cycles, both measured in RTL (the ISS's cycle estimate runs ~25 % low). These run ahead into the 1024-frame audio FIFO; the worst lead needed was ~90 frames, against 512 frames of prime.

**Still to do for Phase 4:** the v0.3.0 hardware test (fit, timing at 74.25 MHz with the CPU added, and listening), and loading the firmware from an SD data slot for dev builds (D44).

## Phase 3: engine test on hardware (2026-10-12)

Build v0.2.3 (`engine_test.sv`): SRAM BIST, then recorded composer excerpts (seed 123, `tools/gen_vecrom.py`) replayed through the real voice engine, FX engine and SRAM. Tested by the user; screenshots taken a few seconds into each excerpt. **All four excerpts sound good.**

| | DRIFT | TIDES | MOTHER | HUSH |
|---|---|---|---|---|
| Play time at screenshot | 00:13 | 00:08 | 00:09 | 00:04 |
| Max engine cycles / sample | 0x266 = 614 | 0x284 = 644 | 0x298 = 664 | 0x2AC = 684 |
| Tick overruns | **FF** | **FF** | **FF** | **FF** |

- **SRAM BIST:** all four wait-state pairs (6,5) (5,4) (4,3) (3,2) pass with 0 errors over all 131,072 words. The 55 ns simulation model fails the last two pairs on purpose, so the real part is faster than assumed.
- **Cycle budget:** the worst case is 684 of 1,546.875 cycles per sample (44%), well inside the ≥ 20% slack target.
- **Open: tick overruns.** The counter (8-bit, saturating) reads FF in every excerpt. An overrun is a 48 kHz frame tick that arrives while the engine is still busy with the previous sample. With the measured ≤ 684 busy cycles and write bursts of ≤ 302 writes, evenly spaced ticks can't produce one. Re-run 2026-10-12 on Linux (Icarus 12): `tb_player` (6,000 samples per excerpt) and `tb_engine_test` (3,000 DRIFT samples through the pin-level SRAM model) are bit-exact with **0 overruns**. The difference is the tick. The testbenches make it directly in `clk_74a`; on hardware it comes from the LRCK edge in `core_top.v`. Suspect: extra or irregular ticks from the I2S clock chain (MCLK accumulator → ripple-clocked SCLK/LRCK → synchroniser). This is not audible in the test, but in Phase 4 the composer is paced by the same tick, so it must be explained first. Next hardware build: show the minimum and maximum tick spacing (in `clk_74a` cycles) and the tick count next to the sample count.
  - **Follow-up (2026-10-12):** the exact `core_top.v` clock chain (MCLK accumulator → SCLK divider → LRCK → 3-flop synchroniser → edge detect) simulated for 20 ms: 958 gaps, all 1546 (120) or 1547 (838) cycles, mean 1546.87. So the tick logic is correct as written. The derived audio clocks have no SDC constraints (template practice), so a physical cause can't be ruled out from simulation. v0.3.0 shows FRAME MIN / MAX on screen to settle it, and its audio no longer depends on the tick being regular (the FIFO absorbs it; an irregular tick would show as UNDERRUN instead).
- The title line on the test screen read "ENGINE TEST V0.2.1": `screen_test.hex` hadn't been regenerated after the version bump. Regenerated now.


## Phase 1: host model + composer (2026-10-10)

Build: WinLibs GCC 16.2, `-O2`. Raw results: `reports/2026-10-09-full/` (JSON, plots, traces).

### Unit / system tests (`make test`)

494,957 checks, **0 failed**: PRNG KAT and streams, scheduler (ordering, ties, capacity, wrap at 2³² and 2⁶⁴), harmony (scale membership, key neighbours, voice-leading bounds), TIDES period relations, fixed-point helpers and limiter, SVF stability over the full f/q grid, envelope timing (±5%, 30 s attack, 60 s release), retrigger continuity, allocator (no stealing gated or audible voices, freeing, max-length safety), determinism, fades/stop to exact zero in all modes, switching stress, TIDES notes in scale, tick overflow at 2³¹ and 2³². `--long` adds the sleep timer.

### Section 8.3 render matrix (`tools/run_validation.sh full`)

**38 / 38 pass**: 8 h per mode at defaults, 1 h at the 4 density/brightness corners, 5 seeds per mode (1 h), counter-overflow renders at 2³¹ and 2³² (all modes), a 30 min switching stress (mode/reseed every 5 s), and determinism.

| Run (8 h, defaults) | LUFS | Peak dBFS | Mono Δ LU | >16 kHz dB | <20 Hz dB | DC dBFS | Clicks | Repeated 30 s | Keys visited |
|---|---|---|---|---|---|---|---|---|---|
| DRIFT | -19.7 | -6.2 | -1.0 | -81.4 | -59.4 | -85.2 | 0 | 0 | 14 |
| TIDES | -19.8 | -2.4 | -0.3 | -77.4 | -64.3 | -90.3 | 0 | 0 | 23 |
| HUSH  | -20.2 | -4.4 | -0.3 | -53.5 | -51.0 | -86.8 | 0 | 0 | 11 |

Across all 38 runs:
- Integrated loudness -17.8 … -22.5 LUFS (the density-1 corners are the extremes).
- Mode spread at defaults is 0.4 LU.
- No full-scale samples, no dead air, no stuck notes, and no steal above -90 dB.

**Thin margins to watch:** HUSH HF energy (-52.5 dB vs the -50 dB limit) and LF energy (-49.7 dB vs -40, fine). TIDES at brightness 5 / density 1 reaches -56 dB HF.

### Listening review (spec 8.5)

Round 1 (2026-10-10): DRIFT and TIDES good. HUSH (nature textures) rejected as "white noise, static" → redesigned as a lo-fi beat (D27). The user then redirected TIDES to Blade Runner / CS-80 (D29) and DRIFT to cold orchestral Goldsmith (D30).

Round 2 (2026-10-10): TIDES perfect. DRIFT had a "harsh noisy constant background" and HUSH "static" in the crackle → removed DRIFT's room-air bed and HUSH's rain bed, and thinned the crackle (D34). A 4th mode, **MOTHER** (Nostromo computer room, no music), was added (D35, D36).

**Round 3 (2026-10-10): signed off as "good enough" by the user for all four modes.** Further musical tuning is deferred to after the hardware build (DECISIONS D39). Validation for this round:
- `reports/2026-10-10-full/`: DRIFT, TIDES and HUSH all pass, with defaults at -19.5 / -20.7 / -19.9 LUFS.
- `reports/2026-10-10-full-mother/`: **13 / 13**, default -21.5 LUFS. This is MOTHER retuned to the reference clip (D38): within ±3.7 dB of `reference/MOTHER.wav` from 0 Hz to 10 kHz.

Unit tests: 494,960 checks, 0 failed.

Excerpts in `renders/listening/`: `<mode>_0h.wav`, `<mode>_2h.wav`, `<mode>_6h.wav` (10 min each, seed 123) and `montage.wav` (DRIFT → TIDES → HUSH → MOTHER → stop).

### Round 3

| Question | DRIFT | TIDES | HUSH | MOTHER |
|---|---|---|---|---|
| Calm? Anything harsh, too loud, too bright? |bass is too heavy and it sounds too synth like, it should be more orchestral| perfect, dont change a thing| perfect, except just remove the vinyl pops | it all sounds like its being played at 2x+ speed.  Tones should be softer and I expected more sounds like 70's hard drive clicking|
| Evolving rather than static / repetitive? |yes | yes| yes| yes|
| Clearly distinct from the other modes? |yes |yes |yes |yes |
| OK on a phone speaker? |yes |yes |yes |yes |

### Round 2 (answered)

| Question | DRIFT | TIDES | HUSH |
|---|---|---|---|
| Calm? Anything harsh, too loud, too bright? | drift is great except there's a harsh noisy constant background|Perfect! | great, but the vinyl crackle just sounds like noise and theres a static to it|
| Evolving rather than static / repetitive? |perfect |perfect |perfect |
| Clearly distinct from the other modes? |yes |yes |yes |
| OK on a phone speaker? | too much static| yes|too much static |

### Hardware matrix (HW-01 … HW-16)

Phase 0 build `dist/AmbientPocket-0.1.0.zip` (440 Hz test tone + title screen), tested by the user 2026-10-10: **Phase 0 passed** (HW-04 deferred: no Dock yet).

| ID | Test | Result |
|---|---|---|
| HW-01 | Boot & load from SD: core listed, loads, title screen appears | yes |
| HW-02 | 440 Hz tone on the internal speaker: clean, no buzz | yes |
| HW-03 | 440 Hz tone on headphones: clean, no clicks | yes |
| HW-04 | Docked: HDMI audio + video scale | no Dock...yet |
