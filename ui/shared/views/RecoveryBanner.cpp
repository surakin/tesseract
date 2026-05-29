#include "RecoveryBanner.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

constexpr float kHeight = 48.0f;
constexpr float kPadX = 12.0f;
constexpr float kPadY = 8.0f;
constexpr float kKeyFieldWidth = 220.0f;
constexpr float kVerifyMinWidth = 80.0f;
constexpr float kDismissSide = 24.0f;
constexpr float kGap = 8.0f;

// Warm-yellow banner tint, regardless of theme — the banner is always a
// notice, not a decorative element.
const tk::Color kBannerBg = tk::Color::rgb(0xFFF4D6);
const tk::Color kBannerBorder = tk::Color::rgb(0xE0C97A);
const tk::Color kLabelText = tk::Color::rgb(0x5C4500);

} // namespace

RecoveryBanner::RecoveryBanner()
{
    auto label = std::make_unique<tk::Label>("", tk::FontRole::Body);
    label->set_colour(kLabelText);
    label->set_halign(tk::TextHAlign::Leading);
    label->set_trim(tk::TextTrim::Ellipsis);
    label_ = add_child(std::move(label));

    auto verify = std::make_unique<tk::Button>(
        tk::tr("Verify"), std::function<void()>{}, tk::Button::Variant::Primary);
    verify->set_on_click(
        [this]
        {
            if (on_verify)
            {
                on_verify(current_key_);
            }
        });
    verify->set_min_size({kVerifyMinWidth, 28.0f});
    verify_ = add_child(std::move(verify));

    auto dismiss = std::make_unique<tk::Button>("✕", std::function<void()>{},
                                                tk::Button::Variant::Subtle);
    dismiss->set_on_click(
        [this]
        {
            if (on_dismiss)
            {
                on_dismiss();
            }
        });
    dismiss->set_min_size({kDismissSide, kDismissSide});
    dismiss_ = add_child(std::move(dismiss));

    apply_state();
}

void RecoveryBanner::set_state(State s)
{
    state_ = s;
    if (s != State::Failed)
    {
        failure_msg_.clear();
    }
    if (s != State::Importing)
    {
        imported_keys_ = 0;
    }
    apply_state();
}

void RecoveryBanner::set_failure_message(std::string msg)
{
    failure_msg_ = std::move(msg);
    if (label_)
    {
        label_->set_text(label_text());
    }
}

void RecoveryBanner::set_import_progress(std::uint64_t imported)
{
    imported_keys_ = imported;
    if (label_ && state_ == State::Importing)
    {
        label_->set_text(label_text());
    }
}

bool RecoveryBanner::recovery_key_field_visible() const
{
    return state_ == State::Form || state_ == State::Failed;
}

tk::Rect RecoveryBanner::recovery_key_field_rect() const
{
    return recovery_key_field_visible() ? key_field_rect_ : tk::Rect{};
}

std::string RecoveryBanner::label_text() const
{
    switch (state_)
    {
    case State::Form:
        return tk::tr("Verify this device:");
    case State::Verifying:
        return tk::tr("Verifying\xe2\x80\xa6");
    case State::Importing:
        return imported_keys_ > 0
                   ? tk::trf(tk::tr("Importing keys from backup\xe2\x80\xa6 {0} imported."),
                             {std::to_string(imported_keys_)})
                   : tk::tr("Downloading historical keys\xe2\x80\xa6");
    case State::Failed:
        return failure_msg_.empty()
                   ? tk::tr("Recovery failed.")
                   : tk::trf(tk::tr("Recovery failed: {0}"), {failure_msg_});
    }
    return {};
}

void RecoveryBanner::apply_state()
{
    if (label_)
    {
        label_->set_text(label_text());
    }
    bool show_field = recovery_key_field_visible();
    if (verify_)
    {
        verify_->set_visible(show_field);
    }
    // Dismiss is always visible — the user can hide the banner regardless.
}

tk::Size RecoveryBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kHeight};
}

void RecoveryBanner::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Dismiss anchored to the right.
    dismiss_rect_ = {bounds.x + bounds.w - kPadX - kDismissSide,
                     bounds.y + (bounds.h - kDismissSide) * 0.5f, kDismissSide,
                     kDismissSide};

    float right_cursor = dismiss_rect_.x - kGap;

    bool show_field = recovery_key_field_visible();
    if (show_field)
    {
        // Verify button next.
        tk::Size verify_size{kVerifyMinWidth, bounds.h - kPadY * 2};
        if (verify_)
        {
            verify_size =
                verify_->measure(ctx, {kVerifyMinWidth, bounds.h - kPadY * 2});
        }
        verify_rect_ = {right_cursor - verify_size.w,
                        bounds.y + (bounds.h - verify_size.h) * 0.5f,
                        verify_size.w, verify_size.h};
        right_cursor = verify_rect_.x - kGap;

        // Key field overlay rect.
        key_field_rect_ = {right_cursor - kKeyFieldWidth,
                           bounds.y + (bounds.h - 28.0f) * 0.5f, kKeyFieldWidth,
                           28.0f};
        right_cursor = key_field_rect_.x - kGap;
    }
    else
    {
        verify_rect_ = {};
        key_field_rect_ = {};
    }

    // Label fills the remaining left segment.
    label_rect_ = {bounds.x + kPadX, bounds.y + (bounds.h - 20.0f) * 0.5f,
                   std::max(0.0f, right_cursor - (bounds.x + kPadX)), 20.0f};

    if (label_)
    {
        label_->arrange(ctx, label_rect_);
    }
    if (verify_)
    {
        verify_->arrange(ctx, verify_rect_);
    }
    if (dismiss_)
    {
        dismiss_->arrange(ctx, dismiss_rect_);
    }
}

void RecoveryBanner::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, kBannerBg);
    // 1 px bottom border to separate from the message list.
    tk::Rect border{bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f};
    ctx.canvas.fill_rect(border, kBannerBorder);

    if (label_)
    {
        label_->paint(ctx);
    }

    // Key-field affordance behind the host's NativeTextField overlay —
    // even when the overlay is hidden, the placeholder helps the user
    // see where their input will land.
    if (recovery_key_field_visible() && !key_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(key_field_rect_, 4.0f,
                                     ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(key_field_rect_, 4.0f,
                                       ctx.theme.palette.border, 1.0f);
    }

    if (verify_ && verify_->visible())
    {
        verify_->paint(ctx);
    }
    if (dismiss_)
    {
        dismiss_->paint(ctx);
    }
}

} // namespace tesseract::views
