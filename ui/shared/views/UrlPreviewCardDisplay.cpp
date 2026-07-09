#include "UrlPreviewCardDisplay.h"

#include "MessageListView.h" // MessageRowData / UrlPreviewData (full defs)

#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>

namespace tesseract::views
{

namespace
{
// URL preview card dimensions. Mirror of the constants the Adapter uses for
// height accounting in MessageListView.cpp (kept in sync — see kUrlPreviewCardH /
// kPreviewCardGapTop there).
constexpr float kUrlPreviewCardH    = 72.0f;
constexpr float kPreviewCardW    = 280.0f;
constexpr float kPreviewThumbSide = 56.0f;
constexpr float kPreviewCardPad  = 10.0f;
} // namespace

bool UrlPreviewCardDisplay::has_preview(const MessageRowData& row) const
{
    if (row.first_url.empty() || !provider_)
    {
        return false;
    }
    const auto* p = provider_(row.first_url);
    return p && p->has_content();
}

void UrlPreviewCardDisplay::paint_card(const MessageRowData& m,
                                       const UrlPreviewData& p,
                                       tk::PaintCtx& ctx, float x, float y,
                                       float col_w)
{
    float card_w = std::min(col_w, kPreviewCardW);
    tk::Rect card{x, y, card_w, kUrlPreviewCardH};

    ctx.canvas.fill_rounded_rect(card, 8.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card, 8.0f, ctx.theme.palette.border, 1.0f);

    // Record world-coord rect for click-to-open hit-test.
    card_geom_[m.event_id] = {m.first_url, card};

    float thumb_right = 0.0f;
    if (!p.image_mxc.empty() && image_provider_)
    {
        const tk::Image* img = image_provider_(p.image_mxc);
        float tx = x + kPreviewCardPad;
        float ty = y + (kUrlPreviewCardH - kPreviewThumbSide) * 0.5f;
        tk::Rect thumb{tx, ty, kPreviewThumbSide, kPreviewThumbSide};
        if (img)
        {
            ctx.canvas.draw_image(*img, thumb);
        }
        else
        {
            ctx.canvas.fill_rounded_rect(thumb, 4.0f, ctx.theme.palette.border);
        }
        thumb_right = tx + kPreviewThumbSide + kPreviewCardPad;
    }
    else
    {
        thumb_right = x + kPreviewCardPad;
    }

    float text_x = thumb_right;
    float text_w = std::max(0.0f, card.x + card.w - text_x - kPreviewCardPad);

    float text_y = y + kPreviewCardPad;

    if (!p.title.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::UiSemibold;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lo      = ctx.factory.build_text(p.title, st);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {text_x, text_y},
                                 ctx.theme.palette.text_primary);
            text_y += lo->measure().h + 2.0f;
        }
    }
    if (!p.description.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lo      = ctx.factory.build_text(p.description, st);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {text_x, text_y},
                                 ctx.theme.palette.text_secondary);
            text_y += lo->measure().h + 2.0f;
        }
    }
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lo      = ctx.factory.build_text(m.first_url, st);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {text_x, text_y},
                                 ctx.theme.palette.text_muted);
        }
    }
}

} // namespace tesseract::views
