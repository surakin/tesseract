#include "views/GifPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"

namespace tesseract::views
{

void GifPopup::set_results(std::vector<tesseract::GifResult> results)
{
    results_ = std::move(results);
    reset_items_(results_.size());
}

void GifPopup::set_status(std::string message)
{
    results_.clear();
    ImageStripPopup::set_status(std::move(message));
}

void GifPopup::paint_item(tk::PaintCtx& ctx, int i, tk::Rect cell)
{
    const auto& r = results_[static_cast<std::size_t>(i)];
    const tk::Image* img = image_provider_ ? image_provider_(r) : nullptr;
    if (!img)
    {
        return;
    }
    draw_image_fit(ctx, *img, cell);
    if (ctx.anim_damage)
    {
        ctx.anim_damage->note_image(tk::CacheKey::media(r.strip_url), cell);
    }
}

void GifPopup::accept_item(int i)
{
    if (on_accepted)
    {
        on_accepted(results_[static_cast<std::size_t>(i)]);
    }
}

void GifPopup::paint_footer(tk::PaintCtx& ctx)
{
    // Attribution — required by the provider's ToS. Bottom-right, secondary.
    tk::TextStyle st{};
    st.role = tk::FontRole::Small;
    st.halign = tk::TextHAlign::Leading;
    st.valign = tk::TextVAlign::Top;
    if (auto tl = ctx.factory.build_text("Powered by KLIPY", st))
    {
        tk::Size tsz = tl->measure();
        float lx = bounds_.x + bounds_.w - kPad - tsz.w;
        float ly = bounds_.y + bounds_.h - kAttribH + (kAttribH - tsz.h) * 0.5f;
        ctx.canvas.draw_text(*tl, {lx, ly}, ctx.theme.palette.text_secondary);
    }
}

} // namespace tesseract::views
