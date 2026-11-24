# Genie Minimize Animation

A minimised window bends and pours into its own taskbar button. A Windhawk mod.
Version 1.0.1 | Windhawk 1.7.3, Windows 11 (build 26100) | Status: installed and enabled on my PC; no written test record

## Whose code this is

This is a standalone extraction of the Genie effect of [Windows Animations](https://windhawk.net/mods/windows-animations) 1.3.7 by ReDrag (MIT). The genie maths in that mod goes back to [MacOS Minimize Animation](https://windhawk.net/mods/macos-minimize-animation) by Abdullah Masood (MIT), which Windows Animations credits. I compared this file with Windows Animations range by range:

| Share of this file | What |
|---|---|
| about 22 %, nearly verbatim from Windows Animations | The genie rendering (`MorphAt`, `RenderGenie`), the canvas bounds, the fade at the end, taskbar-edge detection, the lists of window classes and helper processes to skip, the rounded-corner mask |
| about 22 %, adapted from it | Window capture, the taskbar-button lookup through UI Automation and its scoring, the overlay window |
| about 33 %, same approach, written anew | Where the animation ends on the taskbar, the worker threads, the hand-off protocol for taskbar clicks |
| about 23 %, mine | The animation job and its states, the frame loop, the cache of button positions with background refresh, the hook set, the unload sequence, the fullscreen guard |

The shares are estimates, plus or minus 5 points. See [THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md).

## The problem

The genie effect existed only inside Windows Animations, a mod of 14,495 lines with many effects that is loaded into every process. This mod holds the minimise effect alone.

## What I built

A standalone mod of 1,540 lines of C++ with one setting. It changes: what is shown while a window is minimised, inside every process the mod is loaded into. It writes no files and no registry values (it reads one: the taskbar alignment).

## How it works

- It hooks four window functions (`ShowWindow`, `ShowWindowAsync`, `CloseWindow`, `DefWindowProcW`) to notice a minimise.
- It takes a picture of the window, hides the real window, lets Windows minimise it without its own animation, and plays the genie in a click-through overlay window.
- The target is the window's real taskbar button, found through UI Automation on a separate thread. The first lookup may wait up to 0.4 seconds. A found position is reused for 3 seconds, and the last button position of a process is remembered per monitor. If no button is found, the animation ends at the left edge or the centre of the taskbar, depending on the taskbar alignment.
- A click on a taskbar button is minimised by Explorer, not by the program itself. For that case the mod posts a probe message to program windows; a window that forwards it gets a mark, and when Explorer later minimises a marked window, the request is passed to the window so that the animation runs in the program's own process. Windows that do not forward unknown messages are not animated on taskbar clicks.
- Restoring a window is left to Windows. Restoring while the genie still runs stops it at once.
- It does not load into session-0 processes or into browser helper processes.

## Setting

`duration_ms`, default 450, 150 to 1500.

## Install and remove

In Windhawk choose "Create a New Mod", paste `genie-minimize-animation.wh.cpp` and compile. If Windows Animations or MacOS Minimize Animation is also installed, switch their minimise animation off. To remove it, switch the mod off or delete it in Windhawk.

## Tested

In use on my PC. No test log and no recording exist. The fullscreen guard added in 1.0.1 was written after a code review and was not run with a game.

## Known issues

- Minimise only; there is no restore animation.
- The first minimise of a window can be delayed by up to 0.4 seconds for the button lookup and up to 0.5 seconds for the first frame.
- Minimising with Win+D or Win+M: the Windows-key check exists in one of the three minimise paths only, so "left to Windows" is not guaranteed for all programs.
- Exclusive fullscreen is recognised for the foreground window only.
- The mod loads into every process (with an exclude list); the cost of its hooks per window message was not measured.

## Changelog

- 1.0.1 (2026-11-23): fullscreen windows are never animated.
- 1.0.0: first version.

## Licence

MIT for my part, see [LICENSE](../../LICENSE). Parts from Windows Animations by ReDrag (MIT) and, through it, from MacOS Minimize Animation by Abdullah Masood (MIT), see above.

Built with AI assistance (Claude); designed, tested and maintained by me.
