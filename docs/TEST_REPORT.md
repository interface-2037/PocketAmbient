# Test report

## v0.5.0: Pocket lock (simulation, 2026-10-18)

D62. Not yet built in Quartus or tested on hardware (steps in `HW_TEST_NEXT.md`).

| Test | Result |
|---|---|
| `tb_pocket_lock` (new, Icarus, HOLD 100 / HINT 50 cycles): A acts on its press; Start only on release; Select-then-Start and Start-then-Select send no play / stop; other buttons work with Select down; a chord shorter than HOLD does nothing; holding it locks once (no second toggle while held), hint then black; while locked nothing passes and each press re-shows the hint for HINT; holding the chord again unlocks with no play / stop on release; the menu action locks, a Start held across it is not sent; menu action while locked is a no-op; reset unlocks | **PASS**. Mutation check: 9 single-line mutations (lock gate, Start cancel / pending, re-arm, toggle, hint, menu guard) all make it fail |
| `tb_settings` (+ boot / cold): new Pocket Lock action at `0x5000_0024` pulses `cmd_lock` once and is not a reseed; all earlier checks unchanged | **PASS** |
| `tb_ap_top` (Icarus, pin-level SRAM, CPU + new firmware + engine, `pocket_lock` in the path) | **PASS**: 1440 samples bit-exact, menu seed reads 123, CPU max 200,201 cycles/tick, frame gap 1546..1547 |
| rvsim, DRIFT and HUSH, seed 9, 20 s: `--ui --lock 4` vs no UI | **identical** audio (also equal to host `render`): the lock screen does not touch the composer |
| rvsim `--frames` with `--lock`: lock screen while playing (TIDES) and stopped | `docs/ui/pocket_lock.png` (normal screen, locked while playing, locked while stopped) |
| Firmware size | 49,166 of 65,536 bytes |

## v0.4.4 on hardware (2026-10-12)

Build on the PC (Quartus Lite 25.1std). The default fitter seed missed hold by −0.044 ns (fast 1100 mV 0 °C, `ve_interp:u_isawA` c2 → fr, logic unchanged since v0.4.1). A refit with `--seed=3` and no source change met every corner: worst setup +1.723 ns, worst hold +0.111 ns, 12,115 ALMs (66 %), 301 / 308 M10K. Tested as the dev package.

| Step (v0.4.4 `HW_TEST_NEXT.md`) | Result |
|---|---|
| 1. Power-cycle, version 0.4.4 | **pass** |
| 2. Button changes (Density via Down, Brightness via L/R, Mode via Left/Right) survive a power cycle; menu shows the same values | **pass** |
| 3. A menu change survives a power cycle | **pass**: Reverb was already High, so the user changed it to Low in the menu; Low came back after the power cycle |
| 4. Lock Seed on: same seed after a power cycle | **pass** |
| 5. Files on the card after the last power-off | `ambient.sav`: 32 bytes, magic `AMBS`, format 1, Lock 1, seed `FD5ADEF7`, settings MOTHER / Density 4 / Brightness 1 / Reverb Low, check word valid. `interact_persist.json`: Mode 1 (stale APF menu cache), Density 4, Brightness 1, Lock 1. The `.sav` takes precedence at a cold start (D59), so the stale menu mode does not matter. |

**All core-menu persistence items from D51 are resolved on hardware.** Phases 4–5 are complete.

## v0.4.4: settings from ambient.sav at a cold start (simulation, 2026-10-12)

**A. v0.4.3 on hardware (user, 2026-10-12)**

| Step (v0.4.3 `HW_TEST_NEXT.md`) | Result |
|---|---|
| 1. Power-cycle, version 0.4.3 | **pass** |
| 2.1–2.3 Mode change / change + A / B during the stop fade-out (D56) | **pass**: no boost, smooth crossfade, B in the fade OK |
| 2.4 Y overlay | **pass**: CPU MAX 230,942, FIFO MIN 512, UNDERRUN 0, FRAME 1546–1547 |
| 2.5 Listening check | **pass** |
| 2.6 Core menu Mode / Density | **pass** |
| 3. Settings saved at power-off | **FAIL**. Changes made in the menu persist, but a change made with the buttons does not. Menu Density 5, then Down (Density 4 on screen), power off / on: Density came back as **5** |
| 4. Lock Seed on / off across power cycles | **pass**: seed nonzero, kept when locked, changes when unlocked |
| 5. Files on the SD card | both written at the same power-off, 11:57 PDT (see below) |

Files after the session (both from the last power-off, 11:57):
- `Settings/beingFar.AmbientPocket/Interact/_core/interact_persist.json`: id1 Mode 1, id2 Density 3,
  id3 Brightness 1, id4 Reverb 2, id5 Visuals 0, id6 Sleep 0, id7 Lock 0.
- `Saves/ambient/beingFar.AmbientPocket/ambient.sav`, 32 bytes, valid: `414D4253 01000000 A5B20AD1
  00002241 00000000 00000000 00000000 CBD1BC75`. Word 3: TIDES, density 4, brightness 2, reverb High,
  visuals Full, sleep Off. Word 1 was reported as `00000001`, but the check word `CBD1BC75` matches only
  `01000000` (format 1, lock off, as the menu file's Lock 0 says). `00000001` gives `CDD1BC73`.

The `.sav` holds the live settings, so the core's power-off reads (D57) and the save slot (D58)
work. The menu file stores a list's 0-based option **index** in `val`. Its Density 3 / Brightness 1
are options "4" / "2" (Mode and Reverb options start at 0, so index = value), so at 11:57 the two
files agree. They don't show what the APF held at the step-3 power-off. Either way, a button change
did not come back through the menu file, and the core can't control what the APF restores from it.
Fix: D59.

**B. v0.4.4 checks**

RTL (`settings_bridge.sv`) and test benches only. No firmware, model or engine change.

| Check | Result |
|---|---|
| `tb_settings`, new `tb_settings_cold`: five power-ups, each with fresh registers. **Session 0:** no save file, so the menu file is used (DRIFT / 3 / 3 / Med). Menu Density 5, then Down (4), L (brightness 2) and Right (TIDES) with the buttons, and Reverb High from the menu. Quit: the menu reads density 4, and the save image = TIDES / 4 / 2 / High with a valid check word. **Next power-on**, two orders (menu file then slot; slot then menu file). The APF's stale cache (DRIFT / 5 / 3 / High) and session 0's file are written in reset. Before Reset Exit cfg = the menu file's values. After it, cfg, live and APF-style menu reads of all six settings = the file's (TIDES / 4 / 2 / High / Full / Off), and the composer starts with them. Then a menu Density 2 change applies, and a second Reset Exit without a load does not re-apply. A new load of the older file after the composer has run is not applied either. The Quit image carries density 2. **Rejected files**, each with a valid check word: density 0 / 6, brightness 0 / 7, mode 4, reverb 3, visuals 3, a reserved bit, mode bit 3. Each keeps the menu file's values (HUSH / 5 / 1 / Low / Minimal / sleep 2), and the seed is still restored. No file, a bad check word, a foreign magic and format 2 also keep the menu file's values. A valid file with every field at its limit (MOTHER / 1 / 5 / Med / Off / sleep 3) is applied. A menu change, then an unbooted Reset Exit with no load, keeps the change. **The hardware file** above, with a stale menu file (TIDES / 5 / 3 / High), gives density 4 / brightness 2 and lock off. The existing v0.4.3 checks are unchanged | **PASS** |
| Negative test: the same bench on the v0.4.3 `settings_bridge.sv` | **FAIL, 40 checks**: every cold-start case returns the menu file's values (e.g. "v0.4.3 hardware file: density 4, brightness 2"), and the composer starts with the stale density |
| Mutations of the new RTL: (a) no `!booted` guard; (b) no range check; (c) `cfg_*` set but not `live_*`; (d) no density upper bound; (e) `sav_new` never cleared | (a) **6 failures** (an older file re-applied over a same-session menu change); (b) **63**; (c) **13** (menu reads disagree with the music); (d) **7**; (e) **3** (re-applied without a new load) |
| `tb_ap_top` `RESTORE=1` (Icarus, pin-level SRAM, CPU + firmware + engine), now with a stale menu file (DRIFT, density 5, brightness 3, reverb High, Lock seed on) and a save file (TIDES / 4 / 2 / High, lock on, seed 4242), both written over the bridge in reset. APF-style menu reads right after Reset Exit. First samples vs `render --mode tides --seed 4242 --density 4 --brightness 2 --reverb 2`. Then Down (density 3), reset entry, Quit reads | **PASS**: the menu reads TIDES / 4 / 2 / High after Reset Exit, and 480 samples are bit-exact. The Quit reads return TIDES / 3 / 2 / High / lock / 4242, and the save file `414D4253 01000001 00001092 00002231 0 0 0 6299A5F1`. CPU max 230,849 cycles/tick, frame gap 1546..1547 |
| Same `RESTORE=1` bench on the v0.4.3 `settings_bridge.sv` | **FAIL**: the menu reads DRIFT / 5 / 3 after Reset Exit, the audio differs from sample 467 (10 mismatches in 480), and the Quit reads return DRIFT / 4 / 3 |
| `tb_ap_top` default (Lock seed + Seed 123 from the menu, no save file) | **PASS**: 480 samples bit-exact, menu seed read 123, CPU max 230,675 cycles/tick |
| `verilator --lint-only -Wall settings_bridge.sv` | the same single warning as v0.4.3 (`bridge_addr[1:0]` unused) |
| Firmware on the ISS == host render (`tools/check_fw.sh 60`) | **bit-identical**, all modes |
| Full system, Verilator (`sim/run_sys.sh`), CI script (2 s, menu mode / reseed / density) | **bit-identical**, all modes; CPU max 230,625–230,675 cycles/tick; 0 underruns |
| Full system, Verilator, 21 s per mode, presses `6:A,6.5:RIGHT,9.5:A,13:A,13.4:LEFT,13.8:A,17:A,17.5:B,18:RIGHT` + menu `19:DENSITY=2,19.5:REVERB=0` | **bit-identical**, all modes (1,008,000 samples each); CPU max 230,625–230,675 cycles/tick; 0 underruns |
| Music unchanged vs v0.4.3 | no file under `model/`, `fw/` or `tests/` changed, so the host render and the firmware are the same. The only RTL change acts at Reset Exit, before the composer runs, and only with a valid save file. Without one (every music path above) the settings are bit-for-bit those of v0.4.3 |
| `make fw` | `composer.hex` regenerated byte-identical (48,934 of 65,536 bytes): no firmware change, not recommitted |

Not testable here: whether the APF writes any menu value *after* Reset Exit at power-on. Such a
write would count as a menu change and override the file. Hardware checklist step 2.

## v0.4.3: settings persistence and the saved seed (simulation, 2026-10-12)

Fixes for the v0.4.1 hardware report. The APF's `interact_persist.json` had every list element 0
(Density / Brightness were 5, TIDES was playing), Lock seed 1, and no entry for the `number_u32`
Seed, so the next boot showed `SEED 00000000`. Lists: D57 (reads captured on the `bridge_rd`
strobe and held; live values kept through reset). Seed: D58 (nonvolatile save slot `ambient.sav`).
RTL and packaging only: no firmware, model or engine change. Hardware checklist: `docs/HW_TEST_NEXT.md`.

| Check | Result |
|---|---|
| `tb_settings` (Icarus; now also `slot_size.sv`). All reads APF-style: strobe, the address moves on, the word is taken 3 cycles later. Covers the v0.4.1 case: menu Density 5 / Brightness 5, TIDES and Density 4 with the buttons, Lock seed latch, reset entry with the composer status cleared, then reads of every element. Also: the word held until the next strobe; Quit reads of the save slot (magic, format, lock, seed, settings, check word computed independently); restore at Reset Exit, no re-apply without a new load, damaged file ignored, file with lock off. Fresh-register power-on paths: loaded words given back unchanged before the first run, Lock seed with no seed turned off, menu file + save file applied at boot, then Quit reads after a run. `slot_size`: dev / release data-table layouts, no save slot | **PASS** |
| Negative test: the same bench on the v0.4.2 `settings_bridge.sv` (only an unused `rst` port added) | **FAIL, 44 checks**, among them every reset read: Density 5 instead of 4 (the button change is lost), other elements return the word of the address now on the bus |
| Mutations of the new RTL: (a) read word re-registered every cycle instead of on the strobe; (b) the old `st_valid ? st : cfg` fallback for Density | (a) **43 failures** (all reset / Quit reads); (b) **caught** ("reset read: density = live 4 (buttons)") |
| `tb_ap_top` (Icarus, pin-level SRAM, CPU + firmware + engine), as before: Lock seed + Seed 123 from the menu | **PASS**: 480 samples bit-exact, menu seed read 123 (APF-style read), CPU max 230,675 cycles/tick, frame gap 1546..1547 |
| `tb_ap_top` `RESTORE=1`, the boot-restore path at system level: menu file (TIDES, density 5, brightness 2, reverb High, Lock seed on, no Seed) and a save file (lock on, seed 4242) written over the bridge before Reset Exit; first samples vs `render --mode tides --seed 4242 --density 5 --brightness 2 --reverb 2`; then Down (density 4), reset entry, APF-style Quit reads of the menu and the save slot | **PASS**: 480 samples bit-exact (seed and settings restored), composer status density 4 / seed 4242. Quit reads return TIDES / 4 / 2 / High / Lock on / 4242, and the save file `414D4253 01000001 00001092 00002241 0 0 0 6299A571`. CPU max 230,844 cycles/tick |
| Same `RESTORE=1` bench on the v0.4.2 `settings_bridge.sv` | **FAIL**: seed not restored (17 mismatches in 480 samples, status seed 0); every Quit read returns 1, the Mode word at the address now on the bus |
| `core_top` elaborated with Verilator (all core RTL + `core_bridge_cmd` + `mf_datatable`, vendor IP stubbed); `verilator --lint-only -Wall` on `settings_bridge.sv` / `slot_size.sv` | no errors; one lint warning, the same as v0.4.2 (`bridge_addr[1:0]` unused) |
| Firmware on the ISS == host render (`tools/check_fw.sh 60`) | **bit-identical**, all modes |
| Full system, Verilator (`sim/run_sys.sh`), CI script (2 s, menu mode / reseed / density) | **bit-identical**, all modes; CPU max 230,625–230,675 cycles/tick; 0 underruns |
| Full system, Verilator, 21 s per mode, presses `6:A,6.5:RIGHT,9.5:A,13:A,13.4:LEFT,13.8:A,17:A,17.5:B,18:RIGHT` + menu `19:DENSITY=2,19.5:REVERB=0` | **bit-identical**, all modes (1,008,000 samples each); CPU max 230,625–230,675 cycles/tick; 0 underruns |
| Music unchanged: v0.4.2 vs v0.4.3 host render, 4 modes × seeds 7, 31337 × {autostart only; 12 presses + 5 menu changes incl. stop / switch / restart in the fade, reseed, density, brightness, reverb, X / Y}, 120 s each | **16 / 16 byte-identical** (no file under `model/`, `fw/` or `tests/` changed) |
| `make fw` | `composer.hex` regenerated byte-identical (48,934 of 65,536 bytes): no firmware change, not recommitted |

Not testable here: which address the APF puts on the bus after each Quit read (it decides the exact
wrong values v0.4.1 saved), the data-table layout for a slot whose file doesn't exist yet, and the
save file's path. Hardware checklist steps 3–5.

## v0.4.2: mode change / reseed during the stop fade-out (simulation, 2026-10-12)

Fix for the v0.4.1 hardware report (HW_TEST_NEXT step 5): Right during the 3 s stop fade gave the old
tail a short volume boost. Cause confirmed: `do_switch()` snapped the FX set (trim, dry, reverb, echo)
to the new mode with `fx_retarget(c, 0)` while the old tail was still audible. Fix and audit: D56.
No RTL change. Hardware checklist: `docs/HW_TEST_NEXT.md`.

**A. Level during the fade (host render, 20 ms RMS windows above −70 dBFS)**

Stop at 20 s, then a menu mode change at +0.25 / 0.75 / 1.5 / 2.25 / 2.75 s into the fade. All 12 mode
pairs × seeds 5, 77, 4242 (180 runs), each compared with the same fade without the change.

| Metric | v0.4.1 | v0.4.2 |
|---|---|---|
| Max tail level above the same fade without the change | **+8.98 dB** (TIDES → DRIFT) | **+0.00 dB** |
| Max 20 ms rise beyond the reference fade's own rise, worst pair | +6.78 dB (TIDES → DRIFT) | +1.04 dB (TIDES → any)¹ |
| Per pair, v0.4.1 (dB): DRIFT → TIDES / HUSH / MOTHER | +0.87 / −0.20 / −0.14 | −0.02 for all three |
| TIDES → DRIFT / HUSH / MOTHER | +6.78 / +4.64 / +1.63 | +1.04 for all three¹ |
| HUSH → DRIFT / TIDES / MOTHER | +3.10 / −0.74 / −1.35 | −0.28 for all three |
| MOTHER → DRIFT / TIDES / HUSH | +3.11 / −0.35 / +1.52 | −0.18 for all three |

¹ Now the same for every target mode, so it does not come from the FX. It is an echo repeat that
stands out of the released (quieter) tail. It stays below the level of the fade without the change.

Other inputs during the fade (same runs: 4 modes × 3 seeds × 5 points, vs the fade without the input):

| Input during the fade | Level above the reference, v0.4.1 → v0.4.2 | Verdict |
|---|---|---|
| Reseed (B, or menu Reseed) | **+1.70 dB → +0.00 dB** (20 ms rise +4.78 dB before) | fixed: the fading piece is ended (D56) |
| Density + / − (Up / Down) | +2.16 / +0.01 dB, unchanged | next notes of the fading piece, as during play |
| Brightness + / − (R / L) | +0.42 / +0.52 dB, unchanged | as during play |
| Menu reverb 1 → 2 / 1 → 0 | +4.14 / +0.12 dB, unchanged; 20 ms rise beyond the reference ≤ +0.35 dB | 2 s ramp, as during play (no step) |
| X / Y | 0.00 dB, byte-identical | no audio |

**B. Checks**

| Check | Result |
|---|---|
| Unit tests (`make test`) incl. new `test_fade_switch`: from every mode, 1–3 Right presses (to every other mode) or B, at +0.1 / 1.5 / 2.9 s into the fade. No FX register write and no FX-set change from the press to the end of the fade, title FX = the new mode's, tail never above the fade without the press (+1 dB tolerance), and change + A in the fade ramps the FX from the old set over 6 s | **495,160 checks, 0 failed**. The same test on the v0.4.1 composer: 49 failures |
| Music unchanged: v0.4.1 vs v0.4.2 host render, 4 modes × seeds 7, 31337 × {autostart only; 24 presses and menu changes: switches while playing and on the title screen, stop / start, A again in a fade, reseed (B and menu), density, brightness, reverb, X / Y}, 120 s each | **16 / 16 byte-identical** (audio and event trace). A first script that had a menu mode change in a fade by mistake differed from 114.0 s, exactly at that change |
| Firmware on the ISS == host render (`tools/check_fw.sh 60`) | **bit-identical**, all modes |
| ISS, 50 s: `20:A,21:RIGHT,25:A,40:A,41.5:LEFT,41.6:LEFT,44:B` (stop + switch in the fade, restart, stop + two switches + reseed in the fade) | **bit-identical**, all modes |
| ISS, 60 s: `20:A,21:RIGHT,21.5:A,35:A,36:B,37:A` + menu `45:MODE=2` (switch + A in the fade, reseed + A in the fade) | **bit-identical**, all modes |
| ISS, 30 s: `19:A` + menu `20:MODE=3,22:MODE=1` (menu mode changes in the fade) | **bit-identical**, all modes |
| Full system, Verilator (`sim/run_sys.sh` model and flow), 21 s per mode, presses `6:A,6.5:RIGHT,9.5:A,13:A,13.4:LEFT,13.8:A,17:A,17.5:B,18:RIGHT` (stop + switch in the fade, then start from the title; stop + switch + A in the fade; stop + reseed + switch in the fade) | **bit-identical**, all modes (1,008,000 samples each); CPU max 230,625–230,675 cycles/tick; FIFO min 511–514; 0 underruns |
| `make fw` | `composer.hex` regenerated: 48,934 of 65,536 bytes |

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
