#include "views/ShortcodePopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include <algorithm>

namespace tesseract::views
{

void ShortcodePopup::set_suggestions(std::vector<ShortcodeSuggestion> s)
{
    suggestions_ = std::move(s);
    // Preselect the first match so Tab/Enter accepts the top result without
    // the user having to press Down first. Stays -1 when the list is empty.
    selected_index_ = suggestions_.empty() ? -1 : 0;
    reset_transient_state_();
}

void ShortcodePopup::paint_row(tk::PaintCtx& ctx, const tk::Rect& row,
                               size_t index, bool /*selected*/,
                               bool /*hovered*/)
{
    const auto& pal = ctx.theme.palette;
    const auto& s   = suggestions_[index];

    // 28×28 glyph cell left-aligned with 4px margin
    tk::Rect cell{row.x + 4.0f, row.y + (kRowHeight - 28.0f) * 0.5f, 28.0f,
                  28.0f};
    if (!s.glyph.empty())
    {
        tk::TextStyle st{};
        st.role =
            tk::FontRole::Title; // 15pt — same as EmojiPicker, fits 28px cell
        auto layout = ctx.factory.build_text(s.glyph, st);
        if (layout)
        {
            tk::Size sz = layout->measure();
            tk::Point origin{cell.x + (cell.w - sz.w) * 0.5f,
                             cell.y + (cell.h - sz.h) * 0.5f};
            ctx.canvas.draw_text(*layout, origin, pal.text_primary);
        }
    }
    else if (image_provider_)
    {
        const tk::Image* img = image_provider_(s.emoticon.url);
        if (img)
        {
            float iw = static_cast<float>(img->width());
            float ih = static_cast<float>(img->height());
            float sc = std::min(cell.w / iw, cell.h / ih);
            float dw = iw * sc, dh = ih * sc;
            ctx.canvas.draw_image(*img, {cell.x + (cell.w - dw) * 0.5f,
                                         cell.y + (cell.h - dh) * 0.5f, dw, dh});
        }
        else
        {
            ctx.canvas.fill_rect(cell, pal.chrome_bg);
        }
    }
    else
    {
        ctx.canvas.fill_rect(cell, pal.chrome_bg);
    }

    // Shortcode label — use Top alignment; ly already computes the
    // vertical centre.  Center+unbounded-height would render at ly+4096.
    std::string label = ":" + s.shortcode + ":";
    tk::TextStyle tst{};
    tst.role = tk::FontRole::Body;
    tst.halign = tk::TextHAlign::Leading;
    tst.valign = tk::TextVAlign::Top;
    auto tl = ctx.factory.build_text(label, tst);
    if (tl)
    {
        tk::Size tsz = tl->measure();
        float lx = cell.x + cell.w + 8.0f;
        float ly = row.y + (kRowHeight - tsz.h) * 0.5f;
        ctx.canvas.draw_text(*tl, tk::Point{lx, ly}, pal.text_primary);
    }
}

} // namespace tesseract::views
