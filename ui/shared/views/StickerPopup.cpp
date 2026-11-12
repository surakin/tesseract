#include "views/StickerPopup.h"
#include "tk/canvas.h"

namespace tesseract::views
{

void StickerPopup::set_results(std::vector<tesseract::ImagePackImage> results)
{
    results_ = std::move(results);
    reset_items_(results_.size());
}

void StickerPopup::set_status(std::string message)
{
    results_.clear();
    ImageStripPopup::set_status(std::move(message));
}

void StickerPopup::paint_item(tk::PaintCtx& ctx, int i, tk::Rect cell)
{
    const auto& s = results_[static_cast<std::size_t>(i)];
    const tk::Image* img = image_provider_ ? image_provider_(s) : nullptr;
    if (!img)
    {
        return;
    }
    draw_image_fit(ctx, *img, cell);
    if (ctx.anim_damage)
    {
        ctx.anim_damage->note_image(
            cache_key_resolver ? cache_key_resolver(s.url) : tk::CacheKey::media(s.url), cell);
    }
}

void StickerPopup::accept_item(int i)
{
    if (on_accepted)
    {
        on_accepted(results_[static_cast<std::size_t>(i)]);
    }
}

} // namespace tesseract::views
