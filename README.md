# Ambient Pocket

A generative ambient music core for the Analogue Pocket (openFPGA). It plays procedurally generated ambient music indefinitely, in one of four styles:

| Mode | Character |
|---|---|
| **DRIFT** | Cold, sparse, orchestral: dark strings, a lonely distant horn, an echoing flute, a low drone, icy clusters and rare stingers, built on silence and dissonance (Alien-era Goldsmith) |
| **TIDES** | Blade Runner / CS-80: swelling detuned brass pads with filter sweeps, vibrato and glide, a sparse melody with pitch "sighs", bells, distant thunder, huge reverb |
| **HUSH** | A lo-fi beat: swung boom-bap drums, jazzy electric-piano chords with tape wow, sine bass, sparse plucked lead |
| **MOTHER** | No music: the Nostromo's computer room. Sine chirps, a low rumbling hum, vent hiss and clunks, and a very rare motion-tracker sequence, driven by a wandering system-activity level |

## Installing

1. Download the latest `interface2037.AmbientPocket_<version>_<date>.zip` from the
   [Releases](https://github.com/interface-2037/PocketAmbient/releases) page (or install it with
   an updater that reads the [openFPGA Library](https://openfpga-library.github.io/analogue-pocket/)).
2. Unzip it onto the root of the Pocket's SD card. It adds `Cores/interface2037.AmbientPocket/` and
   `Platforms/ambient.json` (+ its banner). No ROM or BIOS is needed: the music is generated on
   the Pocket.
3. On the Pocket: **openFPGA → Ambient Pocket**, then press **A** to start. Needs Pocket firmware
   1.1 or newer (`version_required` in `core.json`).

Settings changed with the buttons or the menu are remembered (`Saves/ambient/interface2037.AmbientPocket/ambient.sav`).
The core supports **sleep / wake** (the piece resumes where it was) and **Memories**, and has a
**Pocket lock** for listening with the Pocket in a pocket (hold Select + Start for 2 s, see
Controls).

## Development

`AMBIENT_POCKET_CORE_SPEC.md` is the original specification (written as a hand-off brief).
Where it differs from the code (four modes instead of three, clang instead of GCC for the
firmware), `docs/DECISIONS.md` is the current record. Items marked **[VERIFY]** in either
document are facts still to be checked on hardware.

## Status

| Phase | State |
|---|---|
| 0: template + hello tone on hardware | done; v0.1.0 tested on hardware (2026-10-10) |
| 1: C model + composer + renderer | done; signed off after listening round 3 (2026-10-10) |
| 2: voice engine RTL | done; bit-exact vs the C model (Icarus) |
| 3: FX + SRAM | done; v0.2.3 engine test on hardware (2026-10-12); tick spacing confirmed on hardware with v0.4.0 |
| 4: CPU integration | done; v0.4.0 tested on hardware (2026-10-11), firmware from the SD card works |
| 5: controls, menu, video | done; v0.4.0 tested on hardware (2026-10-11); fixes in v0.4.1–v0.4.4 (D53–D59) |
| 6: timing closure, soak | done; timing margin (D55), 10.2 M-sample voice-engine soak and mutation check (docs/TEST_REPORT.md); v0.4.4 passed on hardware (2026-10-12), settings persist |
| v0.5.0 | sleep / wake through a resume save state (D63; passed on hardware as a sleep-only build), menu changes no longer undo button changes (D61, D62), MOTHER rework (D60). Combined build: hardware test pending |
| v0.6.0 | v0.5.0 + Pocket lock (D64: hold Select + Start 2 s; passed on hardware as a lock-only build v0.5.1 / v0.5.2, together with D62). Combined build **passed on hardware** (2026-10-18) |

## Layout

```
fw/hal/regs.h          register map shared by composer, model and RTL
fw/hal/hal_model.c     host HAL (drives the C model)   fw/hal/hal_mmio.c  RISC-V HAL
fw/composer/           portable composer: modes, scheduler, PRNG, harmony, voices
fw/rv/                 RISC-V startup, linker script, runtime helpers, firmware main loop, UI coroutine
fw/ui/                 screens: framebuffer drawing (fb.c), per-mode screens (ui.c), font
model/synth_model.c    bit-exact fixed-point synth engine + FX + master
model/render.c         offline renderer -> WAV
model/rvsim.c          RV32IMC instruction-set simulator: runs the real firmware against the model
src/fpga/core/         RTL: ap_top.sv, synth/ (engine), cpu/ (PicoRV32 system + firmware image), ui/ (video), test/ (Phase 3)
sim/                   RTL testbenches (Icarus) and the Verilator full-system sim (run_sys.sh)
tests/                 unit + system tests
tools/                 table generator, audio analyzer, validation runner, ui/ (screen mockups, platform art + icon)
docs/                  decisions, fixed-point formats, memory budget, register map, test report
```

## Requirements

| For | You need |
|---|---|
| Host model, renderer, unit tests | GCC and make (Linux, WSL2, or WinLibs MinGW-w64 on Windows) |
| Analysis and tools | Python 3 with `pip install -r requirements.txt` (numpy, scipy, soundfile, pyloudnorm, matplotlib, Pillow) |
| Composer firmware | clang, ld.lld and llvm-objcopy (any recent LLVM, tested with 23.1; no RISC-V GCC needed) |
| RTL simulation | Icarus Verilog 12 or newer; Verilator 5 or newer for `sim/run_sys.sh` and `SIM=verilator` |
| FPGA bitstream | Intel Quartus Prime **Lite** with Cyclone V support (tested with 25.1std) |

The firmware image is committed as `src/fpga/core/cpu/composer.hex`, so a Quartus build does not
need LLVM.

## Building and running (host)

On Linux or WSL2:

```bash
make
```

```bash
make test
```

```bash
./build/render --mode drift --seed 123 --density 3 --brightness 3 --minutes 10 --out drift.wav --events drift.csv
```

```bash
python tools/analyze_audio.py drift.wav --plots
```

```bash
tools/run_validation.sh quick
```

On Windows (Git Bash) with WinLibs GCC from winget, run `source tools/env.sh` first, then use
`mingw32-make` in place of `make` and `./build/render.exe`. Plain `make` (the default target)
builds the renderer, the tests, the fuzz model and the instruction-set simulator; `make test`
builds and runs only the tests.

`tools/run_validation.sh full` runs the long matrix (8 h per mode, 1 h per density/brightness
corner, 5 seeds, overflow and switching stress). It takes hours and several GB of disk for the
temporary WAVs, so run it locally; CI runs the `quick` matrix.

## Firmware and RTL simulation

Build the host tools first (`make`), then:

```bash
make fw                     # build/fw/composer.elf, .bin, .map -> src/fpga/core/cpu/composer.hex
tools/check_fw.sh 60        # firmware on the ISS == host render, all modes
build/rvsim --mode tides --seconds 6 --frames 2,5 --frame-prefix build/tides   # screens as PPM
sim/run_sys.sh 60           # Verilator: CPU + firmware + engine == host render (slow: ~1 MHz)
sim/run_fuzz.sh 12 850000   # random-stimulus soak of the voice engine vs the C model (TB=audio: + FX)
SIM=verilator sim/run_fuzz.sh ...   # same testbench under Verilator, ~30x faster than Icarus
sim/mutate.sh               # proves the fuzz bench catches deliberate RTL bugs
```

## FPGA build and installing on the Pocket

```bash
make fpga
```

This runs `quartus_sh --flow compile ap_core` in `src/fpga` (about 15 minutes). The Makefile
looks for Quartus in `/c/altera_lite/25.1std/quartus/bin64`; pass `QUARTUS_BIN=<path to bin64>`
for another install. Check `src/fpga/output_files/ap_core.sta.summary` for negative slack.

```bash
python tools/package.py
```

This makes `dist/sdcard/` and `dist/AmbientPocket-<version>.zip` (firmware inside the
bitstream). To install, copy the contents of `dist/sdcard/` (or of the zip) to the root of the
Pocket's SD card, so you get `Cores/interface2037.AmbientPocket/`, `Platforms/ambient.json` and
`Platforms/_images/ambient.bin`. Power the Pocket fully off and on afterwards: it caches core
metadata across sleep.

`python tools/package.py --dev` makes a dev zip that also loads
`Assets/ambient/common/composer.bin` from the SD card: after `make fw`, copying the new
`composer.bin` there is enough to try new music, with no Quartus rebuild (DECISIONS D50).

### Renderer options

`--mode drift|tides|hush|mother`, `--seed N`, `--density 1-5`, `--brightness 1-5`, `--reverb 0-2`, `--sleep 0-3`,
`--hours/--minutes/--seconds N`, `--out FILE` (`.wav`, RF64 above 4 GB; any other extension = raw s16le),
`--events FILE.csv` (composer trace), `--press 30:RIGHT,90:B`, `--script FILE`, `--tick-start N`,
`--no-autostart`, `--fade-end`.

## Controls (on the Pocket)

| Input | Action |
|---|---|
| D-pad Left / Right | Previous / next mode (6 s crossfade while playing) |
| A or Start | Start (4 s fade-in) / Stop (3 s fade-out to digital silence). Start acts when it is released |
| B | Reseed: a new piece, crossfaded |
| D-pad Up / Down | Density 1–5 |
| L / R | Brightness 1–5 |
| X | Visuals: Full → Minimal → Off |
| Y | Info overlay |
| Select + Start, held 2 s | Pocket lock on / off (D64) |

Menu settings (`interact.json`, D51): mode, density, brightness, reverb Low/Med/High, visuals, sleep timer Off/30/60/90 min, Lock seed (keeps the piece playing now for the next boot) + the seed value, a Reseed action and a Pocket Lock action. The menu shows live values, so button changes appear there too, and settings persist.

**Pocket lock (D64).** For listening with the Pocket in a pocket or bag: hold **Select + Start** for 2 seconds (or choose **Pocket Lock** in the core menu). The music keeps playing and every button is ignored. A lock screen shows for 3 seconds after locking and after any press, then the player screen returns; hold Select + Start for 2 seconds again to unlock. The lock does not survive a power cycle. A core cannot turn the Pocket's backlight off (the openFPGA API has no command for it), so the screen stays on; lower the brightness in the Pocket's own settings for longer battery life. The Home button (Pocket menu) and the power button are handled by the Pocket itself and are not locked.

## Known limitations

- Y shows the engine diagnostics (CPU, FIFO, underruns, frame-tick gaps); a red screen means the
  composer CPU stopped.
- After a setting was changed with the buttons, choosing in the core menu the value the menu
  last set for it does nothing (e.g. back to the starting mode); pick another value or use the
  buttons (D61, D62).
- The MOTHER tone was tuned against a private reference recording (`reference/MOTHER.wav`, used
  by `tools/compare_reference.py`). It is not part of this repository.
- The host renderer's speed depends on the compiler and platform: it was measured at ~90–140×
  realtime (D14) but ran much slower with WinLibs GCC 16 on Windows. The spec target is 200×.

## License

Ambient Pocket is licensed under the GNU General Public License v3.0 (`LICENSE`). Files from
Analogue's openFPGA core template, Intel/Altera-generated IP and PicoRV32 keep their own
licenses; see `THIRD_PARTY_NOTICES.md`.

## Legal

Analogue's Development program was created to further video game hardware preservation with FPGA technology. Analogue Developers have access to Analogue Pocket I/O's so Developers can utilize cartridge adapters or interface with other pieces of original or bespoke hardware to support legacy media. Analogue does not support or endorse the unauthorized use or distribution of material protected by copyright or other intellectual property rights.
