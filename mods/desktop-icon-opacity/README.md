# Desktop Icon Opacity

Semi-transparent desktop icons that return to full opacity under the mouse or when selected. A Windhawk mod.
Version 1.0.0 | Windhawk 1.7.3, Windows 11 (build 26100) | Status: installed and enabled on my PC; no written test record

## Whose code this is

The drawing logic is mine. The code that finds the desktop icon view and attaches to it (roughly a third of the code: `IsDesktopListView`, `ListViewUnder`, `GetExistingDesktopListView`, the subclass procedure, `AttachToListView`, the `CreateWindowExW` hook, and the matching set-up and clean-up lines) is taken from [Desktop Icon Selection Style](https://windhawk.net/mods/desktop-icon-selection-style) by RiteshK, MIT licence. See [THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md).

## The problem

Desktop icons sit on top of the wallpaper at full strength. Windows can hide them all, but it has no setting to make them fainter.

## What I built

A mod of 510 lines of C++ that fades the icon images on the desktop to a chosen percentage. Labels and the selection highlight are not changed. It changes: how icon images are drawn while the desktop icon view paints itself, inside `explorer.exe`. Nothing is written to disk or to the registry.

## How it works

- Windows draws desktop icons through image lists. The mod hooks the two drawing entry points, `ImageList_DrawIndirect` and the `Draw` method of the image-list interface, and adds an alpha value to a copy of the drawing parameters. A draw call that is already translucent is multiplied, not replaced.
- Explorer can have two versions of the common-controls library loaded under the same file name. The mod picks the right one (version 6) by looking for an export that only that version has.
- The address of the interface's `Draw` method is read from a temporary image list the mod creates for that purpose.
- Only the desktop is affected: a counter is raised while the desktop icon view handles a paint message, and the hooks do nothing outside of that. Menus, the taskbar and folder windows draw through the same functions and stay as they are.
- A per-thread flag stops the fade being applied twice when one hooked function calls the other.
- The icon under the mouse and selected icons are found with the list view's own hit test and item state.
- If the right library or any of the hooks cannot be set up, the mod does not load at all.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `iconOpacity` | 50 | Percent, 0 to 100. 100 changes nothing |
| `fullOpacityOnHover` | on | The icon under the mouse is drawn at full opacity |
| `fullOpacityOnSelect` | on | Selected icons are drawn at full opacity |

## Install and remove

In Windhawk choose "Create a New Mod", paste `desktop-icon-opacity.wh.cpp` and compile. To remove it, switch the mod off or delete it in Windhawk; the desktop repaints with normal icons.

## Tested

In use on my PC at 50 %. There is no test log and no screenshot. Not checked: Windows 10, and whether every icon overlay is faded together with its icon.

## Known issues

- If a Windows build draws desktop icons through a different path than the two hooked functions, nothing is faded.
- No Windows version check.

## Licence

MIT for my part, see [LICENSE](../../LICENSE). The attach code is by RiteshK (MIT), see above.

Built with AI assistance (Claude); designed, tested and maintained by me.
