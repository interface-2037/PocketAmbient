# Hardware test: v0.4.3

Dev package (`AmbientPocket-0.4.3-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. Fixes in this build: core-menu settings are now saved
correctly at power-off (D57), and the locked seed is saved in a new save file, `ambient.sav` (D58).
The v0.4.2 fade-out fix (D56) is in it too, and it hasn't been tested on hardware yet (step 2).
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again: it caches core info across
   sleep. Then check that the core info shows version **0.4.3**.
   The first start of 0.4.3 may show Lock Seed **off** even though it was on in v0.4.1: there was
   no saved seed to lock, so the core now takes the seed from the clock instead of locking seed
   00000000. It may also start at Density 1 / Brightness 1, from the bad values v0.4.1 saved.
   Both are expected.
2. **v0.4.2 items, not yet tested (fade-out fix, D56):**
   1. **Mode change during the fade-out.** Let a mode play ~20 s, press A, and within the 3 s
      fade press Right once. The old mode keeps fading down with **no volume boost** and no jump
      in the echo or reverb. It ends in silence on the next mode's title screen. Do this from
      every mode: DRIFT, TIDES, HUSH, MOTHER. Try Left too, and two presses in one fade.
   2. **Change + A during the fade.** While a mode plays, press A, then Right and A straight away
      (all within 3 s). The new mode fades in with a smooth crossfade: no jump in level, echo or
      reverb. Do this from every mode.
   3. **B during the fade.** Press A, then B within the fade. The tail keeps fading down with no
      boost.
   4. **Y overlay after ~8 minutes of play, any mode:** UNDERRUN 0, FIFO MIN ≥ 500, FRAME
      1546–1547, CPU MAX in the 230k range, no red screen. Write down the values.
   5. **Listening check:** about 1 minute of each mode (DRIFT, TIDES, HUSH, MOTHER), on the
      speaker and on headphones. They should sound the same as v0.4.0.
   6. **Core menu:** change Mode and Density from the menu. Check that the music and the screen
      follow.
3. **Settings saved at power-off.** In the core menu set Mode = **Tides**, Density = **5**,
   Brightness = **2**, Reverb = **High**. Close the menu, then press **Down** once (Density 4 on
   the screen). Open the menu again: it should show Density 4. Write down what the menu and the
   screen show. Power off, power on, start the core. The menu and the screen show the **same
   values** as before power-off (Tides, Density 4, Brightness 2, Reverb High).
4. **Lock Seed.**
   1. Turn Lock Seed **on** in the menu. Press Y and note the SEED in the overlay: it must **not**
      be 00000000. Power-cycle and start the core. The overlay shows the **same seed**, the menu
      shows Lock Seed on, and the piece starts the same way.
   2. Turn Lock Seed **off**. Power-cycle twice, noting the SEED in the Y overlay each time. It
      **changes** each time.
5. **Afterwards, put the SD card back in the PC** so the saved files can be checked:
   `Settings/beingFar.AmbientPocket/Interact/_core/interact_persist.json` (the list values should
   match what you set) and `Saves/ambient/beingFar.AmbientPocket/ambient.sav` (32 bytes,
   starting with `AMBS`). If the .sav file is somewhere else under `Saves/`, note where.
