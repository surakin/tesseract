#include "ImagePackTileGridBase.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cctype>

namespace tesseract::views
{

std::string suggest_pack_shortcode_from_filename(const std::string& filename)
{
    std::string stem = filename;
    if (const auto dot = stem.find_last_of('.'); dot != std::string::npos && dot != 0)
        stem = stem.substr(0, dot);

    std::string base;
    base.reserve(stem.size());
    for (unsigned char c : stem)
    {
        if (std::isalnum(c))
            base += static_cast<char>(std::tolower(c));
        else if (c == ' ' || c == '_' || c == '-')
            base += '_';
    }
    return base.empty() ? "sticker" : base;
}

std::string dedupe_pack_shortcode(const std::vector<StagedPackImage>& siblings,
                                  const std::string& base)
{
    auto taken = [&](const std::string& candidate)
    {
        return std::any_of(siblings.begin(), siblings.end(),
                           [&](const StagedPackImage& s)
                           { return s.shortcode == candidate; });
    };
    if (!taken(base))
        return base;
    for (int n = 2; n <= 10000; ++n)
    {
        std::string candidate = base + "_" + std::to_string(n);
        if (!taken(candidate))
            return candidate;
    }
    return base + "_" + std::to_string(siblings.size() + 1);
}

std::vector<tk::Rect> ImagePackTileGridBase::layout_tile_row_(
    float width, std::size_t tile_count, float y_start) const
{
    std::vector<tk::Rect> out;
    const float grid_w = std::max(0.0f, width - 2.0f * kTilePad);
    const int cols = std::max(
        1, static_cast<int>((grid_w + kTileSpacing) / (kTileSize + kTileSpacing)));

    out.reserve(tile_count);
    for (std::size_t t = 0; t < tile_count; ++t)
    {
        const int row = static_cast<int>(t / static_cast<std::size_t>(cols));
        const int col = static_cast<int>(t % static_cast<std::size_t>(cols));
        const float tx = kTilePad + col * (kTileSize + kTileSpacing);
        const float ty = y_start + kTilePad + row * (kTileSize + kTileSpacing);
        out.push_back({tx, ty, kTileSize, kTileSize});
    }
    return out;
}

bool ImagePackTileGridBase::hit_remove_chip_(tk::Point local, float y_content,
                                             const tk::Rect& image_rect) const
{
    const float ccx = image_rect.x + image_rect.w - kRemoveChipR;
    const float ccy = image_rect.y + kRemoveChipR;
    const float dx = local.x - ccx;
    const float dy = y_content - ccy;
    const float tol = kRemoveChipR + 4.0f;
    return (dx * dx + dy * dy) <= (tol * tol);
}

void ImagePackTileGridBase::paint_tile_shared_(tk::PaintCtx& ctx,
                                               const StagedPackImage& img,
                                               const tk::Rect& cell_local,
                                               tk::Point origin,
                                               bool hovered_remove,
                                               bool is_editing) const
{
    const tk::Rect cell{origin.x + cell_local.x, origin.y + cell_local.y, cell_local.w,
                        cell_local.h};
    const tk::Rect image_rect{cell.x, cell.y, cell.w, kImageH};
    const auto& pal = ctx.theme.palette;

    ctx.canvas.fill_rounded_rect(image_rect, 6.0f, pal.subtle_hover);

    const tk::Image* bmp =
        img.local_preview ? img.local_preview.get()
                          : (image_provider_ && !img.existing_url.empty()
                                 ? image_provider_(img.existing_url)
                                 : nullptr);
    if (bmp)
    {
        const float iw = static_cast<float>(bmp->width());
        const float ih = static_cast<float>(bmp->height());
        if (iw > 0 && ih > 0)
        {
            const float s  = std::min(image_rect.w / iw, image_rect.h / ih);
            const float dw = iw * s;
            const float dh = ih * s;
            const tk::Rect dst{image_rect.x + (image_rect.w - dw) * 0.5f,
                              image_rect.y + (image_rect.h - dh) * 0.5f, dw, dh};
            ctx.canvas.draw_image(*bmp, dst);
        }
    }

    if (!is_editing)
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = cell.w - 4.0f;
        const std::string text =
            img.shortcode.empty() ? tk::tr("(no shortcode)") : img.shortcode;
        auto lay = ctx.factory.build_text(text, st);
        if (lay)
        {
            const tk::Size sz = lay->measure();
            ctx.canvas.draw_text(
                *lay,
                {cell.x + (cell.w - sz.w) * 0.5f,
                 cell.y + kImageH + (kLabelH - sz.h) * 0.5f},
                pal.text_muted);
        }
    }

    if (hovered_remove)
    {
        const float cx = image_rect.x + image_rect.w - kRemoveChipR;
        const float cy = image_rect.y + kRemoveChipR;
        const tk::Rect chip{cx - kRemoveChipR, cy - kRemoveChipR, kRemoveChipR * 2.0f,
                            kRemoveChipR * 2.0f};
        ctx.canvas.fill_rounded_rect(chip, kRemoveChipR,
                                    tk::Color::rgba(40, 40, 40, 220));
        remove_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg, chip, 12.0f,
                         tk::Color::rgb(0xffffff));
    }
}

void ImagePackTileGridBase::paint_hint_tile_shared_(tk::PaintCtx& ctx,
                                                    const tk::Rect& cell_local,
                                                    tk::Point origin) const
{
    const tk::Rect cell{origin.x + cell_local.x, origin.y + cell_local.y, cell_local.w,
                        cell_local.h};
    const tk::Rect image_rect{cell.x, cell.y, cell.w, kImageH};
    const auto& pal = ctx.theme.palette;
    ctx.canvas.stroke_rounded_rect(image_rect, 6.0f, pal.border);

    tk::TextStyle st;
    st.role      = tk::FontRole::Small;
    st.wrap      = true;
    st.max_width = image_rect.w - 8.0f;
    auto lay = ctx.factory.build_text(tk::tr("Drop image or paste"), st);
    if (lay)
    {
        const tk::Size sz = lay->measure();
        ctx.canvas.draw_text(*lay,
                             {image_rect.x + (image_rect.w - sz.w) * 0.5f,
                              image_rect.y + (image_rect.h - sz.h) * 0.5f},
                             pal.text_muted);
    }
}

} // namespace tesseract::views
