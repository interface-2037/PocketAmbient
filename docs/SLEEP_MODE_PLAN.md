# Sleep mode: research and plan (2026-10-18)

**Status:** plan A is implemented as D62 (v0.5.0); the hardware checks are in `docs/HW_TEST_NEXT.md`.

The sleep **timer** is already done (D2: Off / 30 / 60 / 90 min, 30 s fade). This plan covers the
Pocket's own **sleep / wake** (power button): `core.json` `"sleep_supported"`, today `false`.
Goal from the spec (HW-14): after waking, the music comes back cleanly, with a fade-in and no burst of noise.

## What is known

Analogue docs [Doc] say only that `sleep_supported` turns sleep on and that it runs through the
save-state host commands 0xA0 (Savestate Start/Query) and 0xA4 (Load/Query). The core reports
supported, an address and a size, and the busy / ok / err status. The docs do not say what the
Pocket does with `false`, or whether the core is running or in reset during a load.

PocketIR v0.12.0 measured it on hardware [HW] (`pocket-learnings.md` §4, *Sleep / wake through
save states*; code in `PocketIR/src/fpga/core/lib/sleep_state.v`):

- **Sleep:** the APF raises `savestate_start`. The core acks, is busy for one cycle, then ok. The APF
  reads `size` bytes from `addr` over the bridge and writes them to `/System/user_sleepstate.sta`.
- **Wake is a cold start.** The FPGA is configured again, so every register, the CPU RAM and the
  reverb SRAM are lost. The order is: data slots written while reset is held (in PocketIR, the
  `.sav`), then `dataslot_allcomplete`, then Reset Exit, so the core boots normally. **Then the
  APF writes the blob while the core is running**, then sends Savestate Load.
- A small core needs no copy. The save state can be a bridge window aliased onto registers that
  already exist.
- Turning on save states also turns on the **Memories** menu for the core (save / load at any time).

In this core the `savestate_*` outputs in `core_top.v` (lines 370–385) are not driven, so they read 0
(not supported). `clk_74a` is the clock for both the bridge and `ap_top`, so no clock crossing is needed.

## The choice: what goes in the blob

| | A. Resume blob (recommended) | B. Full snapshot |
|---|---|---|
| Contents | ~0x40 bytes: settings, seed, play state, elapsed time | 64 KB CPU RAM + DSP voice state + 256 KB reverb SRAM + video |
| After wake | Same mode, same seed, same settings; the piece **starts again** with a fade-in. The elapsed time and the sleep timer carry on | The music continues from the exact point |
| Cost | A register window plus a few firmware lines. No BRAM (M10K is at 98 %) | A read port into the CPU RAM, freezing the CPU and DSP for a consistent copy, saving and restoring DSP state, a large blob. Big and risky |
| Noise risk | None: the core wakes silent at the title, then fades in | The reverb tail and the voices must come back together, or there is a click |

Generative music has no fixed timeline, so "the same piece, the same mood, fading back in" is what
the user hears as resuming. **Recommend A.** B can come later if the restart turns out to be a problem.

## Plan for A

**Blob** (bridge window, for example `0x7000_0000`, 0x20 bytes, big-endian words as in `ambient.sav`):

| Word | Contents |
|---|---|
| 0 | magic `AMSS` |
| 1 | `{format 1, …, lock, state[1:0]}` (state: title / playing / stopping) |
| 2 | the locked seed |
| 3 | the session seed (the piece that was playing) |
| 4 | settings, in the same layout as `ambient.sav` word 3 (D58) |
| 5 | elapsed play time in seconds (for the sleep timer and the screen) |
| 6 | 0 |
| 7 | check word, the same formula as D58 |

**RTL**
1. A new module `sleep_state.sv`, based on PocketIR's: `supported = 1`, the address, size and max
   load size, and the ack → busy → ok handshake on the rising edge of the request.
2. **Sleep:** on `savestate_start`, latch an image of the live state (`live_*`, lock, seeds,
   `st_state`, `st_elapsed`) into 8 words that the bridge reads. A snapshot keeps the 8 words
   consistent while the composer keeps running. Add a read capture on the strobe (D57) and a
   `rd_region` case in `core_top.v`.
3. **Wake:** bridge writes to the window fill a load buffer. On `savestate_load`, check the magic,
   the format, the check word and the field ranges (the D59 checks). If they pass, set `resume_seq++`
   and expose the words to the CPU through new control registers (`CR_RESUME_*`; `CR_NUM_REGS` grows
   past 32). A load that fails the checks is still acked (ok) but changes nothing.
4. `settings_bridge.sv` takes the resumed settings into `cfg_*` and `live_*`, as at a D59 cold start,
   so the menu shows them.

**Firmware** (`composer.c`): in `composer_tick`, when `resume_seq` changes:
- apply the settings and seeds, then `new_session_seed(session seed)`;
- if the state was playing: `do_start`, with `play_start = now − elapsed`, so the sleep timer and the
  elapsed readout carry on. If it was stopping, treat it as title (the piece was ending anyway);
- if the core is already playing (a **Memories** load in the middle of a session), crossfade
  (`mode_begin(c, XFADE_MS)`) instead of a hard switch, like B during play.

**Tests:** `tb_sleep` (handshake, the snapshot does not tear while the state changes, bad blobs are
rejected, a wake while running). A `--resume` option in render / rvsim (`model/setscript.h`) that
checks it is bit-exact through the RTL. Unit tests for the sleep timer carrying on and a Memories load
while playing. Then `core.json` `"sleep_supported": true` and a version bump.

## To verify on hardware

1. What power does with `sleep_supported: false` today (full power-off? a message?). This answers
   spec open question 1.2, which is still Open in DECISIONS.
2. Is `ambient.sav` read at sleep and written back at wake? The blob carries the settings, so it
   works either way. If the APF restores its stale menu values after the load, they would count as a
   menu change (the D59 caveat).
3. Is there a click at sleep? The APF cuts the audio when it reads the blob. Holding `busy` for a
   short fade-out is possible, but a Memories save would then dip the music too, so only do it if a
   click is heard.
4. How long the title screen shows between wake and the load (the core wakes silent, so it is only visual).
5. Dev builds: the firmware slot loads again at wake. Check that the resume still works.
6. The Memories menu: save, then load during a different mode, which should crossfade.

The build needs Quartus (local, free). Per the project's working rules, ask before building or
installing to the SD card. Nothing here pushes to GitHub.
