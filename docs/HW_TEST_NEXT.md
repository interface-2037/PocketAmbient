# Hardware test: v0.4.2

Dev package (`AmbientPocket-0.4.2-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`, the v0.4.2 firmware. The only change is the fade-out fix (D56).
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again: it caches core info across
   sleep. Then check that the core info shows version **0.4.2**.
2. **Mode change during the fade-out (the reported bug).** Let a mode play ~20 s, press A, and
   within the 3 s fade press Right once. The old mode keeps fading down with **no volume boost**
   and no jump in the echo or reverb. It ends in silence on the next mode's title screen. Do this
   from every mode: DRIFT, TIDES, HUSH, MOTHER. Try Left too, and two presses in one fade.
3. **Change + A during the fade.** While a mode plays, press A, then Right and A straight away (all
   within 3 s). The new mode fades in with a smooth crossfade: no jump in level, echo or reverb.
   Do this from every mode.
4. **B during the fade.** Press A, then B within the fade. The tail keeps fading down with no boost.
5. **Y overlay after ~8 minutes of play, any mode:** UNDERRUN 0, FIFO MIN ≥ 500, FRAME 1546–1547,
   CPU MAX in the 230k range, no red screen. Write down the values.
6. **Listening check:** about 1 minute of each mode (DRIFT, TIDES, HUSH, MOTHER), on the speaker
   and on headphones. They should sound the same as v0.4.0.
7. **Core menu:** change Mode and Density from the menu. Check that the music and the screen follow.
