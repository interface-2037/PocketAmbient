# Hardware test: v0.5.2

Firmware-only update on top of v0.5.1 (same bitstream, Quartus seed 2). Only
`Assets/ambient/common/composer.bin` and `Cores/beingFar.AmbientPocket/core.json` change.
The fix (D64): a menu change no longer undoes a mode, density, brightness or visuals change
made with the buttons. The Pocket lock (D63) passed on hardware in v0.5.1.
This build branches from main and does **not** include the sleep / wake feature of the
v0.5.0 build (branch `claude/sleep-mode-implementation-b5d45b`, D62). No change to the music.
Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.5.2**.
2. **The v0.5.1 failure.** Start the music. Press **Right** to change the mode. Open the core menu
   and change **Brightness**. Close the menu: the mode is still the one you picked with Right,
   and the brightness is the menu's.
3. **Other button settings.** Press **Up** (density), **X** (visuals) and **L** (brightness). Open
   the menu and change **Reverb**. Density, visuals and brightness keep the button values.
4. **Menu changes still work.** In the menu pick a **different** Mode from the one playing: it
   switches. Change Density in the menu: it applies.
5. **Known limit (just note what happens).** Press Right to change the mode, then in the menu
   pick the mode you had **before** pressing Right. Expected for now: nothing happens (D61 /
   D64 limit); use the buttons instead.
6. **Lock quick check.** Hold Select + Start 2 s: lock screen, then the player screen; buttons
   ignored; hold Select + Start 2 s to unlock.
