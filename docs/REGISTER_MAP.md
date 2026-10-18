# Register map

`fw/hal/regs.h` is the authoritative source. This file explains it.

## CPU address map

| Address | Size | Region |
|---|---|---|
| `0x0000_0000` | 64 KB | BRAM: code, data, stack |
| `0x1000_0000` | 4 KB | Voice registers: voice `v`, word `r` at `0x1000_0000 + (v·32 + r)·4` |
| `0x1000_1000` | 256 B | FX / master registers |
| `0x1000_2000` | 256 B | Control / status |
| `0x1000_3000` | 4 B | Visual events (unused in hardware: the UI runs on the same CPU, `hal_visual_event` queues them in RAM) |
| `0x1000_4000` | 24 B | Video control: `VC_PAL0..3` back palette (0x00RRGGBB), `VC_SWAP` (show the back buffer at the next vsync), `VC_STATUS` (bit 0 swap pending, [31:16] vsync count) |
| `0x3000_0000` | 23,040 B | Back framebuffer, words: 320×288, 2 bpp, 16 pixels per word (pixel x%16 in bits 2·(x%16)+1..2·(x%16)), 20 words per line |
| `0x3100_0000` | write | Back framebuffer, single pixels: address + (((y << 9) \| x) << 2), data[1:0] = colour; the hardware merges it |

The CPU runs while the engine is idle at a tick boundary, so voice and FX writes go straight into the engine and take effect at the tick's first sample (DECISIONS D4, D48). In hardware only voice word 31 (STATUS) and FX word 31 read back; other voice and FX words read 0. Any address outside the map sets a bus-error flag (shown on the test screen).

## Voice registers (32 words per voice)

| Word | Name | Format | Notes |
|---|---|---|---|
| 0 | GATE | bit0 gate, bit1 trigger strobe | rising gate edge or trigger → attack (from the current level) |
| 1 | ALGO | 0 PAD, 1 FM, 2 NOISE, 3 KS, 4 SUB, 5 PULSE | KS works only on voices 28–31 |
| 2 | PINC0 | u32 | carrier / osc A |
| 3 | PINC1 | u32 | detuned osc B / FM modulator / harmonic |
| 4 | FM_INDEX | U4.12 | peak index (slewed) |
| 5 | FM_SUS | U4.12 | index after the mod envelope decays |
| 6 | MOD_DECAY | rate code | FM index envelope |
| 7–10 | ATTACK, DECAY, SUSTAIN, RELEASE | rate codes; SUSTAIN U1.15 | |
| 11 | CUTOFF | U0.16 | SVF f (slewed) |
| 12 | RESO | U2.14 | SVF damping q |
| 13 | FMODE | 0 LP, 1 BP, 2 HP, 3 bypass | |
| 14 | AMP | U1.15 | slewed |
| 15 | PAN | 0…32768 | slewed |
| 16 | SEND_REV | U1.15 | |
| 17 | SEND_ECHO | U1.15 | |
| 18 | WAVE | U1.15 | PAD: saw→tri morph; SUB: osc B level |
| 19 | NOISE | 0 white, 1 pink, 2 dust, 3 pink dust | |
| 20 | DUST_RATE | u16 / 65536 per sample | |
| 21 | KS_LEN | U16.16 samples | |
| 22 | KS_DAMP | U1.15 | loop gain |
| 23 | KS_BRIGHT | U0.16 | excitation low-pass |
| 24 | PENV | u32 | osc-0 extra phase increment × mod envelope (pitch drop; kicks) |
| 25 | VIB_RATE | u32 | vibrato LFO phase increment |
| 26 | VIB_DEPTH | U0.16 | vibrato depth target, fraction of the increment (379 ≈ 10 cents) |
| 27 | VIB_DELAY | rate code | depth ramps in from 0 after each trigger (0 = instant) |
| 28 | GLIDE | rate code | oscillator increments slew to PINC0/1 (0 = instant) |
| 29 | SLEW | rate code | cutoff and amp slew (0 = default ~21 ms) |
| 30 | FENV | U0.16 | cutoff boost × amp envelope level |
| 31 | STATUS (RO) | [1:0] env state, [31:16] level U0.16 | snapshot at the tick boundary |

## FX / master registers

| Word | Name | Format |
|---|---|---|
| 0–7 | REV_GAIN0..7 | U0.16 per-line feedback gain (composer computes from T60) |
| 8 | REV_DAMP | U0.16 |
| 9 | REV_WET | U1.15 |
| 10 | REV_PREDELAY | samples 1…4800 |
| 11 | ECHO_FB | U1.15 |
| 12 | ECHO_DAMP | U0.16 |
| 13 | ECHO_WET | U1.15 |
| 14, 15 | ECHO_LEN_L / R | samples 1…36000 |
| 16 | ECHO_TO_REV | U1.15 |
| 17 | DRY | U1.15 |
| 18 | MASTER_TARGET | Q1.23 fade target |
| 19 | MASTER_STEP | per-sample ramp step |
| 20 | TRIM | U2.14 |
| 21 | ECHO_MOD_DEPTH | U8.8 samples (tape wobble, 0 = off) |
| 22 | ECHO_MOD_RATE | wobble LFO phase increment |
| 31 | STATUS_GAIN (RO) | current fade gain |

## Control / status block

| Word | Name | Direction | Notes |
|---|---|---|---|
| 0 | MODE | bridge → CPU | menu: 0 Drift, 1 Tides, 2 Hush, 3 Mother |
| 1 | DENSITY | bridge → CPU | 1…5 |
| 2 | BRIGHT | bridge → CPU | 1…5 |
| 3 | REVERB | bridge → CPU | 0 Low, 1 Med, 2 High |
| 4 | VISUALS | bridge → CPU | 0 Full, 1 Minimal, 2 Off |
| 5 | SEED_LOCK | bridge → CPU | |
| 6 | SEED | bridge → CPU | |
| 7 | SLEEP | bridge → CPU | 0 Off, 1 30 min, 2 60 min, 3 90 min |
| 8 | CMD | bridge → CPU | bit0 reseed; clear-on-read |
| 9 | SETTINGS_SEQ | bridge → CPU | increments on every menu write; the composer re-reads 0–7 when it changes |
| 10 | BTN_EVENTS | input → CPU | pressed-edge bitmap (debounced), clear-on-read. Passes through `pocket_lock` (D62): nothing while locked, Start on its release, Select never |
| 11 | RTC_EPOCH | APF → CPU | |
| 12, 13 | TICK_LO / HI | engine → CPU | 64-bit tick counter |
| 14 | TICK_DONE | CPU → engine | write: this tick's composer work is done; the engine renders the tick's 48 samples, then increments TICK |
| 15 | TICK_CYCLES | engine → CPU | CPU cycles used by the previous tick (diagnostic) |
| 16–25 | ST_* | CPU → video/bridge | state, mode, elapsed s, seed, density, brightness, visuals, overlay, reverb, sleep |
| 26–29 | DIAG_* | engine → CPU | worst CPU cycles per tick, lowest FIFO level, underruns, shortest/longest frame-tick gap (`[31:16]`/`[15:0]`) |
| 30 | LOCK | `pocket_lock` → CPU | Pocket lock (D62): bit 0 locked, bit 1 show the lock screen (3 s after locking and after each press while locked; the UI draws the player screen otherwise) |

## Bridge (APF) addresses

Every read word is captured on the `bridge_rd` strobe and held; `core_top` selects the device by the region of the last strobe (D57).

| Address | Use |
|---|---|
| `0x2000_0000`–`0x2000_FFFF` | Composer firmware data slot (dev builds, D50): writes CPU RAM while the core is in reset; words byte-swapped from the bridge's big-endian order |
| `0x5000_0000` + 4·k | Core menu (D51, `pkg/interact.json`): k = 0 Mode, 1 Density, 2 Brightness, 3 Reverb, 4 Visuals, 5 Lock seed, 6 Seed, 7 Sleep, 8 Reseed (action), 9 Pocket Lock (action, D62). Reads return the composer's last live value, also in reset (D57). |
| `0x6000_0000`–`0x6000_001F` | Save slot `ambient.sav` (slot id 1, D58): 8 words, magic `AMBS`, lock, seed, settings, check word |
| `0xF8xx_xxxx` | APF command block (template `core_bridge_cmd.v`) |
