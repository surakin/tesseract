#include "KnockStatusCard.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

// Estimate text-row heights (used in arrange before we have a real layout).
// Prefixed kKnockCard* — this file is compiled into a Unity build alongside
// InviteCard.cpp/JoinRoomView.cpp, whose anonymous-namespace constants would
// otherwise collide with unprefixed names like kKnockCardNameH/kKnockCardReasonH.
constexpr float kKnockCardNameH   = 24.0f; // 18 pt bold — Title role
constexpr float kKnockCardTopicH  = 18.0f; // 13 pt — Body role
constexpr float kKnockCardStatusH = 18.0f; // 12 pt — Small role
constexpr float kKnockCardReasonH = 18.0f; // 12 pt — Small role

} // namespace

// ── constructor ───────────────────────────────────────────────────────────

KnockStatusCard::KnockStatusCard()
{
    auto cancel = tk::create_widget<tk::Button>(this,
        tk::tr("Cancel Request"), std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_on_click([this]() { if (on_cancel) on_cancel(); });
    cancel_btn_ = add_child(std::move(cancel));

    set_visible(false);
}

// ── public API ────────────────────────────────────────────────────────────

void KnockStatusCard::set_knock(const tesseract::KnockedRoomInfo& info,
                                ImageProvider provider)
{
    knock_          = info;
    image_provider_ = std::move(provider);
    reset_layouts();
    set_visible(true);
}

void KnockStatusCard::clear()
{
    knock_.reset();
    image_provider_ = nullptr;
    reset_layouts();
    set_visible(false);
}

void KnockStatusCard::reset_layouts()
{
    name_layout_.reset();
    topic_layout_.reset();
    status_layout_.reset();
    reason_layout_.reset();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size KnockStatusCard::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill the available space; the content block is centred inside.
    return constraints;
}

void KnockStatusCard::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    if (!knock_.has_value())
    {
        return;
    }

    // Estimate total content height for vertical centering — mirrors paint's.
    float content_h = kAvatarD + kGap + kKnockCardNameH + kGap * 0.5f + kKnockCardStatusH + kGap;
    if (!knock_->room_topic.empty())
    {
        content_h += kKnockCardTopicH + kGap * 0.5f;
    }
    if (!knock_->reason.empty())
    {
        content_h += kKnockCardReasonH + kGap * 0.5f;
    }
    content_h += kBtnH + kPadY;

    const float cx = bounds.x + (bounds.w - kContentW) * 0.5f;
    float       cy = bounds.y + std::max(0.0f, (bounds.h - content_h) * 0.5f);

    // Skip past avatar + text rows to reach the button.
    cy += kAvatarD + kGap + kKnockCardNameH + kGap * 0.5f + kKnockCardStatusH + kGap;
    if (!knock_->room_topic.empty())
    {
        cy += kKnockCardTopicH + kGap * 0.5f;
    }
    if (!knock_->reason.empty())
    {
        cy += kKnockCardReasonH + kGap * 0.5f;
    }

    if (cancel_btn_)
    {
        cancel_btn_->arrange(lc, {cx, cy, kContentW, kBtnH});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void KnockStatusCard::paint_before_children(tk::PaintCtx& ctx)
{
    if (!knock_.has_value())
    {
        return;
    }

    const auto& pal = ctx.theme.palette;
    auto&       cv  = ctx.canvas;

    // Background.
    cv.fill_rect(bounds_, pal.bg);

    // ── Content block ────────────────────────────────────────────────────

    float content_h = kAvatarD + kGap + kKnockCardNameH + kGap * 0.5f + kKnockCardStatusH + kGap;
    if (!knock_->room_topic.empty())
    {
        content_h += kKnockCardTopicH + kGap * 0.5f;
    }
    if (!knock_->reason.empty())
    {
        content_h += kKnockCardReasonH + kGap * 0.5f;
    }
    content_h += kBtnH + kPadY;

    const float cx = bounds_.x + (bounds_.w - kContentW) * 0.5f;
    float       cy = bounds_.y + std::max(0.0f, (bounds_.h - content_h) * 0.5f);

    // ── Avatar ───────────────────────────────────────────────────────────

    const tk::Point av_centre{cx + kContentW * 0.5f, cy + kAvatarD * 0.5f};
    const tk::Image* av_img = nullptr;
    if (image_provider_ && !knock_->room_avatar_url.empty())
    {
        av_img = image_provider_(knock_->room_avatar_url);
    }

    {
        const std::string& fallback_name =
            knock_->room_name.empty() ? knock_->room_id : knock_->room_name;
        std::string_view disp = fallback_name.empty()
                                    ? std::string_view("#")
                                    : std::string_view(fallback_name);
        draw_avatar(cv, av_img, av_centre, kAvatarD, disp, pal.accent,
                    tk::Color{255, 255, 255, 255});
    }

    cy += kAvatarD + kGap;

    // ── Room name ────────────────────────────────────────────────────────

    const std::string& name_str =
        knock_->room_name.empty() ? knock_->room_id : knock_->room_name;

    if (!name_layout_)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Title;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = kContentW;
        name_layout_ = ctx.factory.build_text(name_str, ts);
    }
    if (name_layout_)
    {
        const tk::Size sz = name_layout_->measure();
        const float tx    = cx + (kContentW - sz.w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, cy}, pal.text_primary);
        cy += sz.h + kGap * 0.5f;
    }
    else
    {
        cy += kKnockCardNameH + kGap * 0.5f;
    }

    // ── Status line ("Request pending") ─────────────────────────────────

    if (!status_layout_)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Body;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = kContentW;
        status_layout_ = ctx.factory.build_text(tk::tr("Request pending"), ts);
    }
    if (status_layout_)
    {
        const tk::Size sz = status_layout_->measure();
        const float tx    = cx + (kContentW - sz.w) * 0.5f;
        cv.draw_text(*status_layout_, {tx, cy}, pal.text_muted);
        cy += sz.h + kGap;
    }
    else
    {
        cy += kKnockCardStatusH + kGap;
    }

    // ── Topic line (shown when the room has one) ────────────────────────

    if (!knock_->room_topic.empty())
    {
        if (!topic_layout_)
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Small;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = kContentW;
            topic_layout_ = ctx.factory.build_text(knock_->room_topic, ts);
        }
        if (topic_layout_)
        {
            const tk::Size sz = topic_layout_->measure();
            const float tx    = cx + (kContentW - sz.w) * 0.5f;
            cv.draw_text(*topic_layout_, {tx, cy}, pal.text_muted);
            cy += sz.h + kGap;
        }
        else
        {
            cy += kKnockCardTopicH + kGap;
        }
    }

    // ── Reason line (shown when a reason was supplied while knocking) ───

    if (!knock_->reason.empty())
    {
        if (!reason_layout_)
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Small;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = kContentW;
            reason_layout_ = ctx.factory.build_text(
                tk::trf(tk::tr("Reason: {0}"), {knock_->reason}), ts);
        }
        if (reason_layout_)
        {
            const tk::Size sz = reason_layout_->measure();
            const float tx    = cx + (kContentW - sz.w) * 0.5f;
            cv.draw_text(*reason_layout_, {tx, cy}, pal.text_muted);
            cy += sz.h + kGap;
        }
        else
        {
            cy += kKnockCardReasonH + kGap;
        }
    }
}

} // namespace tesseract::views
