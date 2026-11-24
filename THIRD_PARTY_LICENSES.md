# Third-party code and licences

This repository contains code by other authors. All of it comes from mods published in the Windhawk catalogue (<https://github.com/ramensoftware/windhawk-mods>). None of the original files carries a copyright line or a licence text; they carry at most the tag `@license MIT` in their header. The catalogue's README states: "Mods which don't specify a license are submitted under the MIT license. It's the author's responsibility to specify the appropriate license for third-party code." (read on 2026-11-24).

## Code reused inside my own mods (`mods/`)

| Original | Author | Link | Licence | Where it is used here |
|---|---|---|---|---|
| Windows Animations 1.3.7 | ReDrag | <https://windhawk.net/mods/windows-animations> | `@license MIT` in the header | `mods/genie-minimize-animation/` (genie rendering maths, taskbar-edge and taskbar-button lookup, window capture, process and window-class filters) and `mods/thanos-close-animation/` (particle formulas and constants, close detection strategy, window capture, process and window-class filters) |
| MacOS Minimize Animation | Abdullah Masood | <https://windhawk.net/mods/macos-minimize-animation> | `@license MIT` in the header | The classic genie maths in Windows Animations comes from this mod (credited there), and so reaches `mods/genie-minimize-animation/` |
| Desktop Icon Selection Style 1.0.0 | RiteshK | <https://windhawk.net/mods/desktop-icon-selection-style> | `@license MIT` in the header | `mods/desktop-icon-opacity/`: the code that finds the desktop icon view and attaches to it |

## Modified copies (`modified/`)

| Original | Author | Link | Licence | Where it is used here |
|---|---|---|---|---|
| Drag to Scroll 1.2.0 | ArvindSaini978 | <https://windhawk.net/mods/drag-to-scroll> | `@license MIT` in the header | `modified/drag-to-scroll-terminal/` (whole file, with my changes) |
| Smooth Scroll for Win32 1.0.3 | crazyboyybs | <https://windhawk.net/mods/smooth-scroll-win32> | no licence line; MIT by the catalogue rule | `modified/smooth-scroll-win32-smoother/` (whole file, with my changes) |
| Taskbar Dock Animation 1.9.2 | Ph0en1x-dev | <https://windhawk.net/mods/taskbar-dock-animation> | no licence line; MIT by the catalogue rule | `modified/taskbar-dock-animation-keep-inside/` (whole file, with my changes) |
| Desktop Marquee Fade 1.0.0 | angel_lov | <https://windhawk.net/mods/desktop-marquee-fade> | `@license MIT` in the header | `modified/desktop-marquee-fade-gamemode/` (whole file, with my changes) |
| MicSwitch 2.1.0 | BlackPaw | <https://windhawk.net/mods/microswap> | `@license MIT` in the header | `modified/microswap-gamemode/` (whole file, with my changes) |
| Scroll Window Opacity 1.0.3 | Sondre Myrmel | <https://windhawk.net/mods/scroll-window-opacity> | no licence line; MIT by the catalogue rule | `modified/scroll-window-opacity-gamemode/` (whole file, with my changes) |
| Taskbar Fade 2.2.0 | Lukvbp | <https://windhawk.net/mods/taskbar-fade> | no licence line; MIT by the catalogue rule | `modified/taskbar-fade-gamemode/` (whole file, with my changes) |

## MIT License

The MIT licence text that applies to the code listed above, with the copyright belonging to the respective authors named in the tables:

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
