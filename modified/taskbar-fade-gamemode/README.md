# taskbar-fade-gamemode

Modified copy of [Taskbar Fade](https://windhawk.net/mods/taskbar-fade) 2.2.0 by [Lukvbp](https://github.com/lukvbp). The polling loop stops while a fullscreen window has focus.
Version 2.2.0-gamemode.1 | State on my PC: installed and enabled

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original has no licence line. The Windhawk catalogue rule applies: "Mods which don't specify a license are submitted under the MIT license" ([catalogue README](https://github.com/ramensoftware/windhawk-mods#readme)).

## Why

The original checks the cursor and idle time about 15 times a second (every 66 ms), also while a fullscreen window hides the taskbar.

## What I changed

- The worker loop waits on an event instead of sleeping, with the same intervals as before. While paused it waits without a timeout.
- It pauses only when the fullscreen window is on the same monitor as every visible taskbar.
- On resume the fade continues from the current transparency.
- New setting `FullscreenGameMode` (`pause`, the default, or `off`).
- Fullscreen test as in `microswap-gamemode`.

Size of the change: +245 / -9 lines against the original, in a file of 1,000 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

A new thread with a system-wide foreground-change event hook and a one-second timer, in exchange for the stopped 15-per-second loop.

## Tested

No written test record; the pause was not measured in a game.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `taskbar-fade-gamemode.wh.cpp`.

My changes were made with AI assistance (Claude).
