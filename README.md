# Ambient Pocket

A generative ambient music core for the Analogue Pocket (openFPGA). It plays procedurally generated ambient music indefinitely, in one of four styles:

| Mode | Character |
|---|---|
| **DRIFT** | Cold, sparse, orchestral: dark strings, a lonely distant horn, an echoing flute, a low drone, icy clusters and rare stingers, built on silence and dissonance (Alien-era Goldsmith) |
| **TIDES** | Blade Runner / CS-80: swelling detuned brass pads with filter sweeps, vibrato and glide, a sparse melody with pitch "sighs", bells, distant thunder, huge reverb |
| **HUSH** | A lo-fi beat: swung boom-bap drums, jazzy electric-piano chords with tape wow, sine bass, sparse plucked lead, faint vinyl crackle |
| **MOTHER** | No music: the Nostromo's computer room. Chirping data blips, printout chatter, and a very rare motion-tracker sequence, driven by a wandering system-activity level |

`AMBIENT_POCKET_CORE_SPEC.md` is the full specification.

## Status

| Phase | State |
|---|---|
| 0: template + hello tone on hardware | done; v0.1.0 tested on hardware (2026-10-10) |
| 1: C model + composer + renderer | done; signed off after listening round 3 (2026-10-10) |
| 2: voice engine RTL | done; bit-exact vs the C model (Icarus) |
| 3: FX + SRAM | done; v0.2.3 engine test on hardware (2026-10-12), one open item: tick overruns (docs/TEST_REPORT.md) |
| **4: CPU integration** | **next** |
| 5–6: controls/video, timing/soak | not started |

## Layout

```
fw/hal/regs.h          register map shared by composer, model and RTL
fw/hal/hal_model.c     host HAL (drives the C model)   fw/hal/hal_mmio.c  RISC-V HAL
fw/composer/           portable composer: modes, scheduler, PRNG, harmony, voices
model/synth_model.c    bit-exact fixed-point synth engine + FX + master
model/render.c         offline renderer -> WAV
tests/                 unit + system tests
tools/                 table generator, audio analyzer, validation runner
docs/                  decisions, fixed-point formats, memory budget, register map, test report
```

## Building and running (host)

You need GCC and make, plus Python 3 with numpy, scipy, soundfile, pyloudnorm and matplotlib for the analysis.

On Windows (Git Bash) with WinLibs GCC from winget:

```bash
source tools/env.sh
```

```bash
mingw32-make test
```

```bash
./build/render.exe --mode drift --seed 123 --density 3 --brightness 3 --minutes 10 --out drift.wav --events drift.csv
```

```bash
python tools/analyze_audio.py drift.wav --plots
```

```bash
tools/run_validation.sh quick
```

On Linux/WSL2 use `make` and `./build/render`. `tools/run_validation.sh full` runs the nightly matrix: 8 h per mode, 1 h per density/brightness corner, 5 seeds, overflow and switching stress.

### Renderer options

`--mode drift|tides|hush|mother`, `--seed N`, `--density 1-5`, `--brightness 1-5`, `--reverb 0-2`, `--sleep 0-3`,
`--hours/--minutes/--seconds N`, `--out FILE` (`.wav`, RF64 above 4 GB; any other extension = raw s16le),
`--events FILE.csv` (composer trace), `--press 30:RIGHT,90:B`, `--script FILE`, `--tick-start N`,
`--no-autostart`, `--fade-end`.

## Controls (on the Pocket)

| Input | Action |
|---|---|
| D-pad Left / Right | Previous / next mode (6 s crossfade while playing) |
| A or Start | Start (4 s fade-in) / Stop (3 s fade-out to digital silence) |
| B | Reseed: a new piece, crossfaded |
| D-pad Up / Down | Density 1–5 |
| L / R | Brightness 1–5 |
| X | Visuals: Full → Minimal → Off |
| Y | Info overlay |
| Select | Speaker EQ (reserved, v2) |

Menu settings (`interact.json`): mode, density, brightness, reverb Low/Med/High, visuals, lock seed + seed value, sleep timer Off/30/60/90 min, and a Reseed action.

## Known limitations

- The current hardware build (v0.2.x) is an engine test: it replays recorded composer excerpts. The composer runs on the Pocket from Phase 4.
- The host renderer runs at ~90–140× realtime (spec target 200×). See docs/DECISIONS.md D14.
