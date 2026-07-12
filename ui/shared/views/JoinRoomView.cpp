#include "JoinRoomView.h"

#include "html_spans.h"
#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kJoinRoomPadX = 20.0f;
constexpr float kJoinRoomPadY = 16.0f;
constexpr float kJoinRoomGap = 10.0f;
constexpr float kSmallGap = 6.0f;
constexpr float kTitleH = 28.0f;
constexpr float kInputH = 32.0f;
constexpr float kLookupBtnW = 88.0f;
constexpr float kJoinRoomBtnH = 32.0f;
constexpr float kJoinRoomBtnW = 96.0f;
constexpr float kStatusH = 20.0f;
constexpr float kJoinRoomAvatarSize = 56.0f;
constexpr float kCardPadX = 14.0f;
constexpr float kCardPadY = 12.0f;
constexpr float kJoinRoomTopicMaxH = 66.0f;
constexpr float kPillPadX = 8.0f;
constexpr float kPillH = 18.0f;
constexpr float kJoinRoomRadius = 6.0f;
constexpr float kJoinRoomBorderW = 1.0f;

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
    topic_layout_.reset();
    topic_spans_ = autolink_plain_to_spans(preview_.topic);
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

void JoinRoomView::on_theme_changed(const tk::Theme& t)
{
    if (auto field = native_field_.lock())
        field->set_text_color(t.palette.text_primary);
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
        }
        else if (state_ == State::Error)
        {
            status_lbl_->set_text(error_msg_.empty() ? tk::tr("Room not found.")
                                                     : error_msg_);
        }
        else
        {
            status_lbl_->set_text("");
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

    float x = bounds.x + kJoinRoomPadX;
    float y = bounds.y + kJoinRoomPadY;
    float inner_w = bounds.w - kJoinRoomPadX * 2.0f;

    // Title row (painted directly — no Label child to keep it simple).
    y += kTitleH + kJoinRoomGap;

    // Input row: [alias field] [kSmallGap] [Look up button].
    float lookup_x = x + inner_w - kLookupBtnW;
    alias_field_rect_ = {x, y, inner_w - kLookupBtnW - kSmallGap, kInputH};
    if (lookup_btn_)
    {
        lookup_btn_->arrange(ctx, {lookup_x, y, kLookupBtnW, kJoinRoomBtnH});
    }
    y += kInputH + kJoinRoomGap;

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
        float info_x = card_x + kCardPadX + kJoinRoomAvatarSize + kJoinRoomGap;
        float info_w = inner_w - kCardPadX * 2.0f - kJoinRoomAvatarSize - kJoinRoomGap;
        float info_h = kJoinRoomAvatarSize; // clamp avatar + info side by side

        // Estimate topic height.
        float topic_h = 0.0f;
        if (!preview_.topic.empty())
        {
            topic_h = std::min(kJoinRoomTopicMaxH, 18.0f * 3.0f); // up to 3 lines
        }

        float card_h = kCardPadY * 2.0f + info_h +
                       (topic_h > 0 ? kSmallGap + topic_h : 0.0f);
        preview_card_rect_ = {card_x, card_y, inner_w, card_h};
        y += card_h + kJoinRoomGap;
    }

    // Button row anchored to the bottom or below the card.
    // Cancel always visible; Join only in Preview state.
    float btn_row_y = std::max(y, bounds.y + bounds.h - kJoinRoomPadY - kJoinRoomBtnH);
    float btn_x = x + inner_w; // right-align

    if (join_btn_ && join_btn_->visible())
    {
        btn_x -= kJoinRoomBtnW;
        join_btn_->arrange(ctx, {btn_x, btn_row_y, kJoinRoomBtnW, kJoinRoomBtnH});
        btn_x -= kSmallGap;
    }

    if (cancel_btn_)
    {
        btn_x -= kJoinRoomBtnW;
        cancel_btn_->arrange(ctx, {btn_x, btn_row_y, kJoinRoomBtnW, kJoinRoomBtnH});
    }
}

void JoinRoomView::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Background.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Title.
    float x = bounds_.x + kJoinRoomPadX;
    float y = bounds_.y + kJoinRoomPadY;
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
    y += kTitleH + kJoinRoomGap;

    // Alias field background (the NativeTextField overlays this).
    if (alias_field_visible() && !alias_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(alias_field_rect_, kJoinRoomRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(alias_field_rect_, kJoinRoomRadius, pal.border,
                                       kJoinRoomBorderW);
    }
    y += kInputH + kJoinRoomGap;

    // Status label.
    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->set_colour(state_ == State::Error
                                     ? std::optional<tk::Color>(pal.destructive)
                                     : std::nullopt);
        status_lbl_->paint(ctx);
        y += kStatusH + kSmallGap;
    }

    // Preview card.
    if (state_ == State::Preview && !preview_card_rect_.empty())
    {
        // Card border.
        ctx.canvas.fill_rounded_rect(preview_card_rect_, kJoinRoomRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(preview_card_rect_, kJoinRoomRadius, pal.border,
                                       kJoinRoomBorderW);

        float cx = preview_card_rect_.x + kCardPadX;
        float cy = preview_card_rect_.y + kCardPadY;
        float iw = preview_card_rect_.w - kCardPadX * 2.0f;

        // Avatar.
        tk::Point av_centre{cx + kJoinRoomAvatarSize * 0.5f, cy + kJoinRoomAvatarSize * 0.5f};
        const tk::Image* av_img = nullptr;
        if (avatar_provider_ && !preview_.avatar_url.empty())
        {
            av_img = avatar_provider_(preview_.avatar_url);
        }

        {
            std::string_view disp = preview_.name.empty()
                                        ? std::string_view("#")
                                        : std::string_view(preview_.name);
            draw_avatar(ctx.canvas, av_img, av_centre, kJoinRoomAvatarSize, disp,
                        pal.accent, tk::Color{255, 255, 255, 255});
        }

        // Info column to the right of the avatar.
        float info_x = cx + kJoinRoomAvatarSize + kJoinRoomGap;
        float info_y = cy;
        float info_w = iw - kJoinRoomAvatarSize - kJoinRoomGap;

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
                preview_card_rect_.y + kCardPadY + kJoinRoomAvatarSize + kSmallGap;
            float topic_w = preview_card_rect_.w - kCardPadX * 2.0f;
            if (!topic_layout_)
            {
                tk::TextStyle ts;
                ts.role = tk::FontRole::Body;
                ts.halign = tk::TextHAlign::Leading;
                ts.wrap = true;
                ts.max_width = topic_w;
                ts.max_height = kJoinRoomTopicMaxH;
                ts.trim = tk::TextTrim::Ellipsis;
                if (!topic_spans_.empty())
                    topic_layout_ = ctx.factory.build_rich_text(topic_spans_, ts);
                else
                    topic_layout_ = ctx.factory.build_text(preview_.topic, ts);
            }
            if (topic_layout_)
            {
                ctx.canvas.draw_text(*topic_layout_, {cx, topic_y}, pal.text_secondary);
                topic_rect_ = {cx, topic_y,
                               topic_layout_->measure().w, topic_layout_->measure().h};
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

bool JoinRoomView::on_pointer_down(tk::Point local)
{
    if (state_ != State::Preview || !topic_layout_)
        return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(topic_rect_, w))
        return false;

    const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
    std::string url = topic_layout_->link_at(ll);
    if (url.empty())
        return false;

    press_link_url_ = std::move(url);
    return true;
}

void JoinRoomView::on_pointer_up(tk::Point local, bool inside_self)
{
    if (press_link_url_.empty())
        return;

    std::string url = std::move(press_link_url_);
    press_link_url_.clear();

    if (!inside_self || !on_link_clicked || !topic_layout_)
        return;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(topic_rect_, w))
        return;

    const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
    if (topic_layout_->link_at(ll) == url)
        on_link_clicked(std::move(url));
}

bool JoinRoomView::on_pointer_move(tk::Point local)
{
    if (state_ != State::Preview || !topic_layout_)
        return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    std::string new_link_url;
    if (rect_contains(topic_rect_, w))
    {
        const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
        new_link_url = topic_layout_->link_at(ll);
    }
    const bool changed = (new_link_url != hover_link_url_);
    if (changed)
    {
        hover_link_url_ = new_link_url;
        if (on_link_hovered) on_link_hovered(hover_link_url_);
    }
    return changed;
}

void JoinRoomView::on_pointer_leave()
{
    if (!hover_link_url_.empty())
    {
        hover_link_url_.clear();
        if (on_link_hovered) on_link_hovered({});
    }
}

} // namespace tesseract::views
