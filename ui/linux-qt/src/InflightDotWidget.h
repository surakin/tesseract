#pragma once

#include "tk/canvas.h"

#include <QWidget>
#include <cstdint>

// Custom status-bar widget that replaces the plain QLabel dot. Draws the
// center dot via tk::Canvas and — when count >= 2 — an 8-dot spinning ring
// whose speed scales with the request count. The shell (MainWindow, which
// inherits ShellBase) pushes color/count via update_state() and the current
// rotation phase via update_phase(), so this widget stays decoupled from
// ShellBase internals.
class InflightDotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit InflightDotWidget(QWidget* parent = nullptr);

    // Called from MainWindow::on_inflight_ui_() on every count/color change.
    void update_state(uint32_t count, tk::Color dot_color);

    // Called from MainWindow::repaint_anim_frame_() on every animation tick.
    void update_phase(float phase);

protected:
    void paintEvent(QPaintEvent*) override;
    QSize sizeHint() const override;

private:
    uint32_t  count_     = 0;
    tk::Color dot_color_ = tk::Color::rgb(0x40BF4D);
    float     phase_     = 0.0f;
};
