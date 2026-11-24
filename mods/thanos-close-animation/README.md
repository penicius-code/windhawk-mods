# Thanos Close Animation

A closing window turns into dust that drifts away, starting at the bottom-right corner. A Windhawk mod.
Version 1.1.1 | Windhawk 1.7.3, Windows 11 (build 26100) | Status: installed on my PC, currently switched off; no written test record

## Whose code this is

This mod is built on [Windows Animations](https://windhawk.net/mods/windows-animations) 1.3.7 by ReDrag (MIT), which contains this effect among many others. I compared the two files range by range:

| Share of this file | What |
|---|---|
| about 13 %, nearly verbatim from Windows Animations | Particle formulas and their constants, the lists of window classes and helper processes to skip, the rounded-corner mask, a bitmap helper |
| about 16 %, adapted from it | Window capture (screen grab if nothing covers the window, otherwise `PrintWindow`), the test for which windows qualify, the size of the drift area |
| about 23 %, same approach, written anew | The set of hooked functions and the close-detection strategy, the re-posted close message, the "last window of the process" wait |
| about 47 %, mine | The parallel renderer, drawing only the changed rectangle, frame pacing to the display's vertical blank, the particle cap, the three settings, the fullscreen guard |

The shares are estimates, plus or minus 5 points. See [THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md).

## The problem

The dust effect existed only inside Windows Animations, a mod of 14,495 lines with many effects that is loaded into every process. This mod holds the close effect alone, with a renderer of its own.

## What I built

A standalone mod of 1,396 lines of C++ with only the close effect. It changes: the moment a window disappears when it is closed (the close is delayed by the animation time), inside every process the mod is loaded into. It writes no files and no registry values, and never ends a process.

## How it works

- It hooks six window functions (`DestroyWindow`, `ShowWindow`, `ShowWindowAsync`, `SetWindowPos`, `SetWindowPlacement`, `DefWindowProcW`) to notice a close, or a hide if the "hidden to the tray" setting is on.
- Before the window goes away it takes a picture of it, hides the real window, and shows the picture in an overlay window.
- The picture is cut into blocks of `particle_size` pixels. Each block gets a start time from its distance to the bottom-right corner plus some noise, and its own wind and curve.
- Drawing is split over up to 8 threads in bands, in two passes (place, then draw). Only the rectangle that changed since the last frame is handed to the compositor.
- Frames are timed to the next vertical blank reported by the compositor.
- If the closing window is the last visible window of its process, the closing thread is held for the animation time plus 0.7 seconds, so the process does not exit under the animation.
- For consoles, Windows Terminal, Task Manager and WinUI windows the close message itself is held back, the animation runs, and the message is sent again. If the program then refuses to close (an "are you sure?" prompt), the window is shown again.
- It does not load into session-0 processes or into browser helper processes.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `duration_ms` | 800 | 200 to 3000 |
| `particle_size` | 2 | 1 to 10 pixels. Raised automatically if more than 3,000,000 particles would result |
| `hide_as_close` | on | A window that its program hides (to the tray) is animated like a close |

## Not animated

Store and system apps, MMC consoles, owned windows and popups, windows smaller than 300 x 300, fullscreen windows.

## Install and remove

In Windhawk choose "Create a New Mod", paste `thanos-close-animation.wh.cpp` and compile. If Windows Animations or MacOS Minimize Animation is also installed, switch their close animation off. To remove it, switch the mod off or delete it in Windhawk.

## Tested

No test log and no recording exist. The fullscreen guard added in 1.1.1 was written after a code review and was not run with a game.

## Known issues

- With `hide_as_close` on, any qualifying window that its program merely hides gets the effect.
- At the particle cap the mod uses about 78 MB for one animation.
- The mod loads into every process (with an exclude list); the cost of its hooks per window message was not measured.

## Changelog

- 1.1.1 (2026-11-23): fullscreen windows are never animated.
- 1.1.0: the version before; no notes kept.

## Licence

MIT for my part, see [LICENSE](../../LICENSE). Parts from Windows Animations by ReDrag (MIT), see above.

Built with AI assistance (Claude); designed, tested and maintained by me.
