# Hardware test: v0.5.0

Dev package (`AmbientPocket-0.5.0-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. New in this build: **sleep / wake** (D62). The power button
now puts the core to sleep. After waking, the same piece (mode, seed, settings) fades back in from
its start, and the elapsed time and the sleep timer carry on. No change to the music otherwise.
Note the step number of anything that fails, and anything you hear at sleep or wake.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.5.0**.
2. **Sleep while playing.** Start the core and press A. Set TIDES, Density **4**, Reverb **High**,
   and note the SEED in the Y overlay and the elapsed time. Wait a minute, then press the power
   button once (sleep). Listen: is there a click or pop as it goes to sleep?
3. **Wake.** Press power again. The core comes back on the title screen for a moment, then TIDES
   fades in over about 4 s, with no burst of noise. The Y overlay shows the **same seed**, the
   screen shows Density 4, and the elapsed time carries on from where it was (not 0:00). Open the
   menu: it shows TIDES / 4 / High. Note roughly how long the title screen showed.
4. **Sleep while stopped.** Press A to stop and wait for the fade-out. Sleep, then wake: the core
   stays on the title screen (no music) with the same settings. Press A: the same seed plays.
5. **Sleep timer carries on.** Set the Sleep Timer to **30 min**, press A, and after about 2
   minutes sleep for a minute and wake. Then leave it: the music fades out about 30 min after the
   first A, not 30 min after the wake. (Optional, it takes half an hour.)
6. **Memories.** In the Pocket menu, open **Memories** and save a state while TIDES plays. Switch
   to HUSH with Right, then load that memory: the music crossfades back to TIDES with the saved
   seed, with no gap or volume jump.
7. **Power-off still saves.** Change Density with the buttons, power off fully (not sleep; note how you did it), power
   on and start the core: the settings come back as before (the v0.4.4 behaviour).
8. **Put the SD card back in the PC**, so `System/user_sleepstate.sta` (it should hold `AMSS` after
   the header), `Saves/ambient/beingFar.AmbientPocket/ambient.sav` and `interact_persist.json`
   can be checked.
