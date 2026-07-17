#pragma once

// Banner shown at the top of the message area when a remote participant opens
// a call slot in the current room. Mirrors the PinnedBanner / VerificationBanner
// layout contract: fixed kBannerH, shown/hidden by the shell via set_call() /
// clear(), drives on_layout_changed so the parent RoomView reflows.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class IncomingCallBanner : public tk::Widget
{
public:
    IncomingCallBanner();
    ~IncomingCallBanner() override = default;

    // Configure and show the banner. Overwrites any previous call state.
    // on_answer / on_decline are fired on button press; on_decline is also
    // responsible for any auto-dismiss timer at the RoomView level.
    // call_intent is "audio" | "video" | "" (empty = unknown).
    void set_call(std::string           caller_display_name,
                  const std::string&    call_intent,
                  std::function<void()> on_answer,
                  std::function<void()> on_decline);

    // Hide the banner and clear its callbacks.
    void clear();

    static constexpr float kBannerH = 48.0f;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

private:
    std::string   caller_name_;
    std::string   call_intent_; // "audio" | "video" | ""
    tk::Button*   decline_btn_ = nullptr;
    tk::Button*   answer_btn_  = nullptr;
    tk::IconCache phone_icon_;
    tk::IconCache video_icon_;
    tk::Rect      icon_rect_{};
    tk::Rect      text_rect_{};
};

} // namespace tesseract::views
