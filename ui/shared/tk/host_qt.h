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

namespace tk::qt6 {

class Host;

class Surface : public QWidget {
public:
    explicit Surface(const Theme& theme = Theme::light(),
                      QWidget* parent = nullptr);
    ~Surface() override;

    tk::Host&   host();
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

    // Callback fired at the tail of every relayout. Use this from
    // integration code to keep native overlays (QLineEdit, ...) aligned
    // with the shared widget tree.
    void set_on_layout(std::function<void()> cb);

    // Install a drag-and-drop handler. When set, the Surface accepts
    // image file drops (matching the same allowlist as the clipboard-
    // paste path) and invokes the callback with the file bytes, MIME,
    // and basename. The shell wires this to the active ComposeBar's
    // `set_pending_image`. Pass {} to disable.
    void set_on_image_drop(ImageDropHandler cb);

protected:
    void paintEvent       (QPaintEvent*)      override;
    void resizeEvent      (QResizeEvent*)     override;
    void mousePressEvent  (QMouseEvent*)      override;
    void mouseReleaseEvent(QMouseEvent*)      override;
    void mouseMoveEvent   (QMouseEvent*)      override;
    void wheelEvent       (QWheelEvent*)      override;
    void leaveEvent       (QEvent*)           override;
    void dragEnterEvent   (QDragEnterEvent*)  override;
    void dragMoveEvent    (QDragMoveEvent*)   override;
    void dropEvent        (QDropEvent*)       override;

private:
    std::unique_ptr<Host> host_;
    ImageDropHandler      on_image_drop_;
};

} // namespace tk::qt6
