# microswap-gamemode

Modified copy of [MicSwitch](https://windhawk.net/mods/microswap) 2.1.0 by [BlackPaw](https://github.com/BlackPaw21). The mod stops receiving mouse input while a fullscreen window has focus.
Version 2.1.0-gamemode.1 | State on my PC: installed, currently switched off

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original states `@license MIT` in its header.

## Why

To notice the mouse wheel over its tray icon, the original registers for raw mouse input in the background and so receives every mouse packet of the system, also during a game.

## What I changed

- The raw-input registration is added and removed at run time. While a fullscreen window has focus on the monitor of the tray icon, it is removed.
- Fullscreen test: the window covers its monitor and is not a maximised window with a title bar, or Windows reports exclusive fullscreen, busy or presentation mode.
- New setting `fullscreenGameMode` (`pause`, the default, or `off`).
- Tray tooltip and position updates are postponed while paused.
- The input handler reads into a buffer on the stack instead of allocating one per message.

Size of the change: +248 / -37 lines against the original, in a file of 3,834 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

A system-wide foreground-change event hook and a one-second timer run permanently, in exchange for not receiving mouse packets while paused.

## Tested

Tested, not extensively; the pause was not measured in a game.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `microswap-gamemode.wh.cpp`.

My changes were made with AI assistance (Claude).
