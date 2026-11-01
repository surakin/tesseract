#include "BusyDialog.h"

#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

BusyDialog::BusyDialog()
{
    progress_ = add_child(tk::create_widget<tk::ProgressBar>(this));
    progress_->set_indeterminate();
    set_visible(false);
}

void BusyDialog::open(std::string title, std::string body)
{
    const bool was_open = open_;

    title_ = std::move(title);
    body_ = std::move(body);
    open_ = true;
    set_visible(true);

    title_layout_.reset();
    body_layout_.reset();

    if (!was_open && on_layout_changed)
    {
        on_layout_changed();
    }
}

void BusyDialog::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (was_open && on_layout_changed)
    {
        on_layout_changed();
    }
}

tk::Size BusyDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire host surface
}

void BusyDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    backdrop_rect_ = bounds;

    const float card_h =
        kCardPad * 2 + kTitleH + (body_.empty() ? 0.0f : (kTitleGap + kBodyH)) + kBarGap + kBarH;
    const float card_w = std::min(kCardW, bounds.w);
    const float clamped_h = std::min(card_h, bounds.h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f, bounds.y + (bounds.h - clamped_h) * 0.5f,
                  card_w, clamped_h};

    if (progress_)
    {
        progress_->arrange(lc,
                           {card_rect_.x + kCardPad, card_rect_.y + card_rect_.h - kCardPad - kBarH,
                            card_rect_.w - kCardPad * 2.0f, kBarH});
    }
}

void BusyDialog::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }

    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);

    const float text_x = card_rect_.x + kCardPad;
    const float text_w = card_rect_.w - kCardPad * 2.0f;
    float y = card_rect_.y + kCardPad;

    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        title_layout_ = ctx.factory.build_text(title_, st);
    }
    if (title_layout_)
    {
        cv.draw_text(*title_layout_, {text_x, y}, pal.text_primary);
        y += std::max(title_layout_->measure().h, kTitleH);
    }

    if (!body_.empty())
    {
        y += kTitleGap;
        if (!body_layout_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            st.wrap = true;
            st.max_width = text_w;
            st.max_height = kBodyH;
            body_layout_ = ctx.factory.build_text(body_, st);
        }
        if (body_layout_)
        {
            cv.draw_text(*body_layout_, {text_x, y}, pal.text_secondary);
        }
    }
}

bool BusyDialog::on_pointer_down(tk::Point /*local*/)
{
    return open_;
}

bool BusyDialog::on_key_down(const tk::KeyEvent&)
{
    return open_;
}

bool BusyDialog::dispatch_key_down(const tk::KeyEvent& ev)
{
    if (!open_)
    {
        return tk::Widget::dispatch_key_down(ev);
    }
    return true;
}

} // namespace tesseract::views
