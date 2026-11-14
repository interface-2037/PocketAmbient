# Hardware test: v0.6.1

Release package (`dist/AmbientPocket-0.6.1.zip`, also laid out in `dist/sdcard`). The firmware is
baked into the bitstream, so a `composer.bin` left on the card from a dev build is not used.
Back up `Cores/interface2037.AmbientPocket` on the card first, then copy the package over it.

New in this build:
- **D66** (firmware, first time in a bitstream): A or Start during the sleep timer's fade-out
  cancels the fade; the screens draw from a per-frame copy of the mode state.
- **D67** (RTL): the audio mutes with a short fade if the CPU stalls or faults, instead of holding
  the last sample; the boot seed mixes a cycle counter into the RTC.
- `composer.hex` rebuilt with clang 23.1 (the merged one came from clang 18.1).

Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on, start the core, and check that the
   core info shows version **0.6.1**.
2. **Sleep fade cancel (D66).** Set **Sleep timer** to 30 min in the menu, start a mode and let
   it run until the timer starts its fade-out (30 min, then a 30 s fade). Press **A** (or Start)
   a few seconds into the fade: the music comes back to full level and keeps playing; it does
   not stop at the end of the fade.
3. **Screens (D66).** Switch modes with Right / Left a few times while playing, and let one piece
   end: the title and player screens never show garbage (wrong BPM or numbers for one frame).
4. **Different seed each boot (D67).** With **Lock seed** off, start the core, press **Y** and
   note the seed. Quit the core (or power-cycle), start it again: the seed is different. Do it a
   third time.
5. **Lock seed still works.** Turn Lock seed on, note the seed, power-cycle: the same seed.
6. **Audio keeps playing through the APF menu (D67).** While playing, open the Analogue menu,
   wait 10 s, close it: the music is still playing (it must not have muted).
7. **Sleep / wake (D63 + D67).** Play TIDES, note the seed and elapsed time, press the power
   button to sleep, wake: TIDES fades back in on the same seed and the elapsed time carries on.
8. **Quick regression.** Pocket lock (hold Select + Start 2 s, buttons ignored, hold again to
   unlock), one Brightness change in the menu (mode stays), and a few minutes of each mode
   sounding as before.

The mute itself cannot be triggered on purpose on hardware (it needs a stalled or crashed CPU);
`sim/tb/tb_mute.sv` covers it. If a red screen ever shows (CPU fault), the audio should now be
silent instead of a held tone or a DC offset.
