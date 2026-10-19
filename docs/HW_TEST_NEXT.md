# Hardware test: v0.6.0 (combined)

Dev package (`AmbientPocket-0.6.0-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. This build combines two builds that each passed on their
own: sleep / wake (D63, v0.5.0 sleep-only build) and the Pocket lock with the menu fix
(D64 + D62, v0.5.1 / v0.5.2). New together: the MOTHER rework (D60) on hardware with both, and
the lock's register moved to control word 32 (the sleep work uses 30 and 31).
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.6.0**.
2. **Pocket lock.** Start the music (Start acts on **release**). Hold **Select + Start** for 2 s:
   the lock screen shows for 3 s, then the player screen; the music plays on. Press a few buttons:
   nothing changes, the lock screen comes back for 3 s. Hold Select + Start 2 s: unlocked, the
   music still playing. Lock once more from the core menu (**Pocket Lock**), then unlock.
3. **Menu fix (quick recheck).** Press **Right**, then change **Brightness** in the menu: the mode
   stays. Pick a different Mode in the menu: it switches.
4. **Persistence (the v0.4.4 checks).** Change Density with the buttons, power off and on, start
   the core: the button value comes back, and the menu shows the same values as the screen.
   Lock Seed on, power-cycle: the Y overlay shows the same seed.
5. **MOTHER.** Listen for a few minutes: sine chirps at 1.5–2.2 kHz, a low rumbling hum, vent
   hiss and clunks, nothing harsh, about as loud as the other modes.
6. **Sleep / wake recheck.** Play TIDES, note the seed (Y overlay) and the elapsed time, sleep,
   wake: TIDES fades in on the same seed, the elapsed time carries on, and the menu shows the
   same settings as before the sleep.
7. **Buttons, then sleep.** Press **Up** and **Right**, then sleep and wake: the button values
   come back, and a later menu change of **Reverb** does not undo them.
8. **Lock, then sleep.** Lock (Select + Start 2 s), then press the power button to sleep, then
   wake. The music resumes and the core is **unlocked** (the lock is not part of the save
   state); the buttons work.
9. **Memories.** Save a memory while playing, switch mode with Right, load it: a crossfade back.
