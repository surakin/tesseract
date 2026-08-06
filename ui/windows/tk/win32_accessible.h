#pragma once

// Windows UI Automation (UIA) accessibility bridge: builds a real, navigable
// IRawElementProviderFragment tree from tk::build_access_tree()'s output,
// answered directly from WM_GETOBJECT via UiaReturnRawElementProvider — the
// native Win32 discovery mechanism (unlike Qt6's QAccessibleInterface
// factory or GTK4's ambient AT-SPI GtkAccessible registration, Win32 has no
// ambient discovery: the OS asks the window directly).
//
// Mirrors ui/linux-qt/tk/qt_accessible.{h,cpp} as the reference pattern
// (same AccessKey identity scheme, same lazy rebuild-on-relayout model, same
// ListView/GridView "current row" reporting), adapted to UIA's COM/provider
// model and hand-rolled refcounting (see host_win32.cpp's own DropTarget
// class for the project's established idiom this follows). See
// /home/rayden/.claude/plans/can-we-continue-with-dazzling-hoare.md for the
// full plan this implements (Phase 3 of the accessibility rollout).
//
// UNVERIFIED: written without a Windows/MSVC toolchain available in this
// environment (the repo's MinGW cross-compile presets are not to be relied
// on — see the plan doc). Needs a real Windows/MSVC build, then Narrator/
// NVDA verification, before this can be considered working.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tk
{
class Widget;
}

namespace tk::win32
{

class Surface;

// Call once, from inside Surface's own constructor (after hwnd_/host_ are
// valid) — self-attaching, mirrors tk::gtk4::attach_accessible_bridge, so no
// shell-level (MainWindow/RoomWindow) wiring is needed beyond this one call.
void attach_accessible_bridge(Surface& surface);

// Call from Surface's destructor, BEFORE DestroyWindow(hwnd) — disconnects
// every still-live cached provider (UiaDisconnectProvider) so a UIA client
// (Narrator, NVDA) holding a reference across window teardown degrades
// gracefully instead of dereferencing freed state, then erases the bridge's
// registry entry. Mirrors DropTarget::detach_host()'s existing pattern in
// host_win32.cpp for the same class of problem.
void detach_accessible_bridge(HWND hwnd);

// Call from surface_wnd_proc's WM_GETOBJECT case, unconditionally — falls
// back to DefWindowProcW itself for anything that isn't a UiaRootObjectId
// request or that has no attached bridge, so the call site doesn't need to
// special-case either.
LRESULT handle_get_object(HWND hwnd, WPARAM wParam, LPARAM lParam);

// Called by tk::win32::Host's on_focus_changed_() override (see
// ui/shared/tk/host.h) to raise UIA_AutomationFocusChangedEventId for
// ordinary tk-level Tab-focus moves. Unlike Qt6/GTK4 — whose AT-SPI focus
// reporting is scoped to ListView/GridView row navigation only, since Orca
// apparently didn't need more — UIA is not forgiving about a missing focus
// event, so this closes that gap for Windows specifically.
void notify_focus_changed(HWND hwnd, tk::Widget* old_widget, tk::Widget* now_widget);

} // namespace tk::win32
