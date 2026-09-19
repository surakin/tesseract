#include "CallBanner.h"

#include "icons.h"
#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPadX      = 12.0f;
constexpr float kIconSz    = 20.0f;
constexpr float kGap       = 10.0f;
constexpr float kBtnW      = 76.0f;
constexpr float kBtnH      = 28.0f;
constexpr float kAvatarD   = 28.0f;
constexpr float kOverlap   = 8.0f;  // how far each avatar covers the previous
constexpr float kRing      = 2.0f;  // accent-coloured separator around avatars
constexpr std::size_t kMaxAvatars = 5;
} // namespace

std::size_t CallBanner::avatar_slots(std::size_t count)
{
    return std::min(count, kMaxAvatars);
}

CallBanner::CallBanner()
{
    auto join = tk::create_widget<tk::Button>(this, tk::tr("Join"),
        std::function<void()>{}, tk::Button::Variant::Primary);
    join_btn_ = add_child(std::move(join));
    set_visible(false);
}

void CallBanner::set_call(const std::string& call_intent, std::vector<Member> members,
                          std::function<void()> on_join, bool join_enabled)
{
    call_intent_ = call_intent;
    members_     = std::move(members);
    if (join_btn_)
    {
        join_btn_->set_on_click(std::move(on_join));
        join_btn_->set_enabled(join_enabled);
    }
    set_visible(true);
}

void CallBanner::clear()
{
    call_intent_.clear();
    members_.clear();
    if (join_btn_) join_btn_->set_on_click({});
    set_visible(false);
}

tk::Size CallBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    if (!visible()) return {0.0f, 0.0f};
    return {constraints.w, kBannerH};
}

void CallBanner::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (bounds_.h <= 0.0f) return;

    const float mid_y = bounds_.y + bounds_.h * 0.5f;

    icon_rect_ = {bounds_.x + kPadX, mid_y - kIconSz * 0.5f, kIconSz, kIconSz};

    // Right side: [avatars] [Join], with kPadX outer margin.
    const float right = bounds_.x + bounds_.w - kPadX;
    const tk::Rect join_r = {right - kBtnW, mid_y - kBtnH * 0.5f, kBtnW, kBtnH};
    if (join_btn_) join_btn_->arrange(ctx, join_r);

    const std::size_t slots = avatar_slots(members_.size());
    const float stack_w = slots == 0
        ? 0.0f
        : kAvatarD + static_cast<float>(slots - 1) * (kAvatarD - kOverlap);
    avatars_rect_ = {join_r.x - kGap - stack_w, mid_y - kAvatarD * 0.5f, stack_w, kAvatarD};

    // Text area between the icon and the avatar stack.
    const float text_x = icon_rect_.x + kIconSz + kGap;
    const float text_end = slots == 0 ? join_r.x : avatars_rect_.x;
    text_rect_ = {text_x, bounds_.y, std::max(0.0f, text_end - kGap - text_x), bounds_.h};
}

void CallBanner::paint_before_children(tk::PaintCtx& ctx)
{
    if (bounds_.h <= 0.0f) return;

    const auto& pal = ctx.theme.palette;
    ctx.canvas.fill_rect(bounds_, pal.accent);

    constexpr tk::Color kWhite{255, 255, 255, 255};

    const bool is_video = (call_intent_ == "video");
    if (is_video)
        video_icon_.draw(ctx.canvas, ctx.factory, kVideoSvg, icon_rect_, kIconSz, kWhite);
    else
        phone_icon_.draw(ctx.canvas, ctx.factory, kPhoneSvg, icon_rect_, kIconSz, kWhite);

    const std::string label = is_video ? tk::tr("Video call in progress")
                              : call_intent_ == "audio" ? tk::tr("Voice call in progress")
                                                        : tk::tr("Call in progress");

    if (text_rect_.w > 0.0f)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Body;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = text_rect_.w;
        auto layout  = ctx.factory.build_text(label, ts);
        if (layout)
        {
            const tk::Size sz = layout->measure();
            const float ty = text_rect_.y + (text_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*layout, {text_rect_.x, ty}, kWhite);
        }
    }

    // Overlapped avatar stack, oldest member leftmost. Each avatar sits on an
    // accent-coloured ring so neighbours stay distinct.
    const std::size_t slots = avatar_slots(members_.size());
    const bool overflow = members_.size() > kMaxAvatars;
    for (std::size_t i = 0; i < slots; ++i)
    {
        const float cx = avatars_rect_.x + kAvatarD * 0.5f +
                         static_cast<float>(i) * (kAvatarD - kOverlap);
        const tk::Point centre{cx, avatars_rect_.y + kAvatarD * 0.5f};
        const float ring_d = kAvatarD + 2.0f * kRing;
        ctx.canvas.fill_rounded_rect(
            {centre.x - ring_d * 0.5f, centre.y - ring_d * 0.5f, ring_d, ring_d},
            ring_d * 0.5f, pal.accent);

        if (overflow && i + 1 == slots)
        {
            const std::size_t hidden = members_.size() - (slots - 1);
            draw_avatar(ctx.canvas, nullptr, centre, kAvatarD,
                        "+" + std::to_string(hidden), pal.avatar_initials_bg,
                        pal.avatar_initials_text);
            continue;
        }
        const Member& m = members_[i];
        const tk::Image* img = avatar_provider_ ? avatar_provider_(m.user_id) : nullptr;
        draw_avatar(ctx.canvas, img, centre, kAvatarD,
                    m.display_name.empty() ? m.user_id : m.display_name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);
    }
}

std::string CallBanner::access_name() const
{
    const std::string type = call_intent_ == "video" ? tk::tr("Video call in progress")
                             : call_intent_ == "audio" ? tk::tr("Voice call in progress")
                                                       : tk::tr("Call in progress");
    if (members_.empty()) return type;
    std::string names;
    for (const auto& m : members_)
    {
        if (!names.empty()) names += ", ";
        names += m.display_name.empty() ? m.user_id : m.display_name;
    }
    return tk::trf(tk::tr("{0}: {1}"), {type, names});
}

} // namespace tesseract::views
