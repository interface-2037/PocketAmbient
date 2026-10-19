# Hardware test: v0.5.1

Dev package (`AmbientPocket-0.5.1-dev.zip`). Copy it to the SD card as usual, including
`Assets/ambient/common/composer.bin`. New in this build:

- **Pocket lock (D63).** Hold **Select + Start** for 2 seconds (or use **Pocket Lock** in the
  core menu): the buttons are ignored while the music keeps playing. A lock screen shows for
  3 seconds, then the player screen returns. Hold Select + Start for 2 seconds again to unlock.
  Start now acts when it is **released**.
- **D61** (not yet tested on hardware): a menu change after a button mode change no longer
  reverts the mode.

This build branches from main and does **not** include the sleep / wake feature of the other
v0.5.0 build (branch `claude/sleep-mode-implementation-b5d45b`, D62). No change to the music. Note the step number of anything that fails.

1. **Power-cycle first.** Turn the Pocket fully off and on again, then check that the core info
   shows version **0.5.1**.
2. **Start still plays and stops.** Press and release **Start**: the music fades in when you
   **release** it. Press and release Start again: it fades out. **A** still acts as soon as it is
   pressed.
3. **Lock with the buttons.** Start the music. Hold **Select**, then **Start**, and keep both down.
   After about 2 seconds the **lock screen** appears (padlock, "LOCKED", "HOLD SELECT + START FOR
   2 SECONDS TO UNLOCK", the mode name and elapsed time). The music **does not stop**, also when
   you let go. About 3 seconds later the **player screen** comes back (still locked).
4. **Locked buttons do nothing.** Press every button once: D-pad, A, B, X, Y, L, R, Start,
   Select. The music does not change (no mode change, no stop, no reseed). The first press brings
   the lock screen back for about 3 seconds, then the player screen returns. The player screen
   keeps updating (elapsed time) while locked.
5. **Unlock.** Hold Select + Start: the lock screen shows while you hold, and after about
   2 seconds the player screen comes back. The music is still playing (letting go of Start must **not** stop it). Press Right: the mode
   changes as usual.
6. **A short chord does nothing.** Press Select + Start together for under a second and let go:
   no lock, and the music does not stop.
7. **Lock from the menu.** Open the core menu and choose **Pocket Lock**, then close the menu.
   The lock screen or the player screen shows; the music keeps playing; the buttons are
   ignored. Unlock with Select + Start (2 s). Note whether pressing A on Pocket Lock in the menu
   also started or stopped the music (it tells us whether menu presses reach the core).
8. **Pocket test.** Lock it, put the Pocket in a pocket and walk around for a few minutes. The
   music should play on undisturbed. Note whether the Home button got pressed (that opens the
   Pocket's own menu, which the core cannot lock).
9. **Power cycle unlocks.** Lock, power off, power on, start the core: it is **not** locked.
10. **D61 recheck.** Change the mode with **Right**, then open the menu and change Brightness.
    The mode stays the one you picked with Right.
