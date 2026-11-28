# desktop-marquee-fade-gamemode

Modified copy of [Desktop Marquee Fade](https://windhawk.net/mods/desktop-marquee-fade) 1.0.0 by [angel_lov](https://github.com/Twilight0Sparkle). The system-wide mouse hook is replaced by a hook on the desktop's own thread.
Version 1.0.0-gamemode.2 | State on my PC: installed and enabled

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original states `@license MIT` in its header.

## Why

The original watches the left mouse button with a low-level mouse hook (`WH_MOUSE_LL`). With such a hook, every mouse event in every program waits for a round trip through `explorer.exe`.

## What I changed

- The low-level hook is removed. A thread-local `WH_MOUSE` hook is installed on the Explorer thread that owns the desktop icon view; it only runs for mouse messages sent to the desktop's own windows.
- A fade starts on button release only if the desktop still holds mouse capture. A selection cancelled with Esc no longer leaves a fading rectangle.
- New tracking of the desktop window: the hook follows it when it moves between `Progman` and `WorkerW`, when the icon view is re-created and after an Explorer restart.
- Removal of the hook waits until no call is in progress on the desktop thread.
- The fade animation itself (a 16 ms timer per overlay) is the original's.

Size of the change: +436 / -192 lines against the original, in a file of 869 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

While the desktop window is not found yet, two event hooks limited to the Explorer process are active. At unload the worker waits for calls in progress (at most 5 seconds).

## Tested

Tested, not extensively.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `desktop-marquee-fade-gamemode.wh.cpp`.

My changes were made with AI assistance (Claude).
