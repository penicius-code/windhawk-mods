# taskbar-dock-animation-keep-inside

Modified copy of [Taskbar Dock Animation](https://windhawk.net/mods/taskbar-dock-animation) 1.9.2 by [Ph0en1x-dev](https://github.com/Ph0en1x-dev). Magnified and bouncing icons are no longer cut off at the top edge of the taskbar.
Version 1.9.2-keepinside.1 | State on my PC: installed and enabled (on my PC it still carries the original id and the version number 1.9.3; id, name and version were changed for this repository so that it cannot be mistaken for a release of the original)

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original has no licence line. The Windhawk catalogue rule applies: "Mods which don't specify a license are submitted under the MIT license" ([catalogue README](https://github.com/ramensoftware/windhawk-mods#readme)).

## Why

Windows clips everything the taskbar draws to the taskbar's own height, so an icon that grows upward loses its top. The original lists this under known issues.

## What I changed

- New setting "Keep icons inside the taskbar" (`KeepIconsInsideTaskbar`, on by default).
- New geometry code that finds the visible area of the taskbar and each icon's content box.
- Magnified and bouncing icons are shifted downward (`FitShift`). If that is not enough, the magnification is limited to what fits (`MaxFitScale`).
- With the setting off, the original code path runs unchanged.

Size of the change: +261 / -5 lines against the original, in a file of 1,743 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

None beyond the extra geometry calculation while icons animate.

## Tested

Tested.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `taskbar-dock-animation-keep-inside.wh.cpp`.

My changes were made with AI assistance (Claude).
