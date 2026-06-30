#pragma once

// Qt6 host. The Surface is a regular QWidget that owns:
//   - the root tk::Widget tree (mounted via host().set_root())
//   - a QPainter-backed tk::Canvas for each paintEvent
//   - the QtFactory used to decode images + build text layouts
//   - native QLineEdit overlays produced by tk::NativeTextField
//
// Embed Surface in your normal Qt UI; everything inside it paints
// through the shared toolkit.

#include "canvas.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#include <QtWidgets/QWidget>

#include <memory>

namespace tk
{
class AnimImageCache;
}

namespace tk::qt6
{

class Host;

class Surface : public QWidget
{
public:
    explicit Surface(const Theme& theme = Theme::light(),
                     QWidget* parent = nullptr, bool transparent = false);
    ~Surface() override;

    tk::Host& host();
    const tk::Theme& theme() const;

    // Mount the root widget. Triggers an immediate measure + arrange
    // against the current widget bounds, plus a repaint.
    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange on the existing root + repaint. Call
    // after mutating widget state in a way that affects layout (showing
    // / hiding children, replacing labels with very different lengths,
    // …). resizeEvent already does this automatically.
    void relayout();
    void set_theme(const Theme& t);

    // Animated-image partial repaints. Point the surface at the shell's
    // animation cache once at setup; then call update_anim_regions() from the
    // animation timer instead of update() to invalidate only the rects where
    // animated images were drawn on the last paint.
    void set_anim_cache(const AnimImageCache* cache);
    void update_anim_regions();

    // Callback fired at the tail of every relayout. Use this from
    // integration code to keep native overlays (QLineEdit, ...) aligned
    // with the shared widget tree.
    void set_on_layout(std::function<void()> cb);

    // Install a drag-and-drop handler. When set, the Surface accepts
    // any file drop (and in-app image data) and invokes the callback
    // once per dropped file with raw bytes, OS-supplied MIME, and
    // basename. The shell dispatches by MIME — images go to the
    // compose bar's image preview, files go to its file chip.
    // Pass {} to disable.
    void set_on_file_drop(FileDropHandler cb);
    // Deprecated alias.
    void set_on_image_drop(FileDropHandler cb)
    {
        set_on_file_drop(std::move(cb));
    }

    // Called when a drop fails because the file could not be read.
    void set_on_file_drop_error(FileDropErrorHandler cb);

    // True while a drag is hovering over the Surface (between
    // dragEnter / dragLeave / drop). Painted as a translucent
    // "Drop to attach" overlay so the user sees a valid target.
    bool drag_active() const
    {
        return drag_active_;
    }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    std::unique_ptr<Host> host_;
    FileDropHandler on_file_drop_;
    FileDropErrorHandler on_file_drop_error_;
    bool drag_active_ = false;
};

} // namespace tk::qt6
