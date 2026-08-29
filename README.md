# PhantomFence

A tiny Windows system-tray app that keeps windows and the mouse cursor off "phantom" displays — displays that Windows thinks exist (e.g. one created by an AV receiver's HDMI connection) but that have no real screen attached. The display stays enabled, so audio to the device keeps working; PhantomFence just makes sure nothing ever lands on it.

Single static exe (~220 KB), no installer, no runtime dependencies, a few MB of RAM. Config is stored under `HKCU\Software\PhantomFence`.

## Usage

1. Run `PhantomFence.exe`. A monitor-with-a-red-slash icon appears in the tray.
2. Right-click the icon and click the display you want to fence (for your setup, **Display 1** — the portrait 1080×1920 one from the AVR). Displays are listed with number, resolution, and the monitor's friendly name to make the phantom easy to identify.
3. That's it. Optional toggles in the same menu:
   - **Rescue stray windows now** — one-shot sweep (also triggered by double-clicking the tray icon).
   - **Auto-rescue windows** (default on) — any window that appears on or ends up on a fenced display is instantly moved to the nearest real monitor. A background sweep every 2 s catches anything the event hooks miss.
   - **Block mouse on fenced displays** (default on) — a low-level mouse hook deflects the cursor along the fence boundary, so it can never get lost on the phantom display. Diagonal movement slides along the edge rather than stopping dead.
   - **Win+Shift+Arrow skips fenced displays** (default on) — when the next display in the direction you pressed is fenced, PhantomFence intercepts the hotkey and moves the window directly to the next *real* display in that direction, so the window never touches the phantom at all. When the destination is a real display, the keystroke passes through and Windows handles it natively.
   - **Keep taskbar auto-hide enabled** (default off) — display topology changes are notorious for silently turning off "Automatically hide the taskbar". With this on, PhantomFence re-enables auto-hide within ~2 seconds whenever Windows drops it. Note: while enabled, turning auto-hide off in Windows Settings will be reverted — uncheck this first if you ever want the taskbar permanently visible.
   - **Start with Windows** — registers the exe in the per-user Run key (no admin needed). Move the exe to a permanent location (e.g. `C:\Tools\`) *before* enabling this, since the registry entry points at the exe's current path.

## Behavior details

- Fenced displays are remembered by their **monitor device-interface ID**, not by display number, so the fence survives reboots, driver updates, and Windows renumbering displays.
- **Smart resize on rescue:** a window that was "fitted" to the phantom display (fills ≥60% of it in either axis — the usual result of landing there) is rescaled per-axis so it occupies the same fraction of the destination display, converting a portrait-fitted window into a landscape-fitted one. Small dialogs keep their size. Position maps proportionally, and everything is clamped into the destination's work area. The same remapping is used for intercepted Win+Shift+Arrow moves.
- **Snap detection:** a snapped window (halves, thirds, quarters — detected by its visible bounds sitting on exact grid fractions of the work area) is re-snapped to the same grid position on the destination display, pixel-exact, instead of being proportionally scaled.
- Maximized windows found on the phantom display are re-anchored and re-maximized on the target monitor.
- Window moves that can't be intercepted (an app's own "move to display" menu, third-party tools, Alt+Space keyboard moves) still land on the phantom briefly and are caught by auto-rescue.
- Windows being actively dragged by the user are never touched (the sweep skips windows in a move/size loop).
- **The primary display can never be fenced** — the taskbar and the tray icon live there, and fencing it would lock you out of the menu that could undo it. It shows greyed-out in the menu, and if you make a currently-fenced display the primary in Windows Settings, it is automatically (and permanently) unfenced, with a notification.
- **Fencing requires countdown confirmation.** A new fence takes effect immediately, but a "Keep fenced / Undo now" dialog appears on the nearest real display, and the fence auto-reverts after 15 seconds unless you confirm — the same pattern as Windows' "Keep these display settings?" prompt. If you accidentally fence the only display you can see, the dialog lands somewhere invisible and everything reverts on its own. Nothing is saved until you confirm, so even a reboot mid-countdown comes back unfenced.
- Display hot-plug / resolution changes (`WM_DISPLAYCHANGE`) re-evaluate everything immediately.
- Only one instance runs at a time; if Explorer restarts, the tray icon re-registers itself.

## Notes

- The exe is unsigned, so SmartScreen may warn on first run ("More info" → "Run anyway"). If downloaded, you may also need to right-click → Properties → **Unblock**.
- Exiting the app removes the hooks and the tray icon; nothing keeps running.
- To fully remove: exit the app, delete the exe, delete `HKCU\Software\PhantomFence`, and (if you enabled autostart) the `PhantomFence` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## Building from source

Cross-compile from Linux with MinGW-w64 (`apt install g++-mingw-w64-x86-64`):

```
make
```

or manually:

```
x86_64-w64-mingw32-windres -I src src/PhantomFence.rc src/phantomfence_res.o
x86_64-w64-mingw32-g++ -Os -municode -mwindows -static \
    src/PhantomFence.cpp src/phantomfence_res.o \
    -o PhantomFence.exe -lgdi32 -ldwmapi -lshell32 -luser32 -ladvapi32
x86_64-w64-mingw32-strip -s PhantomFence.exe
```

Or on Windows with Visual Studio (Developer Command Prompt), from `src\`:

```
rc PhantomFence.rc
cl /O1 /W4 /DUNICODE /D_UNICODE PhantomFence.cpp PhantomFence.res /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib
```

Everything is in `src/PhantomFence.cpp` plus a small resource script (icon + version info). No manifest required — DPI awareness is set programmatically to Per-Monitor V2.

## License

Phantom Fence is free software, released under the **GNU General Public License, version 3 or (at your option) any later version** — see [LICENSE](LICENSE). Free as in freedom: you may run, study, modify, and redistribute it, including the changes you make, under the same license.

Official prebuilt binaries are sold, for convenience, on [itch.io](https://solemn-scribe.itch.io/phantom-fence) and in the Microsoft Store. Every shipped binary version corresponds to a release tag in this repository, which is the corresponding source for GPL purposes. Buying a binary supports development; building your own from this source is equally legitimate.

The "Phantom Fence" name and icon identify the official builds. The GPL licenses the code, not the trademark — redistributed or modified builds should use a different name and icon so users can tell them apart from official releases.

Copyright (C) 2026 Solemn Scribe.
