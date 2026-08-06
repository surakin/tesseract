#pragma once

// macOS NSAccessibility bridge: builds a real, navigable tree of
// TKAccessElement objects (an NSAccessibilityElement subclass) from
// tk::build_access_tree()'s output. Unlike Win32 (WM_GETOBJECT) or GTK4
// (ambient AT-SPI GtkAccessible registration), AppKit has no separate
// discovery step: the system just walks the real view hierarchy, asking
// each NSView its own -accessibilityXxx questions directly — so
// TKSurfaceView itself (in host_macos.mm) gets a handful of thin overrides
// that delegate here, playing the same "fragment root" role Windows'
// RootProvider does (the view IS the root AccessNode, not a separate
// wrapper around it).
//
// Mirrors ui/linux-qt/tk/qt_accessible.{h,cpp} and
// ui/windows/tk/win32_accessible.{h,cpp} as the reference pattern (same
// AccessKey identity scheme, same lazy rebuild-on-relayout model, same
// ListView/GridView "current row" reporting) — adapted to AppKit's object
// model. ARC manages TKAccessElement lifetime (no hand-rolled refcounting
// needed, unlike Windows' COM); the detach() pattern still applies, since
// VoiceOver can hold a strong reference past a node's retirement or past
// the whole Surface tearing down. See
// /home/rayden/.claude/plans/can-we-continue-with-dazzling-hoare.md for the
// Windows design this follows (Phase 3 of the accessibility rollout;
// macOS is the second platform in that phase) and
// docs/ACCESSIBILITY-PLAN.md for the full design history.
//
// UNVERIFIED: written without a macOS/Xcode toolchain available in this
// environment. Needs a real macOS build, then a VoiceOver pass, before
// this can be considered working.

#import <AppKit/AppKit.h>

namespace tk
{
class Widget;
}

namespace tk::macos
{

class Surface;

// Call once, from inside Surface's own constructor — self-attaching,
// mirrors tk::win32::attach_accessible_bridge / tk::gtk4::
// attach_accessible_bridge, so no shell-level (MainWindowController/
// RoomWindowController) wiring is needed beyond this one call.
void attach_accessible_bridge(Surface& surface);

// Call from Surface's destructor, BEFORE Host::detach() nulls out the
// view's hostPtr — detaches every still-live cached TKAccessElement (so a
// screen reader holding a reference across teardown degrades gracefully
// instead of touching a destroyed Surface/Host) and erases the bridge's
// registry entry.
void detach_accessible_bridge(Surface& surface);

// The following are called directly from TKSurfaceView's own
// -accessibilityXxx overrides (added in host_macos.mm) — `view` is the
// TKSurfaceView* itself, passed as `id` so this header doesn't need
// TKSurfaceView's @interface visible (it isn't, outside host_macos.mm).
// Each falls back to a safe default (NO / nil / empty array / zero rect)
// when no bridge is attached to `view` yet.
BOOL access_is_element(id view);
NSAccessibilityRole access_role(id view);
NSString* access_subrole(id view); // nil when the role has none
NSString* access_label(id view);
NSArray* access_children(id view);
NSArray* access_tabs(id view); // non-nil only when view's root node is Role::TabList
id access_hit_test(id view, NSPoint screen_point);
id access_focused_element(id view);

// Called by tk::macos::Host's on_focus_changed_() override (see
// ui/shared/tk/host.h) to post NSAccessibilityFocusedUIElementChangedNotification
// for ordinary tk-level Tab-focus moves. Unlike Qt6/GTK4 — whose AT-SPI
// focus reporting is scoped to ListView/GridView row navigation only,
// since Orca apparently didn't need more — VoiceOver is not forgiving
// about a missing focus notification, so this closes that gap for macOS
// specifically (same rationale as Windows' notify_focus_changed).
void notify_focus_changed(id view, tk::Widget* old_widget, tk::Widget* now_widget);

} // namespace tk::macos
