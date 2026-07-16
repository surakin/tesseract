#include "ConfirmDialog.h"

#include "media_utils.h" // rect_contains

#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

ConfirmDialog::ConfirmDialog()
{
    // Two child buttons; their roles are reassigned every open() so the
    // labels and variant track the caller's Options.
    confirm_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    confirm_btn_->set_on_click([this]() {
        // Capture the callback locally before close() so re-entrant open()
        // calls inside on_confirm_ see the new state, not the previous one.
        auto cb = std::move(on_confirm_);
        close();
        if (cb) cb();
    });
    cancel_btn_->set_on_click([this]() {
        close();
    });

    // Closed-by-default overlay. Same idiom as RoomInfoPanel: tying widget
    // visibility to the open state lets the widget tree's hit-test walk
    // past us entirely when idle so the underlying buttons stay clickable.
    set_visible(false);
}

void ConfirmDialog::open(Options opts, std::function<void()> on_confirm)
{
    const bool was_open = open_;

    opts_       = std::move(opts);
    on_confirm_ = std::move(on_confirm);
    open_       = true;
    set_visible(true);
    press_backdrop_ = false;

    // Push the labels + variant onto the buttons so they paint correctly
    // before the first arrange(). Layouts depend on theme/factory and are
    // rebuilt lazily inside paint().
    confirm_btn_->set_label(opts_.confirm_label);
    confirm_btn_->set_variant(opts_.destructive
                                  ? tk::Button::Variant::Destructive
                                  : tk::Button::Variant::Primary);
    cancel_btn_->set_label(opts_.cancel_label);

    title_layout_.reset();
    body_layout_.reset();

    // Tell the shell to re-query rect accessors — this is what makes the
    // compose textarea + room-search NativeTextField overlays hide while
    // the dialog is up. Skip the fire when we were already open so back-
    // to-back open() calls don't churn the shell layout.
    if (!was_open && on_layout_changed) on_layout_changed();
}

void ConfirmDialog::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    on_confirm_ = nullptr;
    press_backdrop_ = false;
    if (was_open && on_layout_changed) on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ConfirmDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void ConfirmDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    backdrop_rect_ = bounds;

    // Card height is derived from the laid-out body (one or more lines).
    // We don't have a layout yet here — paint() builds them — so size the
    // card from a conservative estimate and let paint() draw inside it.
    // 22 px title + 12 gap + up to 80 px body + 20 gap + 36 buttons + 20 pad
    // ≈ 190 px when there's body text.
    const float body_estimate = opts_.body.empty() ? 0.0f : 64.0f;
    const float card_h = kCardPad * 2 + kTitleH +
                         (opts_.body.empty() ? 0.0f : (kTitleGap + body_estimate)) +
                         kBodyGap + kBtnH;

    const float card_w = std::min(kCardW, bounds.w);
    const float clamped_h = std::min(card_h, bounds.h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f,
                  bounds.y + (bounds.h - clamped_h) * 0.5f,
                  card_w, clamped_h};

    // Buttons sit at the bottom-right of the card, cancel left of confirm.
    const float btns_y = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    const float btn_w_min = 88.0f;

    // Measure both labels so the buttons size to fit, with a floor of 88 px.
    tk::Size confirm_sz = confirm_btn_ ? confirm_btn_->measure(lc, {-1.0f, kBtnH})
                                        : tk::Size{btn_w_min, kBtnH};
    tk::Size cancel_sz  = cancel_btn_ ? cancel_btn_->measure(lc, {-1.0f, kBtnH})
                                       : tk::Size{btn_w_min, kBtnH};
    const float confirm_w = std::max(confirm_sz.w, btn_w_min);
    const float cancel_w  = std::max(cancel_sz.w,  btn_w_min);

    const float confirm_x =
        card_rect_.x + card_rect_.w - kCardPad - confirm_w;
    const float cancel_x = confirm_x - kBtnGap - cancel_w;

    if (cancel_btn_)
        cancel_btn_->arrange(lc, {cancel_x, btns_y, cancel_w, kBtnH});
    if (confirm_btn_)
        confirm_btn_->arrange(lc, {confirm_x, btns_y, confirm_w, kBtnH});
}

// ── paint ─────────────────────────────────────────────────────────────────

void ConfirmDialog::paint(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // 1. Semi-transparent backdrop.
    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});

    // 2. Card background + 1 px border.
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);

    const float text_x = card_rect_.x + kCardPad;
    const float text_w = card_rect_.w - kCardPad * 2.0f;
    float       y      = card_rect_.y + kCardPad;

    // 3. Title.
    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        title_layout_ = ctx.factory.build_text(opts_.title, st);
    }
    if (title_layout_)
    {
        cv.draw_text(*title_layout_, {text_x, y}, pal.text_primary);
        y += std::max(title_layout_->measure().h, kTitleH);
    }

    // 4. Body (wraps to multiple lines; clipped to card to stay tidy).
    if (!opts_.body.empty())
    {
        y += kTitleGap;
        if (!body_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.wrap      = true;
            st.max_width = text_w;
            // Don't let the body push the buttons off-card.
            const float max_body_h =
                card_rect_.y + card_rect_.h - kCardPad - kBtnH - kBodyGap - y;
            st.max_height = std::max(0.0f, max_body_h);
            body_layout_  = ctx.factory.build_text(opts_.body, st);
        }
        if (body_layout_)
        {
            cv.push_clip_rect({card_rect_.x, card_rect_.y,
                               card_rect_.w,
                               card_rect_.h - kCardPad - kBtnH - kBodyGap});
            cv.draw_text(*body_layout_, {text_x, y}, pal.text_secondary);
            cv.pop_clip();
        }
    }

    // 5. Buttons.
    if (cancel_btn_)  cancel_btn_->paint(ctx);
    if (confirm_btn_) confirm_btn_->paint(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool ConfirmDialog::on_pointer_down(tk::Point local)
{
    if (!open_) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    // Clicks on the card itself fall through to child buttons.
    if (rect_contains(card_rect_, w))
    {
        return false;
    }

    // Backdrop click: consume and remember so on_pointer_up can dismiss.
    press_backdrop_ = true;
    return true;
}

void ConfirmDialog::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_) return;
    press_backdrop_ = false;
    if (!inside_self) return;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(card_rect_, w))
    {
        close();
    }
}

} // namespace tesseract::views
