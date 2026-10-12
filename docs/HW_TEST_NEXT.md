# Hardware test: v0.4.4

Dev package (`AmbientPocket-0.4.4-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. The fix in this build: settings changed with the **buttons**
now survive a power cycle. At power-on the core takes its settings from `ambient.sav`, which is
saved from the live settings at every power-off (D59). No change to the music.
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.4.4**.
2. **Button changes persist (the v0.4.3 step-3 retest).** In the core menu set Density **5**.
   Close the menu and press **Down** once: the screen shows Density **4**. Also press **L** or
   **R** to change Brightness, and **Left** or **Right** to change Mode. Write down Mode, Density
   and Brightness as the screen shows them. Do **not** open the menu again. Power off, power on,
   start the core. Density comes back as **4**, and Mode and Brightness come back as you wrote
   them down. Open the menu: it shows the **same values** as the screen.
3. **A menu change still persists.** In the core menu set Reverb **High**. Power-cycle and start
   the core. The menu shows Reverb High.
4. **Lock Seed recheck.** Turn Lock Seed **on** in the menu and note the SEED in the Y overlay.
   Power-cycle and start the core. The overlay shows the **same seed**.
5. **Put the SD card back in the PC**, so `ambient.sav` and `interact_persist.json` can be checked.
