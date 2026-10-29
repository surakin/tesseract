#include "views/RoomReplacedBanner.h"

#include "banner_style.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kReplacedButtonH = 28.0f;
} // namespace

RoomReplacedBanner::RoomReplacedBanner()
{
    auto label = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    label->set_tone(tk::Label::Tone::Notice);
    label->set_halign(tk::TextHAlign::Leading);
    label->set_trim(tk::TextTrim::Ellipsis);
    label_ = add_child(std::move(label));

    auto action = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                                tk::Button::Variant::Primary);
    action->set_on_click([this] { if (on_open) on_open(); });
    action->set_min_size({90.0f, kReplacedButtonH});
    action_ = add_child(std::move(action));

    apply_();
}

void RoomReplacedBanner::set_successor_joined(bool joined)
{
    if (successor_joined_ == joined) return;
    successor_joined_ = joined;
    apply_();
}

std::string RoomReplacedBanner::label_text() const
{
    return tk::tr("This room has been replaced and is no longer active.");
}

std::string RoomReplacedBanner::action_text() const
{
    return successor_joined_ ? tk::tr("Go to the new room") : tk::tr("Join the new room");
}

void RoomReplacedBanner::apply_()
{
    if (label_) label_->set_text(label_text());
    if (action_) action_->set_label(action_text());
}

tk::Size RoomReplacedBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kHeight};
}

void RoomReplacedBanner::arrange(tk::LayoutCtx& ctx, tk::Rect b)
{
    bounds_ = b;
    float right = b.x + b.w - kBannerPadX;

    const auto sz = action_->measure(ctx, {240.0f, kReplacedButtonH});
    tk::Rect ar{right - sz.w, b.y + (b.h - kReplacedButtonH) * 0.5f, sz.w, kReplacedButtonH};
    action_->arrange(ctx, ar);
    right = ar.x - kBannerGap;

    label_->arrange(ctx, {b.x + kBannerPadX, b.y + (b.h - 20.0f) * 0.5f,
                          std::max(0.0f, right - (b.x + kBannerPadX)), 20.0f});
}

void RoomReplacedBanner::paint_before_children(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.notice_bg);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
                         ctx.theme.palette.notice_border);
}

} // namespace tesseract::views
