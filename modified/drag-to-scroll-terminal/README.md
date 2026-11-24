# drag-to-scroll-terminal

Modified copy of [Drag to Scroll](https://windhawk.net/mods/drag-to-scroll) 1.2.0 by [ArvindSaini978](https://github.com/ArvindSaini978). Ctrl + right-drag scrolling also works in Windows Terminal.
Version 1.2.0-terminal.2 | State on my PC: installed, currently switched off

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original states `@license MIT` in its header.

## Why

The original reads classic mouse messages. Windows Terminal receives its mouse input as pointer messages on a separate input window, so the original never sees the right button there.

## What I changed

- New `PointerTranslator`: converts pointer messages (`WM_POINTERDOWN`, `WM_POINTERUPDATE`, `WM_POINTERUP` and their non-client variants) from mouse and touchpad into the classic mouse messages and passes them to the original, unchanged input engine.
- The two `DispatchMessage` hooks of the original now send pointer messages through that translator first.
- Once pointer input has been seen, scroll output always goes through the original's own `SendInput` path instead of a posted wheel message.
- New watcher thread that polls the physical Ctrl key (every 8 ms during a gesture, every 60 ms otherwise) and ends the gesture when Ctrl is released. Once pointer input has been seen, the key-up message path of the original is no longer used for this.
- A replayed right-click tap is let through within a one-second window.
- The copy loads into `WindowsTerminal.exe` only. Use the original for every other program and exclude Windows Terminal there.

Size of the change: +375 / -79 lines against the original, in a file of 2,709 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

One extra thread inside Windows Terminal that polls a key state about 16 times a second when idle.

## Tested

Two input scripts drove a throw-away terminal window and compared screenshots during development. The stored screenshots do not by themselves show a scroll, and no run output was kept, so there is no usable test record.

## Use

Keep the original mod for all other programs and add `WindowsTerminal.exe` to its exclude list. Then create a new mod in Windhawk and paste `drag-to-scroll-terminal.wh.cpp`.

My changes were made with AI assistance (Claude).
