# Hardware test: v0.4.5

Needs a Quartus build (D61 changes `settings_bridge.sv`), then the dev package
(`AmbientPocket-0.4.5-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. The fixes in this build: a menu change no longer undoes
settings changed with the buttons (D61, D62), and the MOTHER rework (D60).
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.4.5**.
2. **The reported bug.** Start the core in DRIFT. Press **Right** once (TIDES). In the core menu
   change **Brightness**. Close the menu: the mode is still **TIDES**.
3. **Other settings too.** Press **Up** (Density +1), **R** (Brightness +1) and **X** (Visuals).
   Write down what the screen shows. In the core menu change **Reverb** only. Close the menu:
   Mode, Density, Brightness and Visuals are unchanged.
4. **A menu change still applies.** In the core menu pick a **Mode** different from both the
   current one and the one you started in: the mode changes (6 s crossfade). Pick a different
   **Density**: it changes, and Brightness stays.
5. **Persistence still works (the v0.4.4 checks).** Change Density with the buttons, power off
   and on, start the core: the button value comes back, and the menu shows the same values as
   the screen. Lock Seed on, power-cycle: the Y overlay shows the same seed.
6. **MOTHER.** Listen for a few minutes: sine chirps at 1.5–2.2 kHz, a low rumbling hum, vent
   hiss and clunks, nothing harsh, about as loud as the other modes.
