#pragma once

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

// Shared modal scaffolding for the fullscreen media lightbox overlays
// (ImageViewerOverlay, VideoViewerOverlay). Owns the dark scrim, the
// top-right close (×) / save (⬇) real tk::Button children (icon cache +
// paint), and the outside-tap-to-dismiss behaviour. Subclasses supply only
// their media content (image pan/zoom or video surface + transport
// controls) and the content pointer handling.
//
// Pointer routing contract: close/save/copy are real tk::Button children, so
// the base Widget::dispatch_pointer_down/up machinery gives them clicks,
// hover, and keyboard activation for free — neither this class nor its
// subclasses hand-roll hit-testing for them. When a click lands on neither
// button, it reaches this widget's own on_pointer_down/up (via the
// subclasses' overrides, which call handle_pointer_down_/handle_pointer_up_
// below); the press is forwarded to the subclass via
// on_content_pointer_down_, and only when the subclass declines (returns
// false) does the base treat it as an outside tap that dismisses on release.
class MediaOverlayBase : public tk::Widget
{
public:
    MediaOverlayBase();

    bool is_open() const
    {
        return is_open_;
    }

    // Fires when the overlay should be dismissed (× button, outside click).
    std::function<void()> on_close;

    // Fires when the user clicks the ⬇ save button. The string payload is
    // overlay-specific (image: media_url + filename hint; video: source JSON
    // + mime type) so the subclass invokes it from fire_save_().
    std::function<void(std::string, std::string)> on_save;

    // Fires when the user clicks the copy button (only shown when
    // wants_copy_button_() is true — currently the image overlay). Same
    // overlay-specific payload as on_save; the subclass invokes it from
    // fire_copy_().
    std::function<void(std::string, std::string)> on_copy;

    // Fires when the user toggles the full-screen chrome button, with the new
    // state. The shell wires this to put its native window into / out of OS
    // full-screen. Also fired with `false` from dismiss_() when the overlay
    // closes while full-screen, so the window is always restored on close.
    std::function<void(bool)> on_request_fullscreen;

    // Set by the shell so the overlay can schedule repaints of the hosting
    // surface (loading spinner, video frames, the full-screen chrome auto-hide
    // countdown). Shared by both subclasses.
    void set_repaint_requester(std::function<void()> fn);

    // Pointer-move over the scrim/media (not a chrome button) — reveals the
    // auto-hidden chrome in full-screen and restarts the hide timer. Public to
    // match Widget::on_pointer_move (tests drive it directly).
    bool on_pointer_move(tk::Point local) override;

protected:
    // Position the close/save/copy chrome buttons (and show/hide them per
    // is_open_ / wants_copy_button_()). Subclasses call this from their own
    // layout pass — from arrange() with the LayoutCtx it was given, and again
    // from paint() with a LayoutCtx stitched together from PaintCtx, since
    // zoom/pan-driven geometry (image overlay) or hide_controls_ (video
    // overlay) can change bounds between the two passes.
    void layout_chrome_(tk::LayoutCtx& lc, tk::Rect b);

    // Paint the dark scrim across the whole widget.
    void paint_scrim_(tk::PaintCtx& ctx);

    // Refresh icon_scale_ from the canvas DPI scale; if it changed, drop the
    // base close/save icon caches and call on_icon_scale_changed_() so the
    // subclass can drop its own. Call early in paint when the subclass draws
    // cached icons (e.g. the video play glyph) before paint_chrome_buttons_.
    void sync_icon_scale_(tk::PaintCtx& ctx);

    // Paint the close + save (+ copy) chrome buttons: a permanent translucent
    // pill background (so the icon reads against any image/video content),
    // then the real Button's own hover/press fill clipped to that same pill
    // shape, then the tinted icon. Invalidates the icon cache on DPI-scale
    // change and, when it does, calls on_icon_scale_changed_() so subclasses
    // can drop their own icon caches.
    void paint_chrome_buttons_(tk::PaintCtx& ctx);

    // Shared dismiss: clears open state and fires on_close. Virtual so a
    // subclass can reset extra per-overlay state (e.g. image zoom/pan) on the
    // same path the chrome × button and outside-tap take. Also drops
    // full-screen (firing on_request_fullscreen(false)) so closing the overlay
    // always restores the window.
    virtual void dismiss_();

    // Flip full-screen state: fires on_request_fullscreen with the new value,
    // resets the chrome auto-hide timer, and calls on_fullscreen_changed_() so
    // the subclass can recompute its geometry.
    void toggle_fullscreen_();

    // Schedule a repaint of the hosting surface via the shell-provided
    // requester (no-op until set_repaint_requester()).
    void request_repaint_();

    // Called when full-screen state changes so subclasses can collapse /
    // restore their margins and caption / controls layout.
    virtual void on_fullscreen_changed_(bool /*fullscreen*/) {}

    // True while the top-right chrome cluster should be drawn: always in
    // windowed mode, and in full-screen only until the auto-hide timer lapses
    // (reset by any pointer activity). Subclasses gate their own always-on
    // chrome (e.g. the video controls bar) on this too.
    bool chrome_shown_() const
    {
        return !fullscreen_ || chrome_visible_;
    }

    // Evaluate the full-screen chrome auto-hide timer (idempotent within a
    // frame). layout_chrome_ calls this; subclasses that read chrome_shown_()
    // before their own layout_chrome_ call (e.g. the video controls bar) call
    // it first too.
    void refresh_chrome_autohide_();

    // Whole-window full-screen: OS window chrome hidden (driven by the shell
    // via on_request_fullscreen) and the overlay collapses its margins /
    // caption / controls. Reset to false by each subclass's open().
    bool fullscreen_ = false;

    // Chrome buttons (top-right corner), real tk::Button children so they get
    // hover/press/keyboard-activation for free. Positioned by layout_chrome_.
    // copy_btn_ is only shown when wants_copy_button_() returns true.
    tk::Button* close_btn_ = nullptr;
    tk::Button* save_btn_ = nullptr;
    tk::Button* copy_btn_ = nullptr;

    bool is_open_ = false;

    // ── subclass hooks ──────────────────────────────────────────────────

    // Forwarded pointer-down when neither chrome button was hit. `world` is in
    // root-surface coordinates; `local` is widget-local. Return true if the
    // content claimed the press (so it is NOT treated as an outside tap);
    // return false to let the base dismiss on the matching release.
    virtual bool on_content_pointer_down_(tk::Point world, tk::Point local) = 0;

    // Forwarded pointer-up after a pending close/save press has been ruled
    // out. Return true if a content press was consumed; return false to let
    // the base resolve a pending outside-tap.
    virtual bool
    on_content_pointer_up_(tk::Point world, tk::Point local, bool inside_self)
    {
        (void)world;
        (void)local;
        (void)inside_self;
        return false;
    }

    // Invoke on_save with the subclass's overlay-specific payload.
    virtual void fire_save_() = 0;

    // Opt-in flag for the copy-to-clipboard chrome button. Defaults to off so
    // the video overlay is unaffected; the image overlay overrides to true.
    virtual bool wants_copy_button_() const
    {
        return false;
    }

    // Invoke on_copy with the subclass's overlay-specific payload. Only called
    // when wants_copy_button_() is true and on_copy is set; default no-op.
    virtual void fire_copy_() {}

    // Called by paint_chrome_buttons_ when the canvas DPI scale changes, so
    // subclasses can invalidate their own cached icons (e.g. play_icon_).
    virtual void on_icon_scale_changed_() {}

    // ── shared pointer dispatch ─────────────────────────────────────────
    // Subclasses override Widget::on_pointer_down / on_pointer_up to call
    // these, then layer their own content handling via the hooks above.
    // Only reached when no child widget (i.e. none of the chrome buttons)
    // claimed the press first — see Widget::dispatch_pointer_down.
    bool handle_pointer_down_(tk::Point local);
    void handle_pointer_up_(tk::Point local, bool inside_self);

    // Chrome buttons (top-right corner). In windowed mode always shown while
    // open; in full-screen shown until kChromeHideAfter of pointer inactivity.
    // layout_chrome_ evaluates the timer; note_activity_() re-reveals them and
    // is called on any pointer event and on the full-screen toggle.
    void note_activity_();

    // Draw a chrome icon centred in `box`, rasterizing + caching at the
    // current DPI scale. Shared helper for subclasses that need the same
    // icon-draw treatment for extra glyphs (e.g. the video play icon).
    void draw_icon_(tk::PaintCtx& ctx, tk::Rect box, float logical_px,
                    std::unique_ptr<tk::Image>& cache,
                    std::span<const std::uint8_t> svg, tk::Color tint);

    float icon_scale_ = 0.0f;

    // True while any chrome button is under the pointer — keeps the cluster
    // shown in full-screen even when the pointer is momentarily still.
    bool any_chrome_hovered_() const;

private:
    std::unique_ptr<tk::Image> close_icon_;
    std::unique_ptr<tk::Image> save_icon_;
    std::unique_ptr<tk::Image> copy_icon_;
    std::unique_ptr<tk::Image> fullscreen_icon_;

    // Full-screen toggle (⤢ / ⤡). Real tk::Button child, left of the copy /
    // save cluster. Same auto-hide rules as the other chrome buttons.
    tk::Button* fullscreen_btn_ = nullptr;

    std::function<void()> repaint_requester_;

    // Full-screen chrome auto-hide.
    bool chrome_visible_ = true;
    std::chrono::steady_clock::time_point last_activity_{};

    bool press_outside_ = false;
};

} // namespace tesseract::views
