#include "Toast.h"

#include "tk/theme.h"

#include <utility>

namespace tesseract::views
{

Toast::Toast()
{
    // Closed-by-default; same idiom as ConfirmDialog/RoomInfoPanel.
    set_visible(false);
}

void Toast::show(std::string message)
{
    message_ = std::move(message);
    message_layout_.reset();
    set_visible(true);
}

void Toast::hide()
{
    set_visible(false);
}

tk::Size Toast::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // overlays the full parent bounds
}

void Toast::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

void Toast::paint(tk::PaintCtx& ctx)
{
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    if (!message_layout_ && !message_.empty())
    {
        tk::TextStyle st{};
        st.role   = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Leading;
        message_layout_ = ctx.factory.build_text(message_, st);
    }
    if (!message_layout_) return;

    const tk::Size text_sz = message_layout_->measure();
    const float pill_w = text_sz.w + 2.0f * kPadX;
    const float pill_h = text_sz.h + 2.0f * kPadY;
    const float pill_x = bounds_.x + (bounds_.w - pill_w) * 0.5f;
    const float pill_y = bounds_.y + bounds_.h - kBottomMargin - pill_h;

    const tk::Rect pill_rect{pill_x, pill_y, pill_w, pill_h};
    cv.fill_rounded_rect(pill_rect, kRadius, pal.text_primary);
    cv.draw_text(*message_layout_, {pill_x + kPadX, pill_y + kPadY}, pal.bg);
}

} // namespace tesseract::views
