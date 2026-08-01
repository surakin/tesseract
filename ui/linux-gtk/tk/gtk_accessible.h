#pragma once

// GTK4 accessibility bridge: builds a real, navigable GtkAccessible tree
// from tk::build_access_tree()'s output — see
// /home/rayden/.claude/plans/i-need-an-assessment-graceful-steele.md for
// the full history. Supersedes the earlier hardcoded gtk_accessible_spike.
// {h,cpp} (one fake button).
//
// Unlike the Qt6 bridge (qt_accessible.cpp), GTK4 has no notion of a
// "virtual" (non-widget) accessible child: GtkWidget's own default
// GtkAccessible vfuncs walk the *real* widget tree unconditionally
// (confirmed against GTK's own source — gtk_widget_accessible_get_
// first_accessible_child() etc. just call _gtk_widget_get_first_child()),
// and the built-in AT-SPI Action interface is only ever registered for a
// genuine GTK_IS_WIDGET (gtk_atspi_get_action_vtable() returns NULL for
// anything else). So every AccessNode gets a real (zero-size, invisible)
// GtkWidget of its own — not a lightweight virtual GObject — reconciled
// against the tree on each relayout.

typedef struct _GtkWidget GtkWidget;

namespace tk::gtk4
{

class Surface;

// Attaches the real accessibility bridge to `surface`. Call once per
// Surface, after it's fully constructed.
void attach_accessible_bridge(Surface& surface);

} // namespace tk::gtk4
