# windhawk-mods

Windows customisation mods for [Windhawk](https://windhawk.net/): four mods of my own, and seven modified copies of other authors' mods, kept apart and labelled as such.
Windhawk 1.7.3 | Windows 11 (build 26100) | Status: per mod, see the tables

> Not affiliated with Windhawk or with the authors of the original mods. Windhawk mods are C++ source files that Windhawk compiles and loads into Windows programs; this repository contains source only, no compiled files.

## What is here, and whose work it is

The two folders are separated on purpose, so that my own work and my edits of other people's work are never mixed.

### `mods/`: my own mods

| Mod | What it does | Origin of the code | State on my PC |
|---|---|---|---|
| [tray-middle-click-close](mods/tray-middle-click-close/) 1.0.1 | Middle-click a tray icon to close its program: a normal close request first, end task if it keeps running | Written from scratch | enabled |
| [desktop-icon-opacity](mods/desktop-icon-opacity/) 1.0.0 | Semi-transparent desktop icons, full opacity on hover or selection | Drawing logic is mine. The code that finds and attaches to the desktop icon view (roughly a third of the code) is taken from *Desktop Icon Selection Style* by RiteshK (MIT) | enabled |
| [thanos-close-animation](mods/thanos-close-animation/) 1.1.1 | A closing window turns into dust that drifts away | About 29 % of the lines are copied or adapted from *Windows Animations* by ReDrag (MIT): the particle formulas, close detection and window capture. The parallel renderer, frame pacing, settings and fullscreen guard are mine | installed, currently switched off |
| [genie-minimize-animation](mods/genie-minimize-animation/) 1.0.1 | A minimised window pours into its taskbar button | A standalone extraction of the Genie effect of *Windows Animations* by ReDrag (MIT), whose genie maths goes back to *MacOS Minimize Animation* by Abdullah Masood (MIT). About 44 % of the lines are copied or adapted from it. The job handling, button-position cache, hand-off for taskbar clicks and fullscreen guard are mine | enabled |

The percentages come from sorting every line range of the two animation mods into "nearly verbatim", "adapted", "same idea, rewritten" and "own", by comparing them with the original file. They are estimates, plus or minus 5 points.

### `modified/`: modified copies of other authors' mods

Each of these is another author's mod with a specific change of mine. The original author stays in the header; my change is named in the `@author` line and in each folder's README. Details and the list: [modified/README.md](modified/README.md).

| Modified copy | Original mod and author | My change |
|---|---|---|
| [drag-to-scroll-terminal](modified/drag-to-scroll-terminal/) | Drag to Scroll 1.2.0, ArvindSaini978 | Makes Ctrl + right-drag scrolling work in Windows Terminal |
| [smooth-scroll-win32-smoother](modified/smooth-scroll-win32-smoother/) | Smooth Scroll for Win32 1.0.3, crazyboyybs | Frames timed to the display refresh instead of a timer |
| [taskbar-dock-animation-keep-inside](modified/taskbar-dock-animation-keep-inside/) | Taskbar Dock Animation 1.9.2, Ph0en1x-dev | Magnified icons are no longer cut off at the taskbar edge |
| [desktop-marquee-fade-gamemode](modified/desktop-marquee-fade-gamemode/) | Desktop Marquee Fade 1.0.0, angel_lov | System-wide mouse hook replaced by one on the desktop's own thread |
| [microswap-gamemode](modified/microswap-gamemode/) | MicSwitch 2.1.0, BlackPaw | Stops receiving mouse input while a fullscreen window has focus |
| [scroll-window-opacity-gamemode](modified/scroll-window-opacity-gamemode/) | Scroll Window Opacity 1.0.3, Sondre Myrmel | Leaves fullscreen windows alone; mouse hook only while the modifier keys are held |
| [taskbar-fade-gamemode](modified/taskbar-fade-gamemode/) | Taskbar Fade 2.2.0, Lukvbp | Stops polling while a fullscreen window has focus |

## The problem behind the "gamemode" copies

Several mods I used did constant background work: a system-wide mouse hook, a raw-input listener, a loop polling 15 times a second. That work also ran while a fullscreen game had focus, where none of these mods has anything to do, and the mods had no option to stop it.

## What I built

For each of those mods, the smallest change that stops the background work when it cannot be useful: remove the hook or listener while a fullscreen window is in the foreground, or replace a system-wide hook with one that only sees the relevant window. Three of the four use the same fullscreen test (it is also in my two animation mods): the window covers its whole monitor and is not a maximised window with a title bar, or Windows reports exclusive fullscreen, busy or presentation mode.

Those three pay for the pause with a foreground-change event hook and a one-second timer. That trade is stated in each folder's README. The fourth (the desktop marquee mod) needs no fullscreen test, because its new hook never sees input outside the desktop.

## Install

A mod from outside the Windhawk catalogue is installed by pasting its source.

1. Install Windhawk from <https://windhawk.net/>.
2. In Windhawk choose "Create a New Mod", replace the template with the content of the `.wh.cpp` file, then "Compile Mod" and "Exit Editing Mode".
3. For a modified copy: switch the original mod off first. Both would act on the same windows.

## Uninstall / roll back

In Windhawk, switch the mod off or remove it. Nothing else is installed. My four mods write no files and no registry values, except `tray-middle-click-close` with its debug setting on (one log file in `%TEMP%`).

## Tested

Stated per mod in its README. In short: the mods marked "enabled" above are installed and enabled on my PC; none of them has a written test record, and the fullscreen pause was not measured in a game. `tools/check_headers.py` runs in CI and only checks that every mod file has a complete header, that id, folder and file name agree, and that every mod is listed in this README.

## Engineering notes

**Symptom.** With the shell mods loaded, `explorer.exe` and the Windhawk tool processes did background work at all times, whether or not a game was running.

**Diagnosis.** By reading each mod's source for hooks, timers and listeners rather than by guessing: a `WH_MOUSE_LL` hook in the desktop marquee mod (every mouse event in every program takes a round trip through `explorer.exe`), a raw-input sink in the microphone switcher (it receives every mouse packet), a 66 ms polling loop in the taskbar fade mod, and a permanent low-level mouse hook in the opacity mod.

**Fix.** One change per mod, described in each README, with the cost of the change written next to it.

**What I did not prove.** My notes hold process CPU counters taken with no game running, but no frame-time comparison in a game with and without these changes. The READMEs therefore describe what the code no longer does, not a frame-rate gain.

**Rollback.** Switch the modified copy off and the original back on in Windhawk.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Credits and licence

My own code is MIT, see [LICENSE](LICENSE). The original mods, their authors and their licences are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). If you are the author of one of the originals and want a copy removed or the credit worded differently, open an issue.

Built with AI assistance (Claude); designed, tested and maintained by me.
