#pragma once
#ifdef TESSERACT_CALLS_ENABLED

// A self-contained widget representing a single call participant.
//
// Renders a video frame (lazily uploaded from raw RGBA pixels), an avatar
// disc fallback when no frame is available, a display-name label at the
// bottom, mic-off and video-off status badges, and a pin button that appears
// in the corner of the video image on hover.
//
// The lazy RGBA upload pattern mirrors CallOverlay::Cell: pixels are stored in
// pending_rgba and uploaded to a tk::Image on the next paint() pass.

#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
        std::string display_name;
        bool        audio_muted = false;
        bool        video_muted = false;

        // RGBA data waiting to be uploaded to a tk::Image.
        std::vector<std::uint8_t> pending_rgba;
        std::uint32_t pending_w = 0, pending_h = 0;
        bool          video_dirty = false;

        std::unique_ptr<tk::Image> video_image;
        bool pinned = false;
    };

    ParticipantTile();

    void set_state(State s);

    // Update only the video buffer without touching metadata (display_name,
    // audio_muted, etc.). Avoids the full State reconstruction that
    // set_state() would require on each video frame.
    void push_video_frame(std::uint32_t w, std::uint32_t h,
                          const std::uint8_t* rgba, std::size_t sz);

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
