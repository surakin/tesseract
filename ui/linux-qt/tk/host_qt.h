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

    // Called when a drop fails because the file could not be read.
    void set_on_file_drop_error(FileDropErrorHandler cb);

protected:
    // Qt's own QWidget::event() intercepts Key_Tab/Key_Backtab for its
    // built-in focus-chain traversal *before* keyPressEvent ever runs
    // whenever this widget itself holds real Qt focus (which it does
    // whenever tk-level focus is on a plain canvas widget with no native
    // control of its own — see Host::claim_native_focus_container_). Mirrors
    // NavLineEdit::event()'s identical override in host_qt.cpp: intercept
    // Tab/Backtab here and forward to our own dispatch so it keeps landing
    // in Host::dispatch_key_down instead of Qt silently moving real focus
    // to whatever's next in Qt's *own* native tab-chain (the handful of
    // real QLineEdit/QTextEdit widgets embedded in this surface).
    bool event(QEvent*) override;
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
    FileDropErrorHandler on_file_drop_error_;
};

} // namespace tk::qt6
