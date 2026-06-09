# TODO (Windows-only): window geometry is saved/restored in physical pixels, ignoring DPI

**Pick this up from a Windows machine** — it's Win32-only and can't be built/verified on Linux.
Pre-existing bug, unrelated to the pre-launch remediation branches.

## Symptom / repro
Save the window position+size on a HiDPI monitor (e.g. 200% scale), then restore it on a
display at a different scale (classic case: a Remote Desktop session at 100%). The window
comes back 2× (or 3×) too large. The reverse (save at 100%, restore at 200%) makes it tiny.

## Root cause
The saved geometry is stored in **physical pixels at the saving monitor's DPI**, then
restored verbatim as physical pixels regardless of the restoring monitor's DPI.

- Save (physical px via `GetWindowRect`):
  - `ui/windows/src/MainWindow.cpp` `WM_SIZE` ~lines 896–908 and `WM_MOVE` ~lines 920–934 →
    write `Settings::instance().main_window_geometry` `{x,y,w,h,valid}`.
- Restore (physical px via `SetWindowPos`):
  - `ui/windows/src/MainWindow.cpp` `on_create()` ~lines 1374–1389 (after `clamp_to_screens_`).
- The struct is DPI-agnostic and shared with pop-out windows:
  - `client/include/tesseract/settings.h:144` `struct WindowGeometry { int x,y,w,h; bool valid; }`,
    used by both `main_window_geometry` (`:157`) and `PopoutEntry::geometry` (`:154`).
  - Serialized in `client/src/settings.cpp:59–63` (main) and `:75–79` (pop-outs).
- **Pop-out room windows have the same bug**: `ui/windows/src/RoomWindow.cpp:96`
  `get_saved_popout_geometry_(800, 600)` restores the same physical-pixel `WindowGeometry`.

Note what's already correct (don't re-do):
- `clamp_to_screens_` (impl uses `get_screen_work_areas_()`, `MainWindow.cpp:6954`) already
  clamps the restored rect to the visible work area, so off-screen/oversized windows are
  pulled back on-screen. The bug is purely the **size scaling**, not off-screen handling.
- There is a per-window DPI helper `dip_to_phys(float)` at `ui/windows/src/MainWindow.h:318`,
  built on `GetDpiForWindow(hwnd_)` (`:315`). Reuse it; add the inverse where needed.

## Recommended fix — persist the save-time DPI, rescale on restore
Make the on-disk geometry DPI-independent without breaking existing saved files.

1. **Add a DPI field** to `WindowGeometry` (`settings.h:144`):
   ```cpp
   struct WindowGeometry { int x=0,y=0,w=0,h=0; int dpi=0; bool valid=false; };  // dpi=0 → unknown (legacy)
   ```
   Serialize it in `settings.cpp` for both the main window (`:59–63`) and pop-outs (`:75–79`):
   write `j["dpi"] = g.dpi;`, read `g.dpi = mw.value("dpi", 0);`.

2. **On save** (the two `MainWindow.cpp` sites + the pop-out save path in `RoomWindowBase`/
   `RoomWindow`): set `g.dpi = GetDpiForWindow(hwnd)` alongside the physical `x/y/w/h`.

3. **On restore** (`MainWindow.cpp:1374` and `RoomWindow.cpp:96`): determine the **target DPI**
   for the monitor the saved position lands on — `GetDpiForMonitor(MonitorFromPoint({g.x,g.y},
   MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, …)` (or `GetDpiForWindow` after create) — then
   rescale **before** `clamp_to_screens_`:
   ```cpp
   if (g.valid && g.dpi > 0 && targetDpi > 0 && targetDpi != g.dpi) {
       const double s = double(targetDpi) / double(g.dpi);
       g.w = lround(g.w * s);
       g.h = lround(g.h * s);
       // optional: rescale the offset of x/y from the target monitor's origin by `s`
       // so the window keeps its relative position on that monitor.
   }
   ```
   **Back-compat:** `g.dpi == 0` (files written before this change) → skip rescaling, i.e. treat
   the saved physical px as already-correct for the current machine. This is a no-op for existing
   users restoring on the same display they saved on (the common case), and only the *next* save
   stamps a real DPI, healing it going forward.

Alternative (cleaner format, worse back-compat): store `w/h` in **logical (96-DPI) units**
(`phys_to_dip` on save, `dip_to_phys` at the target DPI on restore). Rejected as the default
because existing physical-pixel saves would be misread as logical on the first restore after
upgrade (HiDPI users' windows would shrink once). The `dpi`-field approach avoids that.

## Files to touch
- `client/include/tesseract/settings.h` — add `dpi` to `WindowGeometry`.
- `client/src/settings.cpp` — (de)serialize `dpi` for main + pop-out geometry.
- `ui/windows/src/MainWindow.cpp` — stamp `dpi` on save (WM_SIZE/WM_MOVE); rescale on restore (on_create).
- `ui/windows/src/RoomWindow.cpp` (+ `ui/shared/app/RoomWindowBase.*` if the save/restore helper lives there) — same for pop-outs.

## Verification (on Windows)
- Save at 200%, restore at 100% (or via RDP) → window keeps its logical size, not 2× px.
- Save at 100%, restore at 200% → not half-size.
- Multi-monitor with mixed DPI: drag to the other-DPI monitor, resize, relaunch → sane size.
- Existing `app_settings.json` from before the change (no `dpi`) → first launch unchanged on the
  same display; after one move/resize the new `dpi` is stamped.
- Pop-out room windows: repeat the save-HiDPI / restore-loDPI check.

## Cross-platform note
macOS (AppKit points) and GTK/Qt (logical coordinates) persist geometry in DPI-independent
units already, so they don't share this bug — this fix is Win32-only. Quick sanity-check the
GTK/Qt/macOS geometry save still uses logical units when you're in there, but no change expected.
