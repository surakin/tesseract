#pragma once

// GTK4 host. The Surface owns a GtkOverlay containing a GtkDrawingArea
// (rendered into by the shared widget tree via the Cairo+Pango canvas
// backend) plus native overlay widgets (GtkEntry et al.) positioned over
// the canvas via margins.

#include "canvas.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#include <functional>
#include <memory>

typedef struct _GtkWidget GtkWidget;

namespace tk::gtk4 {

class Host;

// Embed `widget()` (a GtkOverlay) into your normal GTK4 widget tree.
// Everything inside paints through the shared toolkit; child native
// widgets like GtkEntry layer on top of the canvas via NativeTextField.
class Surface {
public:
    explicit Surface(const Theme& theme = Theme::light(),
                     bool transparent   = false);
    ~Surface();
    Surface(const Surface&)            = delete;
    Surface& operator=(const Surface&) = delete;

    // The GtkOverlay you embed in your normal GTK4 layout. Owned by the
    // Surface; the Surface destructor unrefs the toplevel.
    GtkWidget* widget() const;

    tk::Host&   host();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange + repaint on the existing root. Call
    // after mutating widget state in a way that affects layout. The
    // GtkDrawingArea's "resize" signal also calls this automatically.
    void relayout();

    // Callback fired at the tail of every relayout (initial, resize,
    // explicit). Use this from integration code to keep native overlays
    // — GtkEntry positions etc. — aligned with the shared widget tree.
    void set_on_layout(std::function<void()> cb);

    // Install a drag-and-drop handler. When set, the drawing area
    // accepts any local file drop (and in-app image data) and invokes
    // the callback once per dropped file with raw bytes, OS-supplied
    // MIME, and basename. The shell dispatches by MIME. Pass {} to disable.
    void set_on_file_drop(FileDropHandler cb);
    // Deprecated alias.
    void set_on_image_drop(FileDropHandler cb) { set_on_file_drop(std::move(cb)); }

private:
    std::unique_ptr<Host> host_;
};

} // namespace tk::gtk4
