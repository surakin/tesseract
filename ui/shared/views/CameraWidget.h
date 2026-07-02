#pragma once

#include "tk/video_capture.h"
#include "tk/widget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace tesseract::views
{

// Full-surface overlay that shows a live mirrored camera preview with a
// 3-second countdown, then fires on_frame_captured with a BGRA8888 still
// frame and dismisses itself.  If no camera is available, on_dismissed is
// called immediately on the first paint without showing any UI.
//
// Lifecycle:
//   1. open() — starts the camera; the countdown clock starts when the first
//      frame arrives (so any OS permission dialog doesn't eat into the timer).
//   2. After kCountdownSecs: on_frame_captured(bgra, w, h) fires, then
//      on_dismissed fires.
//   3. Escape key (or dismiss() call): on_dismissed fires without capture.
//
// The widget requests continuous repaints while the countdown is running so
// the preview stays live.
class CameraWidget : public tk::Widget
{
public:
    static constexpr float kCountdownSecs = 3.0f;

    CameraWidget();
    ~CameraWidget() override;

    // Start the camera and the countdown clock. Safe to call before the
    // widget is visible (capture runs on a background thread).
    void open();

    // Stop the camera and fire on_dismissed (if not already dismissed).
    void dismiss();

    // Fired on the UI thread after the countdown completes.
    // bgra: row-major BGRA8888, stride = w*4, A=255 (opaque camera frame).
    std::function<void(std::vector<std::uint8_t> bgra,
                       std::uint32_t w, std::uint32_t h)>
        on_frame_captured;

    // Fired when the widget closes (after capture or on cancel/no-camera).
    std::function<void()> on_dismissed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;

private:
    std::unique_ptr<tk::VideoCapture> capture_;

    std::mutex              frame_mu_;
    std::vector<std::uint8_t> last_bgra_;
    std::uint32_t           frame_w_  = 0;
    std::uint32_t           frame_h_  = 0;

    std::optional<std::chrono::steady_clock::time_point> start_;
    bool opened_    = false;
    bool captured_  = false;
    bool dismissed_ = false;

    void do_capture_();
    void do_dismiss_();
};

} // namespace tesseract::views
