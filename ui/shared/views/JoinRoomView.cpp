#include "JoinRoomView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kPadX = 20.0f;
constexpr float kPadY = 16.0f;
constexpr float kGap = 10.0f;
constexpr float kSmallGap = 6.0f;
constexpr float kTitleH = 28.0f;
constexpr float kInputH = 32.0f;
constexpr float kLookupBtnW = 88.0f;
constexpr float kBtnH = 32.0f;
constexpr float kBtnW = 96.0f;
constexpr float kStatusH = 20.0f;
constexpr float kAvatarSize = 56.0f;
constexpr float kCardPadX = 14.0f;
constexpr float kCardPadY = 12.0f;
constexpr float kTopicMaxH = 66.0f;
constexpr float kPillPadX = 8.0f;
constexpr float kPillH = 18.0f;
constexpr float kRadius = 6.0f;
constexpr float kBorderW = 1.0f;

// Pill colours for join rules
tk::Color join_rule_bg(const std::string& rule)
{
    if (rule == "public" || rule == "knock")
    {
        return tk::Color::rgba(0x2e, 0x7d, 0x32,
                               0xff); // green — freely joinable
    }
    if (rule == "restricted" || rule == "knock_restricted" ||
        rule == "invite" || rule == "private")
    {
        return tk::Color::rgba(0xe6, 0x5c, 0x00, 0xff); // amber — restricted
    }
    return tk::Color::rgba(0x60, 0x60, 0x60, 0xff); // gray — unknown
}

std::string join_rule_label(const std::string& rule)
{
    if (rule == "public")
    {
        return tk::tr("Public");
    }
    if (rule == "knock")
    {
        return tk::tr("Knock");
    }
    if (rule == "restricted")
    {
        return tk::tr("Restricted");
    }
    if (rule == "knock_restricted")
    {
        return tk::tr("Knock+Restricted");
    }
    if (rule == "invite")
    {
        return tk::tr("Invite only");
    }
    if (rule == "private")
    {
        return tk::tr("Private");
    }
    return tk::tr("Unknown");
}

} // namespace

JoinRoomView::JoinRoomView()
{
    auto lookup = std::make_unique<tk::Button>(
        tk::tr("Look up"), std::function<void()>{}, tk::Button::Variant::Primary);
    lookup->set_on_click(
        [this]
        {
            if (on_lookup_requested && !alias_text_.empty())
            {
                on_lookup_requested(alias_text_);
            }
        });
    lookup_btn_ = add_child(std::move(lookup));

    auto join = std::make_unique<tk::Button>(tk::tr("Join"), std::function<void()>{},
                                             tk::Button::Variant::Primary);
    join->set_on_click(
        [this]
        {
            if (on_join_requested)
            {
                std::string id =
                    preview_.room_id.empty() ? alias_text_ : preview_.room_id;
                if (!id.empty())
                {
                    on_join_requested(id);
                }
            }
        });
    join_btn_ = add_child(std::move(join));

    auto cancel = std::make_unique<tk::Button>(
        tk::tr("Cancel"), std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_on_click(
        [this]
        {
            if (on_cancel)
            {
                on_cancel();
            }
        });
    cancel_btn_ = add_child(std::move(cancel));

    auto status = std::make_unique<tk::Label>("", tk::FontRole::Body);
    status->set_halign(tk::TextHAlign::Center);
    status->set_trim(tk::TextTrim::Ellipsis);
    status_lbl_ = add_child(std::move(status));

    apply_state();
}

void JoinRoomView::set_state(State s)
{
    state_ = s;
    if (s != State::Error)
    {
        error_msg_.clear();
    }
    if (s != State::Preview)
    {
        preview_ = {};
    }
    apply_state();
}

void JoinRoomView::set_preview(const tesseract::RoomSummary& summary)
{
    preview_ = summary;
    state_ = State::Preview;
    error_msg_.clear();
    apply_state();
}

void JoinRoomView::set_error(std::string msg)
{
    error_msg_ = std::move(msg);
    state_ = State::Error;
    apply_state();
}

void JoinRoomView::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

bool JoinRoomView::alias_field_visible() const
{
    return state_ != State::Joining;
}

tk::Rect JoinRoomView::alias_field_rect() const
{
    return alias_field_visible() ? alias_field_rect_ : tk::Rect{};
}

void JoinRoomView::apply_state()
{
    bool show_join = (state_ == State::Preview);
    bool show_status = (state_ == State::Loading || state_ == State::Error);

    if (join_btn_)
    {
        join_btn_->set_visible(show_join);
    }

    if (status_lbl_)
    {
        status_lbl_->set_visible(show_status);
        if (state_ == State::Loading)
        {
            status_lbl_->set_text(tk::tr("Looking up room\xe2\x80\xa6"));
            status_lbl_->set_colour({}); // reset to default
        }
        else if (state_ == State::Error)
        {
            status_lbl_->set_text(error_msg_.empty() ? tk::tr("Room not found.")
                                                     : error_msg_);
            status_lbl_->set_colour(tk::Color::rgb(0xCC2200));
        }
        else
        {
            status_lbl_->set_text("");
            status_lbl_->set_colour({});
        }
    }

    if (lookup_btn_)
    {
        lookup_btn_->set_visible(state_ != State::Joining);
    }
}

tk::Size JoinRoomView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, constraints.h};
}

void JoinRoomView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    float x = bounds.x + kPadX;
    float y = bounds.y + kPadY;
    float inner_w = bounds.w - kPadX * 2.0f;

    // Title row (painted directly — no Label child to keep it simple).
    y += kTitleH + kGap;

    // Input row: [alias field] [kSmallGap] [Look up button].
    float lookup_x = x + inner_w - kLookupBtnW;
    alias_field_rect_ = {x, y, inner_w - kLookupBtnW - kSmallGap, kInputH};
    if (lookup_btn_)
    {
        lookup_btn_->arrange(ctx, {lookup_x, y, kLookupBtnW, kBtnH});
    }
    y += kInputH + kGap;

    // Status / error label.
    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->arrange(ctx, {x, y, inner_w, kStatusH});
        y += kStatusH + kSmallGap;
    }

    // Preview card.
    preview_card_rect_ = {};
    if (state_ == State::Preview)
    {
        // Card occupies inner_w, auto height.
        float card_x = x;
        float card_y = y;
        float info_x = card_x + kCardPadX + kAvatarSize + kGap;
        float info_w = inner_w - kCardPadX * 2.0f - kAvatarSize - kGap;
        float info_h = kAvatarSize; // clamp avatar + info side by side

        // Estimate topic height.
        float topic_h = 0.0f;
        if (!preview_.topic.empty())
        {
            topic_h = std::min(kTopicMaxH, 18.0f * 3.0f); // up to 3 lines
        }

        float card_h = kCardPadY * 2.0f + info_h +
                       (topic_h > 0 ? kSmallGap + topic_h : 0.0f);
        preview_card_rect_ = {card_x, card_y, inner_w, card_h};
        y += card_h + kGap;
    }

    // Button row anchored to the bottom or below the card.
    // Cancel always visible; Join only in Preview state.
    float btn_row_y = std::max(y, bounds.y + bounds.h - kPadY - kBtnH);
    float btn_x = x + inner_w; // right-align

    if (join_btn_ && join_btn_->visible())
    {
        btn_x -= kBtnW;
        join_btn_->arrange(ctx, {btn_x, btn_row_y, kBtnW, kBtnH});
        btn_x -= kSmallGap;
    }

    if (cancel_btn_)
    {
        btn_x -= kBtnW;
        cancel_btn_->arrange(ctx, {btn_x, btn_row_y, kBtnW, kBtnH});
    }
}

void JoinRoomView::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Background.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Title.
    float x = bounds_.x + kPadX;
    float y = bounds_.y + kPadY;
    {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Title;
        ts.halign = tk::TextHAlign::Leading;
        ts.trim = tk::TextTrim::Ellipsis;
        auto lo = ctx.factory.build_text(tk::tr("Join a Room"), ts);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {x, y}, pal.text_primary);
        }
    }
    y += kTitleH + kGap;

    // Alias field background (the NativeTextField overlays this).
    if (alias_field_visible() && !alias_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(alias_field_rect_, kRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(alias_field_rect_, kRadius, pal.border,
                                       kBorderW);
    }
    y += kInputH + kGap;

    // Status label.
    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->paint(ctx);
        y += kStatusH + kSmallGap;
    }

    // Preview card.
    if (state_ == State::Preview && !preview_card_rect_.empty())
    {
        // Card border.
        ctx.canvas.fill_rounded_rect(preview_card_rect_, kRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(preview_card_rect_, kRadius, pal.border,
                                       kBorderW);

        float cx = preview_card_rect_.x + kCardPadX;
        float cy = preview_card_rect_.y + kCardPadY;
        float iw = preview_card_rect_.w - kCardPadX * 2.0f;

        // Avatar.
        tk::Point av_centre{cx + kAvatarSize * 0.5f, cy + kAvatarSize * 0.5f};
        const tk::Image* av_img = nullptr;
        if (avatar_provider_ && !preview_.avatar_url.empty())
        {
            av_img = avatar_provider_(preview_.avatar_url);
        }

        if (av_img)
        {
            ctx.canvas.draw_circle_image(*av_img, av_centre, kAvatarSize);
        }
        else
        {
            std::string_view disp = preview_.name.empty()
                                        ? std::string_view("#")
                                        : std::string_view(preview_.name);
            ctx.canvas.draw_initials_circle(disp, av_centre, kAvatarSize,
                                            pal.accent,
                                            tk::Color{255, 255, 255, 255});
        }

        // Info column to the right of the avatar.
        float info_x = cx + kAvatarSize + kGap;
        float info_y = cy;
        float info_w = iw - kAvatarSize - kGap;

        // Room name.
        {
            tk::TextStyle ts;
            ts.role = tk::FontRole::Title;
            ts.halign = tk::TextHAlign::Leading;
            ts.trim = tk::TextTrim::Ellipsis;
            ts.max_width = info_w;
            auto lo = ctx.factory.build_text(
                preview_.name.empty() ? preview_.room_id : preview_.name, ts);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {info_x, info_y}, pal.text_primary);
                info_y += lo->measure().h + kSmallGap;
            }
        }

        // Join rule pill.
        if (!preview_.join_rule.empty())
        {
            std::string label = join_rule_label(preview_.join_rule);
            tk::Color bg = join_rule_bg(preview_.join_rule);
            tk::TextStyle ts;
            ts.role = tk::FontRole::Small;
            auto lo = ctx.factory.build_text(label, ts);
            if (lo)
            {
                float pw = lo->measure().w + kPillPadX * 2.0f;
                tk::Rect pill{info_x, info_y, pw, kPillH};
                ctx.canvas.fill_rounded_rect(pill, kPillH * 0.5f, bg);
                ctx.canvas.draw_text(
                    *lo,
                    {info_x + kPillPadX,
                     info_y + (kPillH - lo->measure().h) * 0.5f},
                    tk::Color{255, 255, 255, 255});
                info_y += kPillH + kSmallGap;
            }
        }

        // Member count + encryption indicator.
        {
            std::string detail = tk::trf(
                tk::trn("{0} member", "{0} members",
                        static_cast<long>(preview_.num_joined_members)),
                {std::to_string(preview_.num_joined_members)});
            if (!preview_.encryption.empty())
            {
                detail += "  \xF0\x9F\x94\x92"; // UTF-8 🔒
            }
            tk::TextStyle ts;
            ts.role = tk::FontRole::Body;
            ts.halign = tk::TextHAlign::Leading;
            ts.trim = tk::TextTrim::Ellipsis;
            ts.max_width = info_w;
            auto lo = ctx.factory.build_text(detail, ts);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {info_x, info_y}, pal.text_secondary);
                info_y += lo->measure().h;
            }
        }

        // Topic (below the avatar row, full card width).
        if (!preview_.topic.empty())
        {
            float topic_y =
                preview_card_rect_.y + kCardPadY + kAvatarSize + kSmallGap;
            float topic_w = preview_card_rect_.w - kCardPadX * 2.0f;
            tk::TextStyle ts;
            ts.role = tk::FontRole::Body;
            ts.halign = tk::TextHAlign::Leading;
            ts.wrap = true;
            ts.max_width = topic_w;
            ts.max_height = kTopicMaxH;
            ts.trim = tk::TextTrim::Ellipsis;
            auto lo = ctx.factory.build_text(preview_.topic, ts);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {cx, topic_y}, pal.text_secondary);
            }
        }
    }

    // Buttons.
    if (join_btn_ && join_btn_->visible())
    {
        join_btn_->paint(ctx);
    }
    if (cancel_btn_)
    {
        cancel_btn_->paint(ctx);
    }

    // Look up button.
    if (lookup_btn_ && lookup_btn_->visible())
    {
        lookup_btn_->paint(ctx);
    }
}

} // namespace tesseract::views
