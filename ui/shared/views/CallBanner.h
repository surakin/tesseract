#pragma once

// Banner shown at the top of the message area for as long as the room has a
// live call the user is not part of. A state of the room, not an event: it has
// no dismiss and no timeout; the shell hides it (clear()) once the user has
// joined. Shows the avatars of the members in the call and a Join button.
// Mirrors the PinnedBanner / VerificationBanner layout contract: fixed
// kBannerH, shown/hidden by the shell, drives on_layout_changed so the parent
// RoomView reflows.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class CallBanner : public tk::Widget
{
public:
    struct Member
    {
        std::string user_id;
        std::string display_name;
    };
    // Resolves a member's avatar image (nullptr → initials fallback).
    using AvatarProvider = std::function<const tk::Image*(const std::string& user_id)>;

    CallBanner();
    ~CallBanner() override = default;

    // Configure and show the banner. Overwrites any previous call state.
    // call_intent is "audio" | "video" | "" (empty = unknown). Join is shown
    // disabled when join_enabled is false (the user is in a different call).
    void set_call(const std::string& call_intent, std::vector<Member> members,
                  std::function<void()> on_join, bool join_enabled);

    void set_avatar_provider(AvatarProvider p) { avatar_provider_ = std::move(p); }

    // Hide the banner and clear its callback.
    void clear();

    const std::vector<Member>& members() const { return members_; }

    static constexpr float kBannerH = 48.0f;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;

    // Accessibility: a named Group so the call type and who is in it (both
    // canvas-painted) are announced; Join attaches under it.
    tk::Role access_role() const override { return tk::Role::Group; }
    std::string access_name() const override;

private:
    // Number of avatar slots drawn for `count` members (the last one becomes a
    // "+N" disc when members overflow).
    static std::size_t avatar_slots(std::size_t count);

    std::string           call_intent_; // "audio" | "video" | ""
    std::vector<Member>   members_;
    AvatarProvider        avatar_provider_;
    tk::Button*           join_btn_ = nullptr;
    tk::IconCache         phone_icon_;
    tk::IconCache         video_icon_;
    tk::Rect              icon_rect_{};
    tk::Rect              text_rect_{};
    tk::Rect              avatars_rect_{};
};

} // namespace tesseract::views
