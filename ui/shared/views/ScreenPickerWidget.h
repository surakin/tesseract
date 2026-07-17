#pragma once

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/scrollable_base.h"
#include "tk/screen_capture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

// Modal overlay that displays available screen capture sources and lets the
// user pick one before sharing starts. Appears inside the room area, above
// the call overlay. Dismissed by selection or by the Cancel button.
class ScreenPickerWidget : public tk::ScrollableBase
{
public:
    explicit ScreenPickerWidget(std::vector<tk::ScreenSource> sources);
    ~ScreenPickerWidget() override;

    // Fired when the user selects a source. The widget should be removed from
    // the parent after this fires.
    std::function<void(std::string source_id)> on_source_selected;

    // Fired when the user presses Cancel. Remove the widget.
    std::function<void()> on_cancelled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_drag(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;

    // Store a captured thumbnail for sources_[index]; the tk::Image itself is
    // created lazily in paint() (needs a CanvasFactory, unavailable here).
    void set_thumbnail(std::size_t index, std::vector<std::uint8_t> rgba,
                       std::uint32_t w, std::uint32_t h);

    // Weak liveness token for the background thumbnail-capture worker: since
    // thumbnails arrive asynchronously and this widget can be destroyed
    // (selection or cancel) before they all finish, callers must check
    // alive_token().lock() before touching this widget from a posted task.
    std::weak_ptr<bool> alive_token() const { return alive_; }

private:
    struct TileThumb
    {
        std::vector<std::uint8_t>  rgba;
        std::uint32_t              w = 0, h = 0;
        std::unique_ptr<tk::Image> image; // lazily built in paint()
    };

    float content_height() const override { return content_h_; }

    std::vector<tk::ScreenSource> sources_;
    std::vector<TileThumb>        thumbs_; // parallel to sources_
    tk::CanvasFactory*            image_factory_ = nullptr;
    std::shared_ptr<bool>         alive_ = std::make_shared<bool>(true);
    float                         content_h_ = 0.0f;
    int                           hovered_idx_ = -1;
    // Tile clicked on pointer-down; the actual selection fires on pointer-up
    // (see on_pointer_up) so pointer-down can claim capture by returning true
    // without immediately destroying this widget from inside dispatch.
    int                           pressed_tile_idx_ = -1;

    tk::Button* cancel_btn_ = nullptr;

    // Cached tile rects for hit-testing (set in arrange).
    std::vector<tk::Rect> tile_rects_;
    tk::Rect              header_rect_{};
    tk::Rect              grid_rect_{};
};

} // namespace tesseract::views
