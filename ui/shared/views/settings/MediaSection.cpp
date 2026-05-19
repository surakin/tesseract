#include "MediaSection.h"

#include "tesseract/settings.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPadX = 24.0f;
} // namespace

MediaSection::MediaSection()
{
    const auto& s = tesseract::Settings::instance();

    prefetch_cb_ = add_child(std::make_unique<tk::CheckButton>(
        "Pre-load full images while scrolling", s.prefetch_full_media));
    prefetch_cb_->on_change = [this](bool v)
    {
        if (on_prefetch_changed)
            on_prefetch_changed(v);
    };
}

void MediaSection::set_prefetch_checked(bool enabled)
{
    prefetch_cb_->set_checked(enabled);
}

tk::Size MediaSection::measure(tk::LayoutCtx& ctx, tk::Size constraints)
{
    const float w       = constraints.w > 0 ? constraints.w : 0;
    const float inner_w = std::max(0.0f, w - kPadX * 2);
    return {w, prefetch_cb_->measure(ctx, {inner_w, 0}).h};
}

void MediaSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    const float inner_w = std::max(0.0f, bounds.w - kPadX * 2);
    const float h = prefetch_cb_->measure(ctx, {inner_w, 0}).h;
    prefetch_cb_->arrange(ctx, {bounds.x + kPadX, bounds.y, inner_w, h});
}

void MediaSection::paint(tk::PaintCtx& ctx)
{
    prefetch_cb_->paint(ctx);
}

} // namespace tesseract::views
