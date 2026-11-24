# scroll-window-opacity-gamemode

Modified copy of [Scroll Window Opacity](https://windhawk.net/mods/scroll-window-opacity) 1.0.3 by [Sondre Myrmel](https://github.com/Sondre234). Fullscreen windows are never made transparent, and the mouse hook exists only while the modifier keys are held.
Version 1.0.3-gamemode.3 | State on my PC: installed and enabled

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original has no licence line. The Windhawk catalogue rule applies: "Mods which don't specify a license are submitted under the MIT license" ([catalogue README](https://github.com/ramensoftware/windhawk-mods#readme)).

## Why

The original keeps a low-level mouse hook installed all the time and would also change the opacity of a fullscreen game or video when the key combination and wheel are used there.

## What I changed

- A fullscreen window is left alone: the wheel event passes through unchanged. New setting `protectFullscreen` (on by default).
- The low-level mouse hook is installed when a required modifier key goes down and removed when the combination is released. To see those key presses the mod registers for raw keyboard input in the background; it looks only at the key code of the configured modifier keys and stores nothing.
- While a fullscreen window is in the foreground, both the keyboard registration and the mouse hook are removed.
- A window this mod made transparent is put back to full opacity when it goes fullscreen. New setting `restoreIfFullscreen` (on by default).
- Entries for windows that no longer exist are dropped from the mod's list once a second.
- If the helper window for keyboard input cannot be created, the mod falls back to the original's permanent hook.

Size of the change: +309 / -39 lines against the original, in a file of 754 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

A background raw-input registration for the keyboard, a system-wide foreground-change event hook and a one-second timer, in exchange for having no mouse hook most of the time.

## Tested

No written test record; the pause was not measured in a game.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `scroll-window-opacity-gamemode.wh.cpp`.

My changes were made with AI assistance (Claude).
