#include "UrlPreviewCardDisplay.h"

#include "MessageListView.h" // MessageRowData / UrlPreviewData (full defs)

#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>

namespace tesseract::views
{

namespace
{
// URL preview card dimensions. `kUrlPreviewCardH` and `kPreviewCardGap` also
// drive the Adapter's row-height math via stack_height(); the rest is
// paint-only.
constexpr float kUrlPreviewCardH = 72.0f;
constexpr float kPreviewCardGap  = 4.0f; // vertical space between stacked cards
constexpr float kPreviewCardW    = 280.0f;
constexpr float kPreviewThumbSide = 56.0f;
constexpr float kPreviewCardPad  = 10.0f;
} // namespace

std::vector<const UrlPreviewData*>
UrlPreviewCardDisplay::cards_for(const MessageRowData& row) const
{
    std::vector<const UrlPreviewData*> out;

    if (row.bundled_previews_present)
    {
        // MSC4095: the sender controls previews for this message. Render each
        // bundled entry that carries content; an entry with only a matched_url
        // is resolved through the homeserver like the legacy path.
        for (const auto& bp : row.bundled_previews)
        {
            if (bp.has_content())
            {
                out.push_back(&bp);
            }
            else if (!bp.matched_url.empty() && provider_)
            {
                if (const auto* hp = provider_(bp.matched_url);
                    hp && hp->has_content())
                {
                    out.push_back(hp);
                }
            }
        }
        return out;
    }

    // Legacy path: one homeserver-fetched card keyed by the row's first URL.
    if (!row.first_url.empty() && provider_)
    {
        if (const auto* p = provider_(row.first_url); p && p->has_content())
        {
            out.push_back(p);
        }
    }
    return out;
}

float UrlPreviewCardDisplay::stack_height(const MessageRowData& row) const
{
    const int n = static_cast<int>(cards_for(row).size());
    if (n == 0)
    {
        return 0.0f;
    }
    return n * kUrlPreviewCardH + (n - 1) * kPreviewCardGap;
}

void UrlPreviewCardDisplay::paint_cards(const MessageRowData& row,
                                        tk::PaintCtx& ctx, float x, float y,
                                        float col_w)
{
    float cy = y;
    for (const auto* p : cards_for(row))
    {
        // Click target / third line: canonical og:url, else the matched URL,
        // else (legacy path) the row's first URL.
        const std::string url = !p->canonical_url.empty() ? p->canonical_url
                                : !p->matched_url.empty() ? p->matched_url
                                                          : row.first_url;
        paint_one_(url, url, *p, ctx, x, cy, col_w);
        cy += kUrlPreviewCardH + kPreviewCardGap;
    }
}

void UrlPreviewCardDisplay::paint_one_(const std::string& target_url,
                                       const std::string& third_line,
                                       const UrlPreviewData& p,
                                       tk::PaintCtx& ctx, float x, float y,
                                       float col_w)
{
    float card_w = std::min(col_w, kPreviewCardW);
    tk::Rect card{x, y, card_w, kUrlPreviewCardH};

    ctx.canvas.fill_rounded_rect(card, 8.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card, 8.0f, ctx.theme.palette.border, 1.0f);

    // Record world-coord rect for click-to-open hit-test.
    card_geom_.push_back({target_url, card});

    float thumb_right = 0.0f;
    const std::string img_key =
        p.image_source ? p.image_source->fetch_token() : std::string{};
    if (!img_key.empty() && image_provider_)
    {
        const tk::Image* img = image_provider_(img_key);
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
    if (!third_line.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lo      = ctx.factory.build_text(third_line, st);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {text_x, text_y},
                                 ctx.theme.palette.text_muted);
        }
    }
}

} // namespace tesseract::views
