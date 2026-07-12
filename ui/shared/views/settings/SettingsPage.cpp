#include "SettingsPage.h"

#include "SettingsGroup.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Outer page inset + spacing between adjacent groups/widgets. Matches the
// padding the per-section code used to redeclare in its own anonymous
// namespace before this base class existed.
constexpr float kPagePadX = 24.0f;
constexpr float kPagePadY = 16.0f;
constexpr float kGroupSpacing = 20.0f;

// Lightweight scrollbar thumb geometry, matching the style DevicesSection
// used before this scroll behavior moved up into the shared base class.
constexpr float kScrollbarTrackW = 4.0f;
constexpr float kScrollbarInset = 2.0f;
constexpr float kScrollbarMinLen = 20.0f;

} // namespace

SettingsPage::SettingsPage()
{
    set_padding(tk::Edges{kPagePadY, kPagePadX, kPagePadY, kPagePadX});
    set_spacing(kGroupSpacing);
}

SettingsGroup* SettingsPage::add_group(std::string header)
{
    return add_child(std::make_unique<SettingsGroup>(std::move(header)));
}

// ---------------------------------------------------------------------------
// Scroll support: override the base VBox layout so a page can host more
// content than fits in the viewport SideTabView gives it. The base VBox
// layout knows how to stack children; we ask it to lay out at the content's
// natural height (which may be taller than the viewport), then clip painting
// to the page's bounds. Wheel events adjust scroll_y_ within [0, max].
// ---------------------------------------------------------------------------

tk::Size SettingsPage::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever the host gives us; SettingsPage is the inner widget of a
    // SideTabView, which always feeds us a fixed viewport.
    return constraints;
}

void SettingsPage::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    // Probe the natural content height by asking the base measure with an
    // unbounded vertical constraint.
    tk::Size natural = VBox::measure(ctx, {bounds.w, 1.0e6f});
    content_height_ = natural.h;

    // Clamp scroll into range now that we know the content height.
    const float max_scroll = std::max(0.0f, content_height_ - bounds.h);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);

    // Arrange the base at the natural height, shifted up by scroll_y_. The
    // base will set each child's bounds_ in world coords accordingly, so
    // off-screen widgets just sit outside the page's bounds — paint clips
    // them away and pointer dispatch's contains_world filters them.
    const float laid_h = std::max(bounds.h, content_height_);
    VBox::arrange(ctx, {bounds.x, bounds.y - scroll_y_, bounds.w, laid_h});

    // VBox::arrange wrote `this->bounds_` to the laid-out rect (which is
    // content_height_ tall, not the viewport). Restore the viewport bounds so
    // dispatch hit-testing, paint clipping, and the overflow check in
    // on_wheel/paint operate on the visible area.
    bounds_ = bounds;
}

void SettingsPage::paint(tk::PaintCtx& ctx)
{
    // The host's wheel handler triggers request_repaint() but not a full
    // relayout, so the child bounds the previous arrange() set are stale
    // when scroll_y_ has just changed. Re-arrange here off the PaintCtx's
    // CanvasFactory + Theme so the widgets land at the current scroll offset
    // and pointer hit-testing stays consistent.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    const tk::Rect viewport = bounds_;
    const float laid_h = std::max(viewport.h, content_height_);
    VBox::arrange(lc, {viewport.x, viewport.y - scroll_y_, viewport.w, laid_h});
    // VBox::arrange overwrote our viewport bounds — restore.
    bounds_ = viewport;

    ctx.canvas.push_clip_rect(bounds_);
    VBox::paint(ctx);

    // Lightweight scrollbar overlay when there is content overflow.
    if (content_height_ > bounds_.h)
    {
        const auto& pal = ctx.theme.palette;
        const float track_x = bounds_.x + bounds_.w - kScrollbarTrackW - kScrollbarInset;
        const float track_h = bounds_.h;
        const float track_y = bounds_.y;
        const float thumb_h =
            std::max(kScrollbarMinLen, track_h * (track_h / content_height_));
        const float max_scroll = content_height_ - bounds_.h;
        const float frac = max_scroll > 0 ? (scroll_y_ / max_scroll) : 0.0f;
        const float thumb_y = track_y + frac * (track_h - thumb_h);
        ctx.canvas.fill_rounded_rect({track_x, thumb_y, kScrollbarTrackW, thumb_h},
                                     kScrollbarTrackW * 0.5f,
                                     pal.text_muted.with_alpha(120));
    }

    ctx.canvas.pop_clip();
}

bool SettingsPage::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    if (content_height_ <= bounds_.h)
        return false;
    const float max_scroll = content_height_ - bounds_.h;
    const float prev = scroll_y_;
    // Toolkit convention (see host_qt.cpp Surface::wheelEvent): positive dy =
    // scroll content down. Same direction as tk::ListView::on_wheel.
    scroll_y_ = std::clamp(scroll_y_ + dy, 0.0f, max_scroll);
    return scroll_y_ != prev;
}

} // namespace tesseract::views
