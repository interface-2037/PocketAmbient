# Ambient Pocket: Generative Ambient Music Core for Analogue Pocket

**Development & Testing Specification (handoff document for Claude Code)**

> **How to use this document:** Treat it as the source of truth for scope, architecture, and acceptance criteria. Anything marked **[VERIFY]** is a hardware or framework detail you must confirm against the current openFPGA developer docs (https://www.analogue.co/developer/docs) and the official core template (https://github.com/open-fpga/core-template) before relying on it. Where this spec and the official docs disagree, the docs win. Record the discrepancy in `docs/DECISIONS.md`.

---

## 0. Goal

Build an openFPGA core for the Analogue Pocket that procedurally generates ambient music **indefinitely** (no loop points, no end) until the user stops it. The user can pick one of **three ambient styles**:

| # | Mode name | Character | Core techniques |
|---|-----------|-----------|-----------------|
| 1 | **DRIFT** | Slow, lush harmonic pads and drones. Think Eno's *Thursday Afternoon* or Stars of the Lid. | Detuned multi-oscillator pads, slow chord-graph walk, long attacks/releases, large reverb |
| 2 | **TIDES** | Sparse, bell-like melodic phrases on incommensurate loops that phase against each other. Think *Music for Airports 1/1*. | FM bell/marimba voices, "Eno loops" with co-prime periods, pentatonic/modal pitch sets, echo |
| 3 | **HUSH** | Textural nature-like soundscape: wind, rain, distant swells, a low sub drone, occasional plucks. | Filtered noise beds, granular-ish shimmer, sub drone, sparse Karplus-Strong plucks |

Each mode must be listenable for 8+ hours with no clicks, no clipping, no stuck notes, no audible repetition at the phrase level, and no silent dropouts.

---

## 1. Hardware Analysis: Analogue Pocket as a Generative Synth Platform

### 1.1 Capabilities (what the core can use)

| Resource | Spec | Relevance to this project |
|---|---|---|
| **Main FPGA** | Intel Cyclone V **5CEBA4F23C8**: ~49K LEs (18,480 ALMs), ~3,080 Kbit M10K block RAM (~385 KB), **66 variable-precision DSP blocks** (132 × 18x18 or 66 × 27x27 multipliers), fractional PLLs | Plenty of room for a time-multiplexed synth engine and a small soft CPU. DSP blocks handle the multiply-heavy DSP (filters, FM, mixing, reverb). |
| **Second FPGA** | Cyclone 10 LP, owned by the APF (scaler/display). **Not available to cores.** | Don't plan around it. |
| **SDRAM** | 64 MB (16-bit bus) **[VERIFY width/timing]** | Not needed for v1. Reserve for v2 (long sample buffers, granular memory). |
| **Cellular RAM (PSRAM)** | 2 × 16 MB, 16-bit **[VERIFY]** | Optional. Could hold long delay lines in v2. |
| **SRAM** | 256 KB async, 16-bit, simple timing | **Primary reverb/delay memory.** Async SRAM is the easiest external memory to drive with deterministic per-sample access. |
| **Audio out** | Core outputs **I2S, 48 kHz, 16-bit signed stereo** to the APF (template module `sound_i2s`). Routed to the speaker, the headphone jack, and Dock HDMI. | Fixed output format. All internal DSP should be higher precision, then dithered/rounded to 16-bit at the end. |
| **Video out** | Core drives RGB888 + DE/HS/VS at its own pixel clock. The APF scaler upscales to the **1600×1440 (10:9), ~615 ppi LCD**, or to HDMI in the Dock. Scaler modes are declared in `video.json`. | A small native resolution that integer-scales well. Recommended **320×288** (exactly 5× to 1600×1440). |
| **Input** | `cont1_key` bitmap: D-pad, A, B, X, Y, L, R, Select, Start. The Analogue button is **reserved by the APF** (menu). Docked controllers add `cont1_joy`/`cont1_trig` and controllers 2–4. | Enough for mode select, start/stop, and a few performance controls. |
| **APF bridge** | SPI-based host bridge exposing a 32-bit address space to the core. Used for data slots, host commands, and `interact.json` menu variables. | Settings menu (mode, density, reverb, visuals) without custom UI code. Settings can persist **[VERIFY `persist` support in interact.json for current firmware]**. |
| **RTC** | Template exposes `rtc_epoch_seconds`, `rtc_date_bcd`, `rtc_time_bcd` **[VERIFY signal names and validity timing]** | Entropy source for the random seed, so each session differs. Optional time-of-day flavoring (e.g. darker at night). |
| **Clocks** | Two 74.25 MHz inputs (`clk_74a`, `clk_74b`) feed fractional PLLs. | Synth clock and video clock come from PLLs; audio MCLK follows the template's `sound_i2s`. |
| **Link port / cart GPIO** | Exposed as `port_tran_*` and `cart_tran_*` with direction control **[VERIFY]**. | Stretch goal: clock-sync pulse out (Pocket Operator / modular-style sync). **Out of scope for v1.** |

### 1.2 Constraints (what shapes the design)

1. **There's no CPU and no OS.** Everything is RTL unless we instantiate a soft CPU. Generative *composition* (probabilistic choices, Markov chains, scheduling) is awkward in RTL but easy in C. Sample-rate DSP is the opposite. → **Hybrid architecture (section 3).**
2. **Audio is fixed at 48 kHz/16-bit stereo.** Ambient material has a wide dynamic range and long reverb tails that expose quantization. → Run ≥24-bit internally, apply TPDF dither and a soft limiter before the I2S output.
3. **BRAM is ~385 KB total**, shared by CPU program/data, wavetables, video framebuffer/tilemap, and FIFOs. A plate/hall reverb needs seconds of delay memory, which doesn't fit in BRAM. → Reverb lives in **external SRAM** (256 KB = 131,072 16-bit samples ≈ 2.73 s of total mono delay across all lines).
4. **The cycle budget per sample is finite but generous.** At a synth clock of ~74.25 MHz, there are **~1,546 cycles per 48 kHz sample**. A single pipelined, time-multiplexed voice datapath can serve 16–32 voices with headroom. → **Time-multiplexed voice engine**, not N parallel voice instances (saves DSP blocks and ALMs).
5. **The speaker is tiny.** It reproduces almost nothing below ~200–300 Hz. Ambient music relies on low pads and sub drones. → Mixes must hold up on the speaker (keep content in 300 Hz–4 kHz) and on headphones/HDMI. An optional "speaker EQ" low-shelf/harmonic-exciter toggle is a stretch goal. Also check **mono compatibility** **[VERIFY whether the internal speaker is mono or stereo]**.
6. **Battery life and power-saving.** The device will be left running for hours. An LCD backlight is on regardless of pixel color, so black pixels don't save meaningful power. FPGA logic toggling does cost power. → Keep the video simple (low pixel clock, mostly static). Clock-gate or idle unused voices. Measure battery runtime (section 8.6). **[VERIFY whether the APF has an auto-sleep/display-timeout that interrupts cores, and whether `sleep_supported` behaves well for an audio-only core]**.
7. **The Analogue button menu can pause or interrupt the core.** **[VERIFY behavior]** Audio must resume cleanly without pops after returning from the menu.
8. **Timing closure on a C8 speed-grade Cyclone V.** Long combinational DSP chains will fail at 74 MHz. → Pipeline all multiply-accumulate paths and register DSP block inputs and outputs.
9. **Long uptime exposes counter overflow.** A 32-bit sample counter at 48 kHz wraps after **~24.85 hours**. → All time counters must be 64-bit or explicitly wrap-safe. This gets its own tests.
10. **Toolchain.** Quartus Prime Lite (template-recommended version, historically 21.1.x **[VERIFY]**). The bitstream must be bit-reversed into `.rbf_r` for the Pocket.

### 1.3 Design implications summary

- **Composer in C on a small RISC-V soft CPU** (event rate ~1 kHz). The **synth engine in RTL** (sample rate 48 kHz).
- **A bit-exact C reference model** of the synth engine lets us render hours of audio offline for analysis, and verify the RTL against it in Verilator.
- **Fixed-point everywhere** in RTL. No floating point.
- **External SRAM** for the reverb. **BRAM** for CPU, wavetables, and video.

---

## 2. Product Specification

### 2.1 User flow

1. Core boots to a **title/mode screen**. The last-used mode is preselected (persisted setting, falling back to DRIFT).
2. The user picks a mode with **D-pad Left/Right** and presses **A** to start. Audio **fades in over 4 s**.
3. Music plays **indefinitely**. The screen shows a slow, mode-specific visual plus a minimal status line (mode name, elapsed time).
4. **A** again = Stop. Audio **fades out over 3 s** to true digital silence (all-zero samples), then the core returns to the mode screen.
5. Changing modes while playing **crossfades over ~6 s**: the old mode's voices release while the new mode's voices fade in. There's no hard cut.
6. Exiting the core goes through the Analogue button → Quit (APF-provided).

### 2.2 Controls

| Input | Action |
|---|---|
| D-pad Left/Right | Previous/next mode (crossfade if playing) |
| A | Start / Stop (with fades) |
| B | "Reseed": new random seed, with a musically gentle transition (no cut) |
| D-pad Up/Down | **Density**: 5 steps, from fewer notes/events to more |
| L / R | **Brightness**: 5 steps, from darker to brighter filter cutoff and FM index |
| X | Cycle visuals: Full → Minimal (text only) → Off (black) |
| Y | Toggle the on-screen info overlay (seed, density, brightness, uptime) |
| Select | Toggle the "speaker EQ" (stretch goal; no-op in v1) |
| Start | Reserved (same as A) |

Hardware volume stays under APF control. Don't implement a separate master volume button.

### 2.3 `interact.json` menu (Pocket core settings)

Expose these settings through the APF menu:
- Mode (list: Drift, Tides, Hush)
- Density (list 1–5)
- Brightness (list 1–5)
- Reverb amount (list: Low/Med/High)
- Visuals (list: Full/Minimal/Off)
- Lock seed (check) plus Seed value (if feasible) for reproducible sessions
- Action: Reseed

**[VERIFY]** interact variable types, address ranges, and persistence semantics in the current docs. Each maps to a bridge register in the core's control block (section 3.4).

### 2.4 Visuals (low priority, keep simple)

- Native **320×288 @ ~60 Hz**, 5× integer scale on the handheld.
- DRIFT: slowly morphing horizontal color bands (palette cycling, no framebuffer needed).
- TIDES: dots or ripples that appear when bell notes trigger and fade out over time. Use a small sprite list in BRAM.
- HUSH: dim, slow noise field or "rain lines".
- Text uses an 8×8 font ROM in BRAM for the mode name, status, and overlay.
- Keep visuals calm. No flashing above 3 Hz (photosensitivity).

---

## 3. Architecture

### 3.1 Block diagram

```
                ┌───────────────────────── core_top.v (from template) ───────────────────────────┐
  APF bridge ──►│ bridge_ctrl ──► ctrl_regs (mode, density, brightness, seed, cmd) ◄── input_ctrl │◄── cont1_key
  (SPI)         │                     │  ▲                                                       │
                │                     ▼  │ status                                                │
  RTC ─────────►│              ┌──────────────┐  voice/fx param writes  ┌───────────────────┐   │
                │              │  COMPOSER    │ ──────────────────────► │  SYNTH ENGINE     │   │
                │              │  PicoRV32    │   (MMIO, ~1 kHz rate)   │  time-multiplexed │   │
                │              │  RV32IM      │ ◄────────────────────── │  32 voices        │   │
                │              │  64 KB BRAM  │   tick / voice status   │  + FX bus         │   │
                │              └──────────────┘                         └─────────┬─────────┘   │
                │                                                                  │ dry/send    │
                │                                         ┌────────────────────────▼─────────┐   │
                │                       SRAM 256 KB ◄────►│  FX: 8-line FDN reverb + echo    │   │
                │                                         │  master: EQ, soft limiter, dither│   │
                │                                         └────────────────────────┬─────────┘   │
                │                                                     async FIFO   │ 48 kHz      │
                │                                                                  ▼             │
                │                                                       sound_i2s (template) ───►│──► APF audio
                │   video_gen (320x288) ◄── visual events ◄── composer / synth status           │──► APF video
                └────────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Clock domains

| Domain | Freq (target) | Contents |
|---|---|---|
| `clk_74a` | 74.25 MHz | Bridge (per template) |
| `clk_sys` | 74.25 MHz (PLL), may drop to ~49–60 MHz if timing is tight | Composer CPU, synth engine, FX, SRAM controller |
| `clk_audio` | Per template `sound_i2s` (MCLK 12.288 MHz, 48 kHz LRCK) **[VERIFY]** | I2S serializer |
| `clk_video` | ~7–8 MHz pixel clock (+ 90° phase clock per template) | video_gen |

- Synth → audio crossing: a **small async FIFO** (e.g. 16 stereo frames). The synth is **pull-driven**: it computes the next sample whenever the FIFO isn't full, so the sample rate is locked to the I2S clock. No drift, no underruns.
- All control registers crossing into `clk_sys` go through standard 2-FF synchronizers or handshake CDC.

### 3.3 Composer (soft CPU)

- **CPU:** PicoRV32 (single Verilog file, RV32IM, simple to integrate). VexRiscv is an acceptable alternative if more performance is needed. At ~37–74 MHz, PicoRV32 gives ~10–20 MIPS, far more than an event-rate composer needs.
- **Memory:** 64 KB BRAM for code and data, initialized from a `.mif`/`.hex` built by the firmware Makefile. No external memory for the CPU in v1.
- **Toolchain:** `riscv64-unknown-elf-gcc` (or `riscv32-...`) with `-march=rv32im -mabi=ilp32 -Os`, freestanding, plus a custom linker script and crt0. No libc floats. Use fixed-point integer math only.
- **Timing:** The synth engine raises a **control tick at 1 kHz** (every 48 samples). The composer runs its scheduler on each tick. All musical time is measured in ticks (`uint64_t`).
- **Portability (critical):** Composer sources (`fw/composer/*.c`) must compile **unchanged** for:
  1. the RISC-V target, and
  2. the host (x86/ARM) offline renderer and test harness.

  Hardware access goes through a thin HAL (`hal_write_voice()`, `hal_read_ctrl()`, ...) with an MMIO implementation and a host-model implementation.

### 3.4 Memory map (CPU view, suggested)

| Address | Size | Region |
|---|---|---|
| `0x0000_0000` | 64 KB | BRAM: code + data + stack |
| `0x1000_0000` | 4 KB | Voice parameter file (32 voices × 32 words) |
| `0x1000_1000` | 256 B | FX/master parameters |
| `0x1000_2000` | 256 B | Control/status: mode, density, brightness, seed, cmd, tick counter (64-bit), RTC epoch, button state |
| `0x1000_3000` | 256 B | Visual event FIFO (composer → video_gen) |

The bridge-facing control registers (APF address space) mirror the control block in a separate address range. **[VERIFY]** which bridge address ranges are reserved by the APF.

### 3.5 Synth engine (RTL)

**Datapath:** One deeply pipelined voice processor, time-multiplexed across **32 voice slots** per sample. Each voice is evaluated in a fixed number of cycles (target ≤ 32), so 32 voices × 32 = 1,024 cycles, well under the ~1,546-cycle budget at 74.25 MHz. The remaining cycles go to FX and master.

**Per-voice state (in BRAM or MLAB register file):**

| Field | Format | Notes |
|---|---|---|
| `phase_inc[0..1]` | u32 | Two oscillators per voice (detune / FM modulator) |
| `phase[0..1]` | u32 | Accumulators (wrap naturally, so long runs are safe) |
| `algo` | u3 | 0 = dual-saw/tri pad, 1 = 2-op FM, 2 = filtered noise, 3 = Karplus-Strong pluck, 4 = sine sub |
| `fm_index` | Q4.12 | FM depth / waveshape amount |
| `env_state`, `env_level` | u2, Q1.23 | ADSR with **exponential-ish segments** (linear in log domain or one-pole) |
| `attack_rate`, `decay_rate`, `sustain`, `release_rate` | u16 each | Must support very long envelopes (attack up to 30 s, release up to 60 s) |
| `gate` | bit | Set and cleared by the composer |
| `cutoff`, `reso` | Q1.15 | Chamberlin or TPT state-variable LP/BP filter, 2× oversampled if unstable at high cutoff |
| `amp`, `pan` | Q1.15 | Equal-power pan via sine LUT |
| `send_rev`, `send_echo` | Q1.15 | FX sends |
| **Slewed copies** of `cutoff`, `amp`, `pan`, `fm_index` | | One-pole parameter smoothing, so composer writes never zipper or click |

**Oscillators:**
- Sine: 4,096-entry quarter-wave LUT (16-bit) with linear interpolation, in BRAM.
- Saw/tri: PolyBLEP or a 2× oversampled naive waveform plus the filter. Aliasing must stay inaudible at the pad register (≤ 2 kHz fundamentals).
- Noise: 32-bit xorshift or a 31-bit LFSR per voice slot. Pink-ish noise via a 3-pole filter approximation.
- Karplus-Strong pluck: needs per-voice delay memory. Limit to **4 pluck-capable slots** with 2,048-sample BRAM delay lines each (16 KB total).

**Mixing:** 32-bit signed accumulators for the dry L/R, reverb send, and echo send buses.

**FX (shared, run once per sample after voices):**
- **Reverb:** 8-line feedback delay network (FDN) with a Householder or Hadamard mixing matrix (adds/subtracts only, no multipliers), one-pole damping per line, slow modulation of 2 line lengths (chorus-like, to avoid metallic ringing), and an input diffuser of 4 allpasses. Delay lines live in **external SRAM**, with mutually prime lengths spread over ~30–300 ms, plus a pre-delay. Decay ranges from 3 s (Low) to 20+ s (High, used heavily in DRIFT).
- **Echo:** Stereo ping-pong delay with a low-pass in the feedback path (TIDES). Uses up to ~800 ms per side of the remaining SRAM, or a reduced length if the SRAM budget is tight. Document the SRAM allocation in `docs/MEMORY_BUDGET.md`.
- **SRAM controller:** Fixed TDM schedule per sample (e.g. 16 reads + 16 writes) on the 16-bit async SRAM. Verify access timing against the SRAM datasheet and template constraints **[VERIFY]**.

**Master chain:**
1. Gentle high-pass at ~25 Hz (removes DC and subsonic content)
2. Optional speaker EQ (v2)
3. **Mode crossfader and fade in/out gain** (64-step-per-ms smooth ramps)
4. **Soft limiter / soft clipper** (tanh-ish LUT) with ceiling at **-1 dBFS**
5. **TPDF dither**, then round to 16-bit
6. → async FIFO → I2S

**Fixed-point conventions:**
- Audio samples: Q1.23 internally (24-bit), within 27×27 DSP multiplies.
- Coefficients: Q1.17 or Q2.16 as needed. Document every format in `docs/FIXED_POINT.md`.
- Saturating arithmetic at every bus summation point. **Never wrap** on overflow.

### 3.6 Composer algorithms (per mode)

Shared infrastructure (all in C, all fixed-point and integer):
- **PRNG:** xoshiro128** seeded from (RTC epoch ⊕ user seed). Separate PRNG streams per subsystem (harmony, rhythm, timbre) so changing one doesn't perturb the others.
- **Scheduler:** A tick-based event queue (min-heap, ≤ 256 events).
- **Voice allocator:** Prefers free voices, then the quietest voice in release. Never steal a voice in its attack phase. Each mode gets a voice budget.
- **Pitch system:** MIDI-style note numbers → `phase_inc` via a 128-entry table plus fine-tune cents (16-bit). Supports just-intonation tables for DRIFT.
- **Macro form:** Every mode has a slow **"weather" layer** (2–20 minute cycles) that modulates density, brightness, register, and harmonic tension using summed slow LFOs with irrational-ratio periods. This prevents a static "same vibe forever" feel.
- **Key/mode drift:** Every 8–25 minutes (randomized), modulate to a related key or mode (circle-of-fifths neighbor, relative major/minor, or a shift by a common tone). Use long crossfaded chord voicings for these transitions.

**DRIFT (pads/drones)**
- Harmony: random walk over a **weighted chord graph** in the current key (I, ii, iii, IV, vi, sus2, sus4, add9, maj7). Transition weights favor common-tone moves. Chord duration is 20–90 s.
- Voicing: 4–6 voices spread over 2–3 octaves (C2–C5 range), voice-leading by minimal motion.
- Timbre: detuned dual saw/tri (±3–8 cents) → low-pass filter with a slow cutoff LFO per voice. Attack 6–20 s, release 15–40 s.
- An occasional high "shimmer" sine voice (one octave or a fifth above) at low level.
- FX: reverb High (12–20 s decay), echo off.

**TIDES (Eno-style loops)**
- At start (and on each key change), generate **5–7 "loop tracks"**. Each track holds a short phrase of 1–3 notes from a pentatonic or modal pitch set and repeats with a **unique period** drawn from a set of mutually prime durations (e.g. 17.3 s, 19.7 s, 23.1 s, 29.9 s, 31.4 s, 37.6 s). Their alignments drift endlessly, so the overall sequence never repeats within a session.
- Each phrase has a per-repeat probability of skipping, small timing jitter (±40 ms), and slow velocity variation.
- Every N repeats (randomized), a loop "mutates": one note changes by a scale step or is replaced, or the loop retires and is replaced. This keeps the material from going stale.
- Timbre: 2-op FM bell (ratio 1:3.5 or 1:1.41 inharmonic) or soft marimba (ratio 1:4, fast index decay). Optional soft sine pad underneath at very low level.
- FX: reverb Medium (6–10 s), stereo echo at a tempo-free time (e.g. 430 ms / 610 ms), feedback ~35%.

**HUSH (textures)**
- Beds: 2–3 filtered noise voices (wind = band-pass with a slowly wandering center and Q; rain = high-passed noise with random amplitude micro-bursts; surf = low-passed noise with 8–14 s swell envelopes).
- Sub drone: a sine voice on the tonic (A1–E2 range) with a very slow amplitude LFO. Add a 2nd harmonic so it remains audible on the small speaker.
- Events: sparse Karplus-Strong plucks or distant FM "glass" tones from a minor pentatonic set, at a Poisson rate of 1 per 8–40 s (scaled by density).
- Occasional "swell": a 30–60 s pad chord rising out of the noise and receding.
- FX: reverb Medium-High, echo low.

**Density and brightness mapping:** Each mode defines a table mapping Density 1–5 → (event rate multiplier, max active voices, loop count) and Brightness 1–5 → (cutoff offset, FM index scale, shimmer level). The "weather" layer modulates *around* these user settings.

---

## 4. Repository Layout

```
ambient-pocket/
├── README.md
├── docs/
│   ├── DECISIONS.md          # running log of design decisions & [VERIFY] resolutions
│   ├── FIXED_POINT.md
│   ├── MEMORY_BUDGET.md      # BRAM/SRAM allocation tables
│   ├── REGISTER_MAP.md       # CPU MMIO + bridge registers
│   └── TEST_REPORT.md        # filled in per release
├── src/fpga/                 # Quartus project, based on open-fpga/core-template
│   ├── apf/                  # template APF files (unmodified where possible)
│   ├── core/
│   │   ├── core_top.v
│   │   ├── ctrl_regs.sv
│   │   ├── cpu/picorv32.v
│   │   ├── synth/            # voice_engine.sv, osc_*.sv, svf.sv, env.sv, mixer.sv
│   │   ├── fx/               # fdn_reverb.sv, echo.sv, sram_ctrl.sv
│   │   ├── master/           # hpf.sv, limiter.sv, dither.sv, fades.sv
│   │   ├── video/            # video_gen.sv, font_rom, palettes
│   │   └── cdc/              # async_fifo.sv, sync.sv
│   └── ap_core.qsf / .sdc
├── fw/
│   ├── composer/             # PORTABLE C: modes, scheduler, prng, harmony, allocator
│   ├── hal/                  # hal_mmio.c (RISC-V), hal_model.c (host)
│   ├── rv/                   # crt0.S, linker.ld, Makefile → composer.hex/.mif
├── model/
│   ├── synth_model.c         # BIT-EXACT C model of synth engine + FX + master
│   └── render.c              # offline renderer: composer + synth_model → WAV
├── sim/
│   ├── verilator/            # tb harness comparing RTL vs synth_model
│   └── vectors/
├── tools/
│   ├── analyze_audio.py      # loudness, peaks, DC, clicks, repetition, spectra
│   ├── reverse_bits.py       # .rbf → .rbf_r (or use template's tool)
│   └── package.py            # builds SD-card release zip
├── dist/                     # SD card layout (generated)
│   ├── Cores/<Author>.AmbientPocket/
│   │   ├── bitstream.rbf_r
│   │   ├── core.json  data.json  input.json  interact.json  video.json  variants.json
│   │   └── icon.bin
│   ├── Platforms/ambient.json
│   └── Platforms/_images/ambient.bin
└── .github/workflows/ci.yml  # host tests + sims + firmware build (Quartus build optional/self-hosted)
```

Author name and platform category in the JSON files are placeholders to confirm with the user. **[VERIFY]** the current JSON schemas, including required fields, `framework.version_required`, `sleep_supported`, `dock`, the `cartridge_adapter` value for no cart, and data slot rules for a core with **no required data files**.

---

## 5. Toolchain & Environment

| Tool | Purpose |
|---|---|
| Quartus Prime Lite (template-recommended version) | Synthesis, P&R, timing |
| Verilator ≥ 5.x | RTL simulation vs C model |
| Icarus Verilog (optional) | Quick unit sims |
| RISC-V GCC (rv32im, ilp32) | Composer firmware |
| GCC/Clang (host) | Composer + model unit tests, offline renderer |
| Python 3.11+ with numpy, scipy, soundfile, pyloudnorm | Audio analysis |
| A unit test framework for C (e.g. Unity or a minimal custom one) | Composer tests |
| Make (or CMake) | Orchestration. Provide `make test`, `make render`, `make sim`, `make fpga`, `make package` |

The user is on **Windows**. Make sure builds work under WSL2 or MSYS2 and document both paths. Quartus runs natively on Windows. Verilator is easiest in WSL2.

---

## 6. Development Phases & Milestones

Each phase ends with its acceptance criteria passing in CI (except hardware steps). Don't start phase N+1 RTL until phase N's model is stable.

### Phase 0: Scaffolding and "hello tone" on hardware
- Clone the core template. Make it build and load on the Pocket.
- Output a **440 Hz sine at -20 dBFS** through `sound_i2s`, and show a solid color plus the core name on screen.
- **Acceptance:** Audible clean tone on the speaker and headphones. The core appears in the Pocket menu with its platform image. `make package` produces a working SD layout.

### Phase 1: Host-side C model and composer (no FPGA)
- Implement `synth_model.c` (voices, filters, envelopes, FX, master) in **exactly the fixed-point arithmetic the RTL will use**.
- Implement the composer with all 3 modes against `hal_model.c`.
- Implement `render`: `render --mode drift --seed 123 --density 3 --brightness 3 --hours 8 --out drift.wav`.
- **Acceptance:** The section 8.3 automated audio checks pass for all 3 modes at all density/brightness corners over 8 rendered hours. Include a short listening review (section 8.5): render 10-minute excerpts per mode and have the user listen.

> This is where the music is designed. Iterate on the composer in C until the output sounds good *before* writing the RTL synth. Render speed should be ≥ 200× real time on a desktop.

### Phase 2: Synth engine RTL (bit-exact)
- Implement the voice engine, oscillators, filter, envelopes, and mixer in SystemVerilog.
- Verilator testbench: drive identical register-write sequences into the RTL and `synth_model.c`, then compare outputs **sample-for-sample, bit-exact**.
- **Acceptance:** Zero mismatches over ≥ 10 million samples of randomized parameter stimulus, plus recorded composer traces for all modes.

### Phase 3: FX and SRAM
- FDN reverb, echo, and the SRAM controller. Use an SRAM behavioral model in sim.
- **Acceptance:** Bit-exact vs model. The SRAM TDM schedule fits the per-sample budget with ≥ 20% slack.

### Phase 4: CPU integration
- Integrate PicoRV32, BRAM, and MMIO. Build the firmware into a `.mif`.
- Verilator full-system sim: CPU runs the real composer firmware and drives the synth. Compare against the host render with the same seed (bit-exact, since the composer is deterministic given seed and tick).
- **Acceptance:** ≥ 60 simulated seconds per mode with a bit-exact match to the host render.

### Phase 5: Controls, bridge, interact menu, video
- Input handling (debounce, repeat on D-pad), fades, crossfades, reseed, `interact.json` mapping, and the RTC seed.
- `video_gen` with the 3 visual modes and the text overlay.
- **Acceptance:** All controls work on hardware per section 2.2. Settings survive core reload **[if persistence is supported]**.

### Phase 6: Timing closure, power, soak
- Close timing at all corners. Run hardware soak tests and battery tests (section 8.6).
- **Acceptance:** Section 9 Definition of Done.

---

## 7. Coding Standards

- **RTL:** SystemVerilog (Quartus-supported subset), `default_nettype none`, synchronous resets inside cores, one clock per module where possible. Every DSP multiply is registered in and out. No latches; check the Quartus warnings and keep them clean.
- **C:** C99/C11 freestanding subset. No `float`/`double` in the composer or model. No dynamic allocation. All integer widths explicit (`stdint.h`). Time is `uint64_t ticks`.
- **Determinism:** Given (seed, mode, settings, input event log with tick timestamps), output must be bit-identical across host, Verilator, and hardware. This property is what makes the test strategy work. Protect it.
- Every [VERIFY] resolution and every design change gets a line in `docs/DECISIONS.md`.

---

## 8. Testing Strategy

### 8.1 Test pyramid

```
            ┌─────────────────────────────┐
            │ Hardware tests (manual +    │  few, long, real device
            │ recorded captures)          │
            ├─────────────────────────────┤
            │ Full-system Verilator       │  CPU + firmware + synth, bit-exact vs host
            ├─────────────────────────────┤
            │ Module RTL sims (Verilator) │  each block bit-exact vs C model
            ├─────────────────────────────┤
            │ Long offline renders +      │  hours of audio, automated metrics
            │ audio analysis (Python)     │
            ├─────────────────────────────┤
            │ C unit tests (composer,     │  fast, many
            │ model, PRNG, scheduler)     │
            └─────────────────────────────┘
```

### 8.2 C unit tests (`make test`)

| Area | Tests |
|---|---|
| PRNG | Known-answer vectors. Streams are independent. Seed 0 is handled. |
| Scheduler | Ordering, ties, capacity limit behavior (drop lowest-priority, never crash), tick wrap-safety with ticks near `UINT64_MAX - k` (paranoia) and **near 2^32** (realistic) |
| Voice allocator | Never steals a voice in attack. Respects per-mode budget. All voices are eventually freed after gate-off plus release. |
| Harmony | Every generated note lies in the current scale/chord set. Key changes only go to allowed neighbors. Voice-leading max jump is within bounds. |
| TIDES loops | Loop periods are pairwise distinct and non-trivially related (no ratio p/q with small p, q ≤ 4). Mutations keep notes in scale. |
| Fixed-point math | Saturation behavior, rounding mode, filter stability over the whole cutoff/resonance parameter grid (no NaN-equivalents: no runaway, bounded output for bounded input). |
| Envelopes | Attack/release times match spec within 5%. Very long envelopes (60 s) complete. Retrigger during release is click-free (no discontinuity > threshold). |
| Fades/crossfades | Monotonic gain ramps. Stop ends in exact zeros. |

### 8.3 Long-render automated audio analysis (`tools/analyze_audio.py`)

Render **8 hours per mode** at the default settings, and **1 hour per mode** at each density/brightness corner (1/1, 1/5, 5/1, 5/5). Also render with **5 different seeds** per mode. Pass criteria:

| Metric | Threshold |
|---|---|
| Sample peak | ≤ -1.0 dBFS. **Zero** samples at ±32767/-32768. |
| True peak (4× oversampled) | ≤ -0.5 dBTP |
| Integrated loudness | **-20 LUFS ± 3** per mode. Modes within **±2 LU** of each other at default settings. |
| Short-term loudness (3 s) | Never below -50 LUFS for > 20 s while playing (no "dead air"), except during the intentional fade-out |
| Loudness jumps | No 3 s window more than 8 LU louder than the previous 30 s average (no startling swells) |
| DC offset | \|mean\| < -60 dBFS per 10 s window |
| Clicks/discontinuities | High-pass the signal at 8 kHz and flag any sample whose derivative exceeds the local RMS × K (tune K on known-clean material). **Zero flagged events.** Also run a dedicated test that forces voice steals, reseeds, and mode switches every 5 s for 30 min. |
| Spectral sanity | Energy above 16 kHz < -50 dB relative to total (aliasing check). Energy below 20 Hz < -40 dB relative to total. |
| Stuck notes | From the composer event log, every gate-on has a matching gate-off within the mode's max note length. Voice count never stays at budget max for > 60 s. |
| Repetition | Hash composer event-stream windows (pitch, voice, quantized time-delta) of 30 s length. **No identical 30 s window appears twice in 8 h.** Also compute audio chroma self-similarity and flag any off-diagonal stripe of ≥ 60 s with similarity > 0.98. |
| Pitch-class distribution | Matches the active scale. Entropy above a floor (not stuck on one chord). Each mode visits ≥ 3 different keys or modes over 8 h. |
| Mono compatibility | (L+R)/2 loudness within 3 LU of the stereo loudness. No sustained phase-cancellation drop > 6 dB. |
| Determinism | Two renders with the same seed are byte-identical. Different seeds differ within the first 5 s. |
| Long-run overflow | Run renders with the internal tick counter **pre-initialized to 2^32 − 10 s** and 2^31 − 10 s. Output must be continuous across the boundary (no glitch, no scheduler freeze). |

The analysis script writes a JSON summary plus PNG plots (loudness over time, spectrogram thumbnails, self-similarity matrix) into `reports/<date>/`. CI fails on any threshold breach.

### 8.4 RTL verification

- **Module level:** Each synth, FX, and master block gets a Verilator testbench fed by randomized and directed stimulus, compared **bit-exact** to the corresponding `synth_model.c` function. Directed cases: max resonance, cutoff extremes, max FM index, all voices gated simultaneously, envelope rate extremes, saturation on every bus.
- **SRAM controller:** Behavioral async SRAM model with datasheet timing checks (setup/hold/output-enable violations assert).
- **CDC:** Async FIFO tests with randomized clock ratios. Run a reset-during-traffic test. Run Quartus CDC/timing analysis to confirm all crossings are constrained or false-pathed intentionally.
- **Full system:** CPU running the real firmware, compared against the host render (Phase 4). Include scripted button-press logs (start, mode switch, reseed, stop).
- **Coverage goals:** Every `algo` value, every envelope state transition, every mode, saturation hit on every accumulator, and FIFO full/empty boundaries.
- **Timing:** Quartus Timing Analyzer shows **zero negative setup/hold slack** across all corners (slow 85 °C, slow 0 °C, fast). Keep the resource report under ~70% ALM and ~80% M10K for headroom.

### 8.5 Musical listening review (human-in-the-loop)

Automated metrics can't judge "pleasant". For each mode, render 3 × 10-minute excerpts (start, 2 h, 6 h into an 8 h render) and a 1-minute mode-switch montage. Ask the user to rate them, and capture the feedback in `docs/TEST_REPORT.md`:
- Is it calm? Are any sounds harsh, too loud, or too bright?
- Does it feel evolving rather than static, and non-repetitive?
- Are the three modes clearly distinct?
- Does it sound good on a phone speaker (proxy for the Pocket speaker)?

Iterate the composer before freezing the RTL.

### 8.6 Hardware test matrix (manual, recorded in `TEST_REPORT.md`)

| ID | Test | Pass condition |
|---|---|---|
| HW-01 | Boot & load from SD | Core is listed, loads in < 5 s, mode screen appears |
| HW-02 | Each mode on the internal speaker | Audible, no distortion at max volume, no buzz |
| HW-03 | Each mode on headphones | No hiss in silence (fade-out end = digital silence), no clicks |
| HW-04 | Docked HDMI audio + video | Audio over HDMI, visuals scale correctly |
| HW-05 | All controls (section 2.2) | Each behaves as specified. No double-triggers. |
| HW-06 | Mode switch × 50 rapid presses | No crash, no click, no stuck voices. Final mode plays correctly. |
| HW-07 | Reseed × 50 | Each is a smooth transition |
| HW-08 | Analogue-button menu open/close while playing | Audio pauses or continues per APF behavior and resumes without a pop |
| HW-09 | interact.json settings | Each menu item takes effect. Persistence across core reload works (if supported). |
| HW-10 | **Capture comparison** | Record headphone output (or HDMI audio capture) for 60 s with a locked seed. Correlate against the host render with the same seed and button log. Expect high similarity. Bit-exact via HDMI capture is a stretch goal. |
| HW-11 | **8-hour soak** per mode (on dock power) | Still playing, no glitches heard in spot checks, no visual corruption |
| HW-12 | **26-hour soak** (one mode, dock power) | Passes the 24.85 h 32-bit-counter boundary cleanly |
| HW-13 | Battery rundown from 100% | Record runtime. Behavior at low battery is graceful (APF handles shutdown). Compare against an idle-core baseline. |
| HW-14 | Sleep/wake (if `sleep_supported`) | Resumes cleanly with a fade-in, no burst of noise |
| HW-15 | Thermals | No abnormal heat after 8 h |
| HW-16 | Visuals Off mode | Black screen, audio unaffected |

### 8.7 CI (`.github/workflows/ci.yml`)

On every push: build host tests and run `make test`, build the RISC-V firmware, run the Verilator module and full-system sims (short versions), and run a 10-minute render per mode through the analyzer. **Nightly:** 8-hour renders, the overflow-boundary renders, and long randomized RTL sims. Quartus builds need a self-hosted runner or run locally. Provide `make fpga` and `tools/package.py`.

---

## 9. Definition of Done (v1.0)

- [ ] All three modes are implemented and pass every section 8.3 metric across seeds and setting corners
- [ ] RTL is bit-exact with the C model (module and full system)
- [ ] Timing closed at all corners with zero violations. Resource use is within headroom targets.
- [ ] Hardware matrix HW-01…HW-16 passes and is recorded in `docs/TEST_REPORT.md`
- [ ] The user has signed off on the musical listening review
- [ ] SD-card release zip built by `tools/package.py` with the correct folder structure and JSON files
- [ ] README covers install, controls, settings, and known limitations
- [ ] Every `[VERIFY]` item in this document is resolved in `docs/DECISIONS.md`

---

## 10. Risks & Open Questions

| Risk / Question | Mitigation |
|---|---|
| openFPGA schema or firmware details differ from this spec | Resolve every [VERIFY] first (Phase 0). Docs win. |
| PicoRV32 + 64 KB BRAM + wavetables + video exceed the BRAM budget | Maintain `MEMORY_BUDGET.md` from day one. Fallbacks: shrink the CPU RAM to 32 KB, move echo lines to PSRAM, or simplify video to a tile/text mode. |
| Timing at 74.25 MHz fails | Drop `clk_sys` to ~49–60 MHz. The cycle budget still allows 32 voices at ~32 cycles each. |
| Reverb sounds metallic in fixed point | Modulated FDN lines, a diffuser stage, and tuning in the C model before the RTL. |
| Ambient lows are inaudible on the speaker | Add a 2nd/3rd harmonic to the sub voices. Optional speaker EQ. Test early on HW-02. |
| APF auto-sleep or menu interrupts "indefinite" playback | Investigate in Phase 0. Document the behavior. Use `sleep_supported` if useful. |
| The output feels repetitive over hours despite the metrics | Macro "weather" layer, key drift, loop mutation, per-session RTC seed. The listening review gates release. |
| Bit-exactness between C and RTL is costly to maintain | It's worth it. It's the only practical way to test hours of output. Keep the model structure mirroring the RTL pipeline. |

**Questions to ask the user before or while starting:**
1. Author name and core name for `Cores/<Author>.<Core>`, plus the platform name and category.
2. Is there any preference on specific musical influences or keys per mode, or are the defaults above fine?
3. Should there be an optional **sleep timer** (e.g. fade out after 30/60/90 min), or strictly indefinite?
4. Is link-port sync output wanted in a later version?

---

## 11. Reference Links

- openFPGA developer docs: https://www.analogue.co/developer/docs
- Core template: https://github.com/open-fpga/core-template
- PicoRV32: https://github.com/YosysHQ/picorv32
- Verilator: https://verilator.org
- Background reading: Jot/FDN reverbs (Jot 1991), Chamberlin SVF, Välimäki PolyBLEP, Eno "Music for Airports" tape-loop technique, Karplus-Strong (1983)
