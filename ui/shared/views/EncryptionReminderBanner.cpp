#include "views/EncryptionReminderBanner.h"

#include "banner_style.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kButtonH = 28.0f;
} // namespace

EncryptionReminderBanner::EncryptionReminderBanner()
{
    auto label = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    label->set_colour(kBannerLabelText);
    label->set_halign(tk::TextHAlign::Leading);
    label->set_trim(tk::TextTrim::Ellipsis);
    label_ = add_child(std::move(label));

    auto action = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                                tk::Button::Variant::Primary);
    action->set_on_click([this] { if (on_open) on_open(); });
    action->set_min_size({90.0f, kButtonH});
    action_ = add_child(std::move(action));

    auto dismiss = tk::create_widget<tk::Button>(this, "\xe2\x9c\x95", std::function<void()>{},
                                                 tk::Button::Variant::Subtle);
    dismiss->set_on_click([this] { if (on_dismiss) on_dismiss(); });
    dismiss->set_min_size({kBannerDismissSide, kBannerDismissSide});
    dismiss->set_accessible_name(tk::tr("Remind me later"));
    dismiss_ = add_child(std::move(dismiss));

    apply_kind_();
}

void EncryptionReminderBanner::set_kind(Kind k)
{
    if (kind_ == k) return;
    kind_ = k;
    apply_kind_();
}

std::string EncryptionReminderBanner::label_text() const
{
    return kind_ == Kind::SetupNeeded
               ? tk::tr("Your messages aren't backed up yet.")
               : tk::tr("Your encrypted messages are locked on this device.");
}

void EncryptionReminderBanner::apply_kind_()
{
    if (label_) label_->set_text(label_text());
    if (action_)
        action_->set_label(kind_ == Kind::SetupNeeded ? tk::tr("Set up recovery")
                                                      : tk::tr("Unlock"));
}

tk::Size EncryptionReminderBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kHeight};
}

void EncryptionReminderBanner::arrange(tk::LayoutCtx& ctx, tk::Rect b)
{
    bounds_ = b;
    float right = b.x + b.w - kBannerPadX;

    tk::Rect dr{right - kBannerDismissSide, b.y + (b.h - kBannerDismissSide) * 0.5f,
                kBannerDismissSide, kBannerDismissSide};
    dismiss_->arrange(ctx, dr);
    right = dr.x - kBannerGap;

    const auto sz = action_->measure(ctx, {200.0f, kButtonH});
    tk::Rect ar{right - sz.w, b.y + (b.h - kButtonH) * 0.5f, sz.w, kButtonH};
    action_->arrange(ctx, ar);
    right = ar.x - kBannerGap;

    label_->arrange(ctx, {b.x + kBannerPadX, b.y + (b.h - 20.0f) * 0.5f,
                          std::max(0.0f, right - (b.x + kBannerPadX)), 20.0f});
}

void EncryptionReminderBanner::paint_before_children(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, kBannerBg);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
                         kBannerBorder);
}

} // namespace tesseract::views
