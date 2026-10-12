# Hardware test: v0.4.1

Dev package (`AmbientPocket-0.4.1-dev.zip`). Copy it to the SD card as usual (including
`Assets/ambient/common/composer.bin`, which is the v0.4.1 firmware). Nothing in the music changed,
so this is mostly a check of the fixes. Write down anything that fails, with the step number.

1. **Pocket menu art.** In the core list, the AmbientPocket entry shows the new icon (a small
   globe in a double frame), and the platform image shows the new picture (a globe, "AMBIENT",
   "GENERATIVE SOUND SYSTEM", four mode boxes) instead of the openFPGA placeholder. Note whether it
   is upright and not mirrored. Preview: `docs/ui/platform_art.png`.
2. **Version.** The core info shows version 0.4.1.
3. **HUSH title readouts (the reported bug).** Start in any mode and let it play ~20 s. Press A
   to stop and wait for the fade-out (~3 s). Then press Left / Right until HUSH shows. The big
   readout must show `---` BPM (not "11566" or any other number), `BAR --`, and no chord names.
   Repeat starting from DRIFT, TIDES and MOTHER.
4. **The other title screens.** Same procedure. Stopped, each screen shows `--` for its
   mode value: DRIFT `TENSION --` and `STRINGS -  HORN --  DRONE --`, TIDES `CHORD --`, MOTHER
   `ACTIVITY --` and `THERMAL --.-`. After A, the values come back within a few seconds.
5. **Mode change during the fade-out (fixed in the composer).** While a mode plays, press A, and
   within the 3 s fade press Right once. The old mode fades out cleanly (no burst or odd notes),
   and the next mode's title shows `--` readouts. Then, again during a fade-out, press Right and
   then A straight away: the new mode starts with a crossfade and plays normally. Try this from
   every mode, HUSH → TIDES included.
6. **Y overlay after ~8 minutes of play, any mode:** UNDERRUN 0, FIFO MIN ≥ 500, FRAME 1546–1547,
   CPU MAX in the 230k range (v0.4.0: 231,138), no red screen. Write down the values.
7. **Listening sanity check:** ~1 minute of each mode (DRIFT, TIDES, HUSH, MOTHER). They should sound
   exactly like v0.4.0. Try the speaker and headphones.
8. **Core menu:** change Mode and Density from the menu, and check that the screen follows.
