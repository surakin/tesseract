#pragma once

// Qt6 accessibility bridge: builds a real, navigable QAccessibleInterface
// tree from tk::build_access_tree()'s output, registered via Qt's own
// QAccessibleInterface factory mechanism — so it becomes part of Qt's
// *existing* single AT-SPI application instead of a second, separate one
// (the problem the reverted AccessKit-based Linux spike hit). See
// /home/rayden/.claude/plans/i-need-an-assessment-graceful-steele.md for the
// full history — this file supersedes the earlier hardcoded
// qt_accessible_spike.{h,cpp} (one fake button) now that the generic
// role/name/state/action model exists to build a real tree from.

#include <QtWidgets/QWidget>

#include <string>

namespace tk
{
class Widget;
}

namespace tk::qt6
{

class Surface;

// Call once (e.g. from main()) before any accessible query can occur.
void install_accessible_factory();

// Surface registers itself once constructed and unregisters first thing in its
// destructor. The factory answers only for registered surfaces, so it never
// has to dynamic_cast a QObject that is still being built or already being
// torn down (its vtable is not a Surface's then, and RTTI can crash).
void register_accessible_surface(Surface* surface);
void unregister_accessible_surface(Surface* surface);

// Fires a QAccessible::Focus event on whichever AccessNode corresponds to
// `now`, if any — called from tk::qt6::Host's on_focus_changed_() override
// (ui/linux-qt/tk/host_qt.cpp), so ordinary tk-level Tab-focus moves (not
// just ListView/GridView row navigation, see hook_selection_changed()) are
// announced too. Mirrors tk::win32::notify_focus_changed /
// tk::macos::notify_focus_changed. `old` is unused today; kept for symmetry
// with Host::on_focus_changed_'s signature and those two.
void notify_focus_changed(Surface* surface, tk::Widget* old, tk::Widget* now);

// Speaks `text` via a QAccessibleAnnouncementEvent (Qt >= 6.8; no-op on
// older Qt). Called from tk::qt6::Host's on_announce_() override.
void announce(Surface* surface, const std::string& text, bool assertive);

} // namespace tk::qt6
