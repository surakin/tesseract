#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"
#include "tk/screen_capture.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

// Modal overlay that displays available screen capture sources and lets the
// user pick one before sharing starts. Appears inside the room area, above
// the call overlay. Dismissed by selection or by the Cancel button.
class ScreenPickerWidget : public tk::Widget
{
public:
    explicit ScreenPickerWidget(std::vector<tk::ScreenSource> sources);

    // Fired when the user selects a source. The widget should be removed from
    // the parent after this fires.
    std::function<void(std::string source_id)> on_source_selected;

    // Fired when the user presses Cancel. Remove the widget.
    std::function<void()> on_cancelled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;

private:
    std::vector<tk::ScreenSource> sources_;
    int                           hovered_idx_ = -1;

    tk::Button* cancel_btn_ = nullptr;

    // Cached tile rects for hit-testing (set in arrange).
    std::vector<tk::Rect> tile_rects_;
    tk::Rect              header_rect_{};
    tk::Rect              grid_rect_{};
};

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
