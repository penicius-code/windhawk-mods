# Middle click to close tray apps

Middle-click an icon in the notification area to close the program it belongs to. A Windhawk mod.
Version 1.0.1 | Windhawk 1.7.3, Windows 11 (build 26100), 64-bit | Status: installed and enabled on my PC; tested, not extensively (see Tested)

Written from scratch. No code from other mods.

## The problem

Programs that live in the notification area have no common way to quit: each has its own right-click menu, and many only hide when their window is closed. For taskbar buttons there is a mod for this ("Middle click to close on the taskbar" by m417z), but it does not cover the small icons next to the clock.

## What I built

A mod of 481 lines of C++ that closes the owning program when you middle-click its tray icon, including icons inside the overflow flyout. It changes: how `explorer.exe` handles a middle click on a tray icon. It sends a close request to another program and, depending on the setting, ends that program's process.

## How it works

- Explorer tells a tray program about a click by sending the program its own callback message. The mod hooks the two functions Explorer uses for that, `SendNotifyMessageW` and `PostMessageW`, inside `explorer.exe`. No symbol downloads and no taskbar internals are needed.
- A message is treated as a tray middle click when its number is `WM_USER` or higher, the low word of `lParam` is a middle-button event, the target window belongs to another process, and the cursor is over the taskbar or the overflow flyout.
- Button-down and double-click are swallowed. The button release starts the action on a worker thread, so Explorer is not held up.
- Default method: send `WM_CLOSE` to the icon's owner window and to the program's visible top-level windows, wait 3 seconds, and end the process if it is still running. If the program opened a new visible window in the meantime (a "Save changes?" prompt, for example), it is left alone.
- After a process was ended, its leftover icon is removed from the tray.
- The exclusion list is checked before every close path. Explorer's own icons (volume, network, clock) are skipped, because their target window belongs to Explorer itself.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `closeMethod` | `closeThenEndTask` | `closeThenEndTask`, `close` (close request only) or `endTask` (end the process at once) |
| `endTaskTimeout` | 3000 | Milliseconds to wait after the close request |
| `ctrlEndsTask` | on | Ctrl + middle click ends the process at once |
| `excludedPrograms` | empty | Program file names that are never touched |
| `debugLog` | off | Appends to `%TEMP%\tray-middle-click-close.log` |

## Install and remove

In Windhawk choose "Create a New Mod", paste `tray-middle-click-close.wh.cpp` and compile. To remove it, switch the mod off or delete it in Windhawk. Nothing else is installed.

## Tested

- `test/traytest.cpp` is a small tray program written for this: it adds two icons (one in each notification-icon protocol version), logs every callback with the way it was delivered, and can ignore a close request, quit on it, or show a confirmation box. Its build command was not recorded.
- `test/click.py` sends a mouse click at a screen position with `SendInput`, optionally with Ctrl held.
- What the kept logs show: before the mod was installed, clicks arrive at the test program as "notify" deliveries (that is what told me which function to hook). With the mod installed, the test program received two posted `WM_CLOSE` messages on its owner window, which is what the mod's close path produces.
- What they do not show: the middle click that caused those messages, the end-task step, the Ctrl path, the exclusion list, the overflow flyout, or a program running as administrator. Those paths are in the code and the mod is in use on my PC, but none of them is recorded.

## Known issues

- Programs running as administrator cannot be closed this way; Windows refuses the request.
- The icon's own middle-click action, if the program has one, is replaced.
- The debug log has no size limit while `debugLog` is on.
- 64-bit only. The Windows 10 name of the overflow flyout is in the code, but the mod was not tried on Windows 10.

## Engineering notes

**Problem.** Find a way to act on tray clicks without depending on the taskbar's internal code, which changes between Windows builds.

**Diagnosis.** I wrote the test tray program first and let it log how each click reached it. Every click arrived through Explorer's notify call with the mouse event in the low word of `lParam`. So the click can be recognised at the moment Explorer sends it, with two documented functions, and the taskbar's own code does not need to be touched.

**Fix.** Hook those two functions in Explorer, filter strictly (message range, event type, other process, cursor over the tray), and do the closing on a separate thread that keeps the mod loaded until it has finished.

**A detail that cost a reinstall.** Windows does not allow a loaded DLL to be overwritten, so installing a rebuilt mod under the same version number failed. Version 1.0.1 is the same code as 1.0.0 under a new number.

**Rollback.** Switch the mod off in Windhawk.

## Licence

MIT, see [LICENSE](../../LICENSE).

Built with AI assistance (Claude); designed, tested and maintained by me.
