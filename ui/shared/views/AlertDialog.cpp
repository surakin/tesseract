#include "AlertDialog.h"

#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

AlertDialog::AlertDialog()
{
    primary_btn_ = add_child(
        std::make_unique<tk::Button>("", std::function<void()>{},
                                     tk::Button::Variant::Primary));
    secondary_btn_ = add_child(
        std::make_unique<tk::Button>("", std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    primary_btn_->set_on_click([this]() {
        auto cb = std::move(primary_cb_);
        close();
        if (cb) cb();
    });
    secondary_btn_->set_on_click([this]() {
        auto cb = std::move(secondary_cb_);
        close();
        if (cb) cb();
    });

    set_visible(false);
}

void AlertDialog::open(Options opts,
                        std::function<void()> primary_cb,
                        std::function<void()> secondary_cb)
{
    const bool was_open = open_;

    opts_         = std::move(opts);
    primary_cb_   = std::move(primary_cb);
    secondary_cb_ = std::move(secondary_cb);
    open_         = true;
    set_visible(true);

    primary_btn_->set_label(opts_.primary_label);
    primary_btn_->set_variant(tk::Button::Variant::Primary);

    const bool has_secondary = !opts_.secondary_label.empty();
    secondary_btn_->set_label(has_secondary ? opts_.secondary_label : "");
    secondary_btn_->set_visible(has_secondary);

    title_layout_.reset();
    body_layout_.reset();

    if (!was_open && on_layout_changed) on_layout_changed();
}

void AlertDialog::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    primary_cb_   = nullptr;
    secondary_cb_ = nullptr;
    if (was_open && on_layout_changed) on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size AlertDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire host surface
}

void AlertDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    backdrop_rect_ = bounds;

    const float body_estimate = opts_.body.empty() ? 0.0f : 64.0f;
    const float card_h = kCardPad * 2 + kTitleH +
                         (opts_.body.empty() ? 0.0f : (kTitleGap + body_estimate)) +
                         kBodyGap + kBtnH;

    const float card_w     = std::min(kCardW, bounds.w);
    const float clamped_h  = std::min(card_h, bounds.h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f,
                  bounds.y + (bounds.h - clamped_h) * 0.5f,
                  card_w, clamped_h};

    const float btns_y    = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    const float btn_w_min = 88.0f;

    tk::Size primary_sz = primary_btn_ ? primary_btn_->measure(lc, {-1.0f, kBtnH})
                                       : tk::Size{btn_w_min, kBtnH};
    const float primary_w  = std::max(primary_sz.w, btn_w_min);
    const float primary_x  = card_rect_.x + card_rect_.w - kCardPad - primary_w;

    if (primary_btn_)
        primary_btn_->arrange(lc, {primary_x, btns_y, primary_w, kBtnH});

    if (secondary_btn_ && secondary_btn_->visible())
    {
        tk::Size secondary_sz = secondary_btn_->measure(lc, {-1.0f, kBtnH});
        const float secondary_w = std::max(secondary_sz.w, btn_w_min);
        const float secondary_x = primary_x - kBtnGap - secondary_w;
        secondary_btn_->arrange(lc, {secondary_x, btns_y, secondary_w, kBtnH});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void AlertDialog::paint(tk::PaintCtx& ctx)
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

    // 4. Body (wraps; clipped to card).
    if (!opts_.body.empty())
    {
        y += kTitleGap;
        if (!body_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.wrap      = true;
            st.max_width = text_w;
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
    if (secondary_btn_ && secondary_btn_->visible()) secondary_btn_->paint(ctx);
    if (primary_btn_) primary_btn_->paint(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool AlertDialog::on_pointer_down(tk::Point /*local*/)
{
    // Consume all pointer events while open — the dialog is not backdrop-
    // dismissible; the user must press one of the action buttons.
    return open_;
}

} // namespace tesseract::views
