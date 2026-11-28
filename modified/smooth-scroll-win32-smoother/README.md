# smooth-scroll-win32-smoother

Modified copy of [Smooth Scroll for Win32](https://windhawk.net/mods/smooth-scroll-win32) 1.0.3 by [crazyboyybs](https://github.com/crazyboyybs). Scroll animation frames are timed to the display refresh.
Version 1.0.3-smoother.1 | State on my PC: installed and enabled

> This is not the original mod and not an official update of it. Everything except the changes listed below is the original author's work. The original has no licence line. The Windhawk catalogue rule applies: "Mods which don't specify a license are submitted under the MIT license" ([catalogue README](https://github.com/ramensoftware/windhawk-mods#readme)).

## Why

The original animates with `SetTimer`, which Windows limits to about 64 ticks a second at uneven intervals of 15 or 16 ms.

## What I changed

- The timer is replaced by one frame thread that waits for the compositor (`DwmFlush`) and posts a frame message to every window that is animating.
- Each step uses the real time since the last frame (measured with `QueryPerformanceCounter`, capped at 50 ms). The spring runs in fixed 2 ms sub-steps, so the motion is the same at any frame rate.
- Animated windows are subclassed through Windhawk's helper and released again on unload; the thread is stopped before state is freed.
- Behaviour change: a window owned by a different thread than the caller is no longer smoothed.
- Removed with the timer: the per-monitor refresh-rate interval logic. The Portuguese and Spanish header texts of the original were dropped.

Size of the change: +206 / -69 lines against the original, in a file of 1,268 lines. In this repository's copy the `@author` line names the original author and my change; the first lines of the readme block say that it is a modified copy.

## Cost of the change

One worker thread per process the mod is loaded into; it blocks in `DwmFlush` while something animates.

## Tested

Tested, not extensively.

## Use

Switch the original mod off, then create a new mod in Windhawk and paste `smooth-scroll-win32-smoother.wh.cpp`.

My changes were made with AI assistance (Claude).
