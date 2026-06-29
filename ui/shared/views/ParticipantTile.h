#pragma once
#ifdef TESSERACT_CALLS_ENABLED

// A self-contained widget representing a single call participant.
//
// Renders a video frame, an avatar disc fallback when no frame is available,
// a display-name label at the bottom, mic-off and video-off status badges,
// and a pin button that appears in the corner of the video image on hover.
//
// Video pixels arrive pre-converted to premultiplied BGRA from EventHandlerBase
// (worker thread). ParticipantTile stores a shared_ptr and calls
// draw_bgra_premult_pixels() at paint time — no per-frame copy or conversion.

#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class ParticipantTile : public tk::Widget
{
public:
    struct State
    {
        std::string participant_id;
        std::string user_id;
        std::string display_name;
        bool        audio_muted = false;
        bool        video_muted = false;

        // Premultiplied BGRA frame pre-converted on the worker thread.
        // Shared ownership lets EventHandlerBase, CallOverlayWidget, and
        // ParticipantTile all point to the same buffer with zero extra copies.
        std::shared_ptr<std::vector<std::uint8_t>> pending_bgra;
        std::uint32_t pending_w = 0, pending_h = 0;

        // True when this tile shows the local user's own camera feed.
        // The video is horizontally mirrored so it matches a mirror reflection.
        bool is_self = false;
        bool pinned  = false;
        // True when this tile displays a screen-share track rather than a camera.
        // Screen tiles: no mirror flip, prefer 16:9 aspect, show "Screen" label.
        bool is_screen_share_tile = false;
    };

    ParticipantTile();

    void set_state(State s);

    // Update only the video buffer without touching metadata (display_name,
    // audio_muted, etc.). Takes ownership of the shared_ptr so no second
    // memcpy occurs on the UI thread — avoids the full State reconstruction
    // that set_state() would require on each video frame.
    void push_video_frame(std::uint32_t w, std::uint32_t h,
                          std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Update only the pinned flag without touching video or other metadata.
    void set_pinned(bool pinned);

    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& participant_id)> fn);
    void set_repaint_requester(std::function<void()> fn);

    // Fires when the user clicks the pin button.
    std::function<void(const std::string& participant_id)> on_pin_toggled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside) override;

private:
    State     state_;
    bool      hover_       = false;
    bool      video_hover_ = false;  // true only when pointer is inside the video image rect
    bool      pin_pressed_ = false;

    // World-space rects updated each paint() once the actual letterbox is known.
    // Both are zeroed when video is muted so pointer hit-tests silently fail.
    tk::Rect  video_rect_{};  // the drawn video image rect (for hover detection)
    tk::Rect  pin_rect_{};    // top-left corner of video_rect_ (for pin button)

    // Icon caches — one per SVG context.
    tk::IconCache pin_icon_;
    tk::IconCache mic_off_icon_;
    tk::IconCache video_off_icon_;

    std::function<const tk::Image*(const std::string&)> avatar_provider_;
    std::function<void()>                               repaint_requester_;
};

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
