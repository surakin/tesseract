#include "MediaOverlayBase.h"
#include "icons.h"

#include "tk/svg.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>

namespace tesseract::views
{

static constexpr float kCloseBtnS = 36.0f; // close / save button square size
static constexpr float kMediaOverlayBtnIconPx = 20.0f; // logical icon size inside a button

// Full-screen chrome auto-hides after this long without pointer activity.
static constexpr auto kChromeHideAfter = std::chrono::milliseconds(2500);

// Fixed, backdrop-tuned fill states for the chrome buttons — the scrim is
// always near-black regardless of the app's light/dark theme, so these
// bypass tk::Button's default theme-driven fill (see Button::FillOverride).
static constexpr tk::Color kCloseFillRest      = tk::Color::rgba(255, 255, 255, 30);
static constexpr tk::Color kCloseFillHover     = tk::Color::rgba(255, 255, 255, 80);
static constexpr tk::Color kCloseFillPressed   = tk::Color::rgba(255, 255, 255, 115);
static constexpr tk::Color kDarkPillFillRest    = tk::Color::rgba(0, 0, 0, 160);
static constexpr tk::Color kDarkPillFillHover   = tk::Color::rgba(60, 60, 60, 185);
static constexpr tk::Color kDarkPillFillPressed = tk::Color::rgba(95, 95, 95, 200);

// ── construction ─────────────────────────────────────────────────────────

MediaOverlayBase::MediaOverlayBase()
{
    auto close = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                               tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] { dismiss_(); });
    close_btn_->set_fill_override(tk::Button::FillOverride{
        kCloseFillRest, kCloseFillHover, kCloseFillPressed});

    auto save = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    save_btn_ = add_child(std::move(save));
    save_btn_->set_on_click([this] { if (on_save) fire_save_(); });
    save_btn_->set_fill_override(tk::Button::FillOverride{
        kDarkPillFillRest, kDarkPillFillHover, kDarkPillFillPressed});

    auto copy = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    copy_btn_ = add_child(std::move(copy));
    copy_btn_->set_on_click([this] { if (on_copy && wants_copy_button_()) fire_copy_(); });
    copy_btn_->set_fill_override(tk::Button::FillOverride{
        kDarkPillFillRest, kDarkPillFillHover, kDarkPillFillPressed});

    auto fs = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    fullscreen_btn_ = add_child(std::move(fs));
    fullscreen_btn_->set_on_click([this] { toggle_fullscreen_(); });
    fullscreen_btn_->set_fill_override(tk::Button::FillOverride{
        kDarkPillFillRest, kDarkPillFillHover, kDarkPillFillPressed});
}

// ── repaint / full-screen ────────────────────────────────────────────────

void MediaOverlayBase::set_repaint_requester(std::function<void()> fn)
{
    repaint_requester_ = std::move(fn);
}

void MediaOverlayBase::request_repaint_()
{
    if (repaint_requester_)
    {
        repaint_requester_();
    }
}

void MediaOverlayBase::note_activity_()
{
    chrome_visible_ = true;
    last_activity_ = std::chrono::steady_clock::now();
}

void MediaOverlayBase::toggle_fullscreen_()
{
    fullscreen_ = !fullscreen_;
    fullscreen_icon_.reset(); // glyph swaps between ⤢ (enter) and ⤡ (exit)
    note_activity_();
    if (on_request_fullscreen)
    {
        on_request_fullscreen(fullscreen_);
    }
    on_fullscreen_changed_(fullscreen_);
    request_repaint_();
}

bool MediaOverlayBase::any_chrome_hovered_() const
{
    return (close_btn_ && close_btn_->hovered()) ||
           (save_btn_ && save_btn_->hovered()) ||
           (copy_btn_ && copy_btn_->hovered()) ||
           (fullscreen_btn_ && fullscreen_btn_->hovered());
}

bool MediaOverlayBase::on_pointer_move(tk::Point /*local*/)
{
    const bool was_hidden = fullscreen_ && !chrome_visible_;
    note_activity_();
    if (was_hidden)
        request_repaint_();
    return was_hidden;
}

// ── layout ───────────────────────────────────────────────────────────────

void MediaOverlayBase::refresh_chrome_autohide_()
{
    // Full-screen chrome auto-hide: drop the cluster once the pointer has been
    // idle, unless it is parked on one of the buttons.
    if (fullscreen_ && chrome_visible_ && !any_chrome_hovered_() &&
        std::chrono::steady_clock::now() - last_activity_ >= kChromeHideAfter)
    {
        chrome_visible_ = false;
    }
}

void MediaOverlayBase::layout_chrome_(tk::LayoutCtx& lc, tk::Rect b)
{
    refresh_chrome_autohide_();

    close_btn_->arrange(lc, {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f,
                             kCloseBtnS, kCloseBtnS});
    save_btn_->arrange(lc, {close_btn_->bounds().x - kCloseBtnS - 4.0f,
                            b.y + 8.0f, kCloseBtnS, kCloseBtnS});
    const bool want_copy = wants_copy_button_();
    float next_x = save_btn_->bounds().x;
    if (want_copy)
    {
        copy_btn_->arrange(lc, {next_x - kCloseBtnS - 4.0f,
                                b.y + 8.0f, kCloseBtnS, kCloseBtnS});
        next_x = copy_btn_->bounds().x;
    }
    fullscreen_btn_->arrange(lc, {next_x - kCloseBtnS - 4.0f, b.y + 8.0f,
                                  kCloseBtnS, kCloseBtnS});

    const bool show = is_open_ && chrome_shown_();
    close_btn_->set_visible(show);
    save_btn_->set_visible(show);
    copy_btn_->set_visible(show && want_copy);
    fullscreen_btn_->set_visible(show);
}

// ── paint ────────────────────────────────────────────────────────────────

void MediaOverlayBase::paint_scrim_(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds(), tk::Color::rgba(0, 0, 0, 210));
}

void MediaOverlayBase::draw_icon_(tk::PaintCtx& ctx, tk::Rect box,
                                  float logical_px,
                                  std::unique_ptr<tk::Image>& cache,
                                  std::span<const std::uint8_t> svg,
                                  tk::Color tint)
{
    auto& cv = ctx.canvas;
    if (!cache)
        cache = tk::rasterize_svg(
            ctx.factory, svg,
            std::max(1, int(std::lround(logical_px * icon_scale_))), tint);
    if (cache)
        cv.draw_image(*cache, {box.x + (box.w - logical_px) * 0.5f,
                               box.y + (box.h - logical_px) * 0.5f, logical_px,
                               logical_px});
}

void MediaOverlayBase::sync_icon_scale_(tk::PaintCtx& ctx)
{
    // Lucide icons are rasterized at physical-pixel resolution and tinted; the
    // cache is invalidated whenever the canvas DPI scale changes.
    const float icon_scale = ctx.canvas.scale_factor();
    if (icon_scale != icon_scale_)
    {
        icon_scale_ = icon_scale;
        close_icon_.reset();
        save_icon_.reset();
        copy_icon_.reset();
        fullscreen_icon_.reset();
        on_icon_scale_changed_();
    }
}

void MediaOverlayBase::paint_chrome_buttons_(tk::PaintCtx& ctx)
{
    auto& cv = ctx.canvas;

    sync_icon_scale_(ctx);

    const tk::Color icon_tint = tk::Color::rgba(255, 255, 255, 220);

    // × close button. Button's own fill (via FillOverride) draws the full
    // resting/hover/pressed pill; the clip only shapes it into a circle.
    cv.push_clip_rounded_rect(close_btn_->bounds(), kCloseBtnS * 0.5f);
    close_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, close_btn_->bounds(), kMediaOverlayBtnIconPx, close_icon_, kCloseSvg, icon_tint);

    // ⬇ save button
    cv.push_clip_rounded_rect(save_btn_->bounds(), kCloseBtnS * 0.5f);
    save_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, save_btn_->bounds(), kMediaOverlayBtnIconPx, save_icon_, kDownloadSvg, icon_tint);

    // ⧉ copy-to-clipboard button (image overlay only)
    if (wants_copy_button_())
    {
        cv.push_clip_rounded_rect(copy_btn_->bounds(), kCloseBtnS * 0.5f);
        copy_btn_->paint(ctx);
        cv.pop_clip();
        draw_icon_(ctx, copy_btn_->bounds(), kMediaOverlayBtnIconPx, copy_icon_, kCopySvg, icon_tint);
    }

    // ⤢ / ⤡ full-screen toggle
    cv.push_clip_rounded_rect(fullscreen_btn_->bounds(), kCloseBtnS * 0.5f);
    fullscreen_btn_->paint(ctx);
    cv.pop_clip();
    const std::span<const std::uint8_t> fs_svg =
        fullscreen_ ? std::span<const std::uint8_t>{kMinimizeSvg}
                    : std::span<const std::uint8_t>{kExpandSvg};
    draw_icon_(ctx, fullscreen_btn_->bounds(), kMediaOverlayBtnIconPx,
               fullscreen_icon_, fs_svg, icon_tint);

    // While the chrome is still shown in full-screen, keep repainting so the
    // auto-hide timer is re-evaluated even without further pointer events
    // (mirrors the loading-spinner self-drive pattern).
    if (fullscreen_ && chrome_visible_)
    {
        request_repaint_();
    }
}

// ── dismiss ──────────────────────────────────────────────────────────────

void MediaOverlayBase::dismiss_()
{
    // Closing the overlay always restores the window: leaving full-screen is
    // never a separate step the user has to take.
    if (fullscreen_)
    {
        fullscreen_ = false;
        fullscreen_icon_.reset();
        if (on_request_fullscreen)
        {
            on_request_fullscreen(false);
        }
    }
    chrome_visible_ = true;
    is_open_ = false;
    if (on_close)
    {
        on_close();
    }
}

// ── pointer dispatch ───────────────────────────────────────────────────────

bool MediaOverlayBase::handle_pointer_down_(tk::Point local)
{
    if (!is_open_)
    {
        return false;
    }

    note_activity_();

    // Only reached when no chrome button claimed the press first (real
    // tk::Button children get first refusal via Widget::dispatch_pointer_down).
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    // Forward to the subclass content; if it declines, treat as outside tap.
    if (on_content_pointer_down_(w, local))
    {
        return true;
    }
    press_outside_ = true;
    return true;
}

void MediaOverlayBase::handle_pointer_up_(tk::Point local, bool inside_self)
{
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (on_content_pointer_up_(w, local, inside_self))
    {
        return;
    }
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
        {
            dismiss_();
        }
        return;
    }
}

} // namespace tesseract::views
