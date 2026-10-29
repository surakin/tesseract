#include "views/IdentityChangeBanner.h"

#include "banner_style.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kIdentityButtonH = 28.0f;

// "Alice (@alice:example.org)", or just the user id when no name is known.
std::string identity_who(const tesseract::IdentityWarning& w)
{
    if (w.display_name.empty() || w.display_name == w.user_id)
        return w.user_id;
    return tk::trf(tk::tr("{0} ({1})"), {w.display_name, w.user_id});
}
} // namespace

IdentityChangeBanner::IdentityChangeBanner()
{
    auto label = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    label->set_tone(tk::Label::Tone::Notice);
    label->set_halign(tk::TextHAlign::Leading);
    label->set_trim(tk::TextTrim::Ellipsis);
    label_ = add_child(std::move(label));

    auto action = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                                tk::Button::Variant::Primary);
    action->set_on_click(
        [this]
        {
            if (!warnings_.empty() && on_resolve)
                on_resolve(warnings_.front());
        });
    action->set_min_size({60.0f, kIdentityButtonH});
    action_ = add_child(std::move(action));

    set_visible(false);
}

void IdentityChangeBanner::set_warnings(std::vector<tesseract::IdentityWarning> warnings)
{
    warnings_ = std::move(warnings);
    set_visible(!warnings_.empty());
    apply_();
}

std::string IdentityChangeBanner::label_text() const
{
    if (warnings_.empty())
        return {};
    const auto& w = warnings_.front();
    if (w.kind == tesseract::IdentityWarning::Kind::VerificationBroken)
        return tk::trf(tk::tr("{0}'s verified digital identity was reset."), {identity_who(w)});
    return tk::trf(tk::tr("{0}'s digital identity was reset."), {identity_who(w)});
}

void IdentityChangeBanner::apply_()
{
    if (label_) label_->set_text(label_text());
    if (action_ && !warnings_.empty())
    {
        action_->set_label(warnings_.front().kind ==
                                   tesseract::IdentityWarning::Kind::VerificationBroken
                               ? tk::tr("Withdraw verification")
                               : tk::tr("OK"));
    }
}

tk::Size IdentityChangeBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, warnings_.empty() ? 0.0f : kHeight};
}

void IdentityChangeBanner::arrange(tk::LayoutCtx& ctx, tk::Rect b)
{
    bounds_ = b;
    float right = b.x + b.w - kBannerPadX;

    const auto sz = action_->measure(ctx, {220.0f, kIdentityButtonH});
    tk::Rect ar{right - sz.w, b.y + (b.h - kIdentityButtonH) * 0.5f, sz.w, kIdentityButtonH};
    action_->arrange(ctx, ar);
    right = ar.x - kBannerGap;

    label_->arrange(ctx, {b.x + kBannerPadX, b.y + (b.h - 20.0f) * 0.5f,
                          std::max(0.0f, right - (b.x + kBannerPadX)), 20.0f});
}

void IdentityChangeBanner::paint_before_children(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.notice_bg);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
                         ctx.theme.palette.notice_border);
}

} // namespace tesseract::views
