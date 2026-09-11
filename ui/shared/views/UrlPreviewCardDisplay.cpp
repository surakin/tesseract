#include "UrlPreviewCardDisplay.h"

#include "MessageListView.h" // MessageRowData / UrlPreviewData (full defs)
#include "media_utils.h"     // fit_media

#include "tesseract/visual.h" // kMaxInlineImageWidth/Height

#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>

namespace tesseract::views
{

namespace
{
// URL preview card dimensions. `kUrlPreviewCardH` and `kPreviewCardGap` also
// drive the Adapter's row-height math via stack_height(); the rest is
// paint-only. These are the LEGACY (homeserver-fetched) card's dimensions
// only — bundled (MSC4095) cards size their image like an inline timeline
// image instead (see kMaxInlineImageWidth/Height below).
constexpr float kUrlPreviewCardH = 72.0f;
constexpr float kPreviewCardGap  = 4.0f; // vertical space between stacked cards
constexpr float kPreviewCardW    = 280.0f;
constexpr float kPreviewThumbSide = 56.0f;
constexpr float kPreviewCardPad  = 10.0f;

// Bundled (MSC4095) card image cap — the same size an inline image in the
// timeline is capped at, so a sender-bundled preview reads like a normal
// shared image rather than a tiny thumbnail.
constexpr float kBundledImageMaxW =
    static_cast<float>(tesseract::visual::kMaxInlineImageWidth);
constexpr float kBundledImageMaxH =
    static_cast<float>(tesseract::visual::kMaxInlineImageHeight);

// Third line shown under a card's title/description: the canonical og:url,
// else the matched URL, else (legacy path) the row's first URL. Shared by
// stack_height() (measure) and paint_cards() (paint) so both agree on
// exactly which lines will be drawn/measured.
std::string third_line_for(const UrlPreviewData& p, const std::string& first_url)
{
    return !p.canonical_url.empty() ? p.canonical_url
          : !p.matched_url.empty()  ? p.matched_url
                                    : first_url;
}
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

float UrlPreviewCardDisplay::stack_height(const MessageRowData& row,
                                          tk::CanvasFactory& factory,
                                          float col_w) const
{
    auto cards = cards_for(row);
    if (cards.empty())
    {
        return 0.0f;
    }

    float total = 0.0f;
    if (row.bundled_previews_present)
    {
        const float max_w = std::min(col_w, kBundledImageMaxW);
        for (const auto* p : cards)
        {
            const std::string third_line = third_line_for(*p, row.first_url);
            total += bundled_card_size_(*p, third_line, factory, max_w).h;
        }
    }
    else
    {
        total = static_cast<float>(cards.size()) * kUrlPreviewCardH;
    }
    total += static_cast<float>(cards.size() - 1) * kPreviewCardGap;
    return total;
}

void UrlPreviewCardDisplay::paint_cards(const MessageRowData& row,
                                        tk::PaintCtx& ctx, float x, float y,
                                        float col_w)
{
    float cy = y;
    for (const auto* p : cards_for(row))
    {
        const std::string third_line = third_line_for(*p, row.first_url);
        const float h = row.bundled_previews_present
                            ? paint_one_bundled_(third_line, third_line, *p,
                                                 ctx, x, cy, col_w)
                            : paint_one_legacy_(third_line, third_line, *p,
                                               ctx, x, cy, col_w);
        cy += h + kPreviewCardGap;
    }
}

float UrlPreviewCardDisplay::paint_one_legacy_(const std::string& target_url,
                                               const std::string& third_line,
                                               const UrlPreviewData& p,
                                               tk::PaintCtx& ctx, float x,
                                               float y, float col_w)
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
            // Contain-fit within the fixed thumbnail slot instead of
            // stretching to fill it, so a non-square image isn't distorted.
            tk::Size fitted = fit_media(static_cast<float>(img->width()),
                                       static_cast<float>(img->height()),
                                       kPreviewThumbSide, kPreviewThumbSide);
            float dx = thumb.x + (thumb.w - fitted.w) * 0.5f;
            float dy = thumb.y + (thumb.h - fitted.h) * 0.5f;
            ctx.canvas.draw_image(*img, {dx, dy, fitted.w, fitted.h});
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

    return kUrlPreviewCardH;
}

namespace
{
// Height of the stacked title/description/third-line text block a bundled
// card draws below its image (or in place of it, if there's no image).
// Shared by bundled_card_size_() (measure) and paint_one_bundled_() (paint)
// so both agree on exactly how tall the text block is. Only measures height;
// callers already know the width (`text_w`, the card's full text column).
float bundled_text_block_height(const UrlPreviewData& p,
                                const std::string& third_line,
                                tk::CanvasFactory& factory, float text_w)
{
    float h = 0.0f;
    if (!p.title.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::UiSemibold;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        if (auto lo = factory.build_text(p.title, st))
        {
            h += lo->measure().h + 2.0f;
        }
    }
    if (!p.description.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        if (auto lo = factory.build_text(p.description, st))
        {
            h += lo->measure().h + 2.0f;
        }
    }
    if (!third_line.empty())
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        if (auto lo = factory.build_text(third_line, st))
        {
            h += lo->measure().h;
        }
    }
    return h;
}
} // namespace

tk::Size UrlPreviewCardDisplay::bundled_card_size_(const UrlPreviewData& p,
                                                   const std::string& third_line,
                                                   tk::CanvasFactory& factory,
                                                   float max_w) const
{
    const float text_w = std::max(0.0f, max_w - 2.0f * kPreviewCardPad);

    float image_h = 0.0f;
    const std::string img_key =
        p.image_source ? p.image_source->fetch_token() : std::string{};
    const bool has_image = !img_key.empty() && image_provider_;
    if (has_image)
    {
        const tk::Image* img = image_provider_(img_key);
        tk::Size fitted =
            img ? fit_media(static_cast<float>(img->width()),
                            static_cast<float>(img->height()), max_w,
                            kBundledImageMaxH)
                : fit_media(static_cast<float>(p.image_w),
                            static_cast<float>(p.image_h), max_w,
                            kBundledImageMaxH);
        image_h = fitted.h;
    }

    const float text_h = bundled_text_block_height(p, third_line, factory, text_w);

    float total = kPreviewCardPad; // top padding
    if (has_image)
    {
        total += image_h;
    }
    if (has_image && text_h > 0.0f)
    {
        total += kPreviewCardPad; // gap between image and text
    }
    total += text_h;
    total += kPreviewCardPad; // bottom padding

    return {max_w, total};
}

float UrlPreviewCardDisplay::paint_one_bundled_(const std::string& target_url,
                                                const std::string& third_line,
                                                const UrlPreviewData& p,
                                                tk::PaintCtx& ctx, float x,
                                                float y, float col_w)
{
    const float max_w = std::min(col_w, kBundledImageMaxW);
    const tk::Size size = bundled_card_size_(p, third_line, ctx.factory, max_w);
    tk::Rect card{x, y, size.w, size.h};

    ctx.canvas.fill_rounded_rect(card, 8.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card, 8.0f, ctx.theme.palette.border, 1.0f);

    // Record world-coord rect for click-to-open hit-test.
    card_geom_.push_back({target_url, card});

    const float text_w = std::max(0.0f, max_w - 2.0f * kPreviewCardPad);
    float cursor_y = y + kPreviewCardPad;

    const std::string img_key =
        p.image_source ? p.image_source->fetch_token() : std::string{};
    if (!img_key.empty() && image_provider_)
    {
        const tk::Image* img = image_provider_(img_key);
        tk::Size fitted =
            img ? fit_media(static_cast<float>(img->width()),
                            static_cast<float>(img->height()), max_w,
                            kBundledImageMaxH)
                : fit_media(static_cast<float>(p.image_w),
                            static_cast<float>(p.image_h), max_w,
                            kBundledImageMaxH);
        tk::Rect img_rect{x + (max_w - fitted.w) * 0.5f, cursor_y, fitted.w,
                          fitted.h};
        if (img)
        {
            ctx.canvas.draw_image(*img, img_rect);
        }
        else
        {
            ctx.canvas.fill_rounded_rect(img_rect, 4.0f, ctx.theme.palette.border);
        }
        cursor_y += fitted.h;
        if (bundled_text_block_height(p, third_line, ctx.factory, text_w) > 0.0f)
        {
            cursor_y += kPreviewCardPad;
        }
    }

    const float text_x = x + kPreviewCardPad;
    float text_y = cursor_y;

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

    return size.h;
}

} // namespace tesseract::views
