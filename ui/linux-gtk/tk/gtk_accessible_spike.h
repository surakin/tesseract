#pragma once

// Phase 2 spike (native GtkAccessible, mirroring
// ui/linux-qt/tk/qt_accessible_spike.h's QAccessibleInterface spike — see
// /home/rayden/.claude/plans/i-need-an-assessment-graceful-steele.md).
//
// Attaches one hardcoded accessible button under a real GtkWidget (a
// tk::gtk4::Surface's overlay), via a real (invisible, zero-size) marker
// GtkWidget that reimplements GtkAccessible itself. A first attempt tried
// attaching a bare, non-GtkWidget GObject as a "virtual child" directly —
// that doesn't work on GTK4: gtk_accessible_set_accessible_parent() only
// records the parent pointer on the *child's own* context, it does not
// register the child with the parent, and a sealed built-in type like
// GtkOverlay has no way to be told about externally-attached virtual
// children. The fix is this file: a real (if invisible) GtkWidget subclass
// that IS a genuine child in the widget tree (so the parent's default,
// real-widget-walking accessible behavior naturally discovers it), which
// itself overrides get_first_accessible_child()/get_next_accessible_sibling()
// to fan out to the hardcoded virtual button. Verified end-to-end with a
// standalone test exercising GTK's own accessible-tree walk functions
// directly (gtk_accessible_get_first_accessible_child() etc.) before this
// was written, since this project's shells never build/run the GTK4 target
// from the agent side.
//
// No generic tk::Widget-tree walking yet; that's the next step once this
// proves the mechanism for real (Accerciser check still needed).

typedef struct _GtkWidget GtkWidget;
typedef struct _GtkOverlay GtkOverlay;

namespace tk::gtk4
{

// Adds the marker widget as an overlay child of `overlay` (opted out of
// affecting its measurement), which fans out to one hardcoded accessible
// button. Call once per Surface, after the overlay is fully constructed.
void attach_accessible_spike(GtkOverlay* overlay);

} // namespace tk::gtk4
