#pragma once
#ifdef TESSERACT_CALLS_ENABLED

// CallOverlayWidget — call UI container supporting four display modes:
//
//   Docked        — fixed 220 px strip anchored to the call bar area;
//                   expand button toggles to DockedExpanded.
//   DockedExpanded — fills the available height given by the parent.
//   Floating      — 320×240 window fragment; draggable via top 32 px header.
//   Popout        — fills an entire secondary window; no expand button.
//
// Child ParticipantTile widgets are managed dynamically via add_child /
// remove_child. `tiles_` holds borrowed pointers; ownership lives in the
// widget tree (children_). The grid layout algorithm reuses the column
// heuristic from CallOverlay: 1 tile → 1 col, 2–4 → 2 cols, 5+ → 3 cols.
// A pinned participant receives ~70% of the width; the rest fill a sidebar.
//
// The elapsed-time counter is driven by a 1 s post_delayed_ loop (same
// pattern as CallOverlay). Call stop_timer() before destroying the widget.

#include "ParticipantTile.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class CallOverlayWidget : public tk::Widget
{
public:
    enum class Mode
    {
        Docked,
        DockedExpanded,
        Floating,
        Popout,
    };

    CallOverlayWidget();
    ~CallOverlayWidget() override;

    using PostDelayedFn = std::function<void(int, std::function<void()>)>;

    void set_post_delayed(PostDelayedFn fn);
    void set_repaint_requester(std::function<void()> fn);
    // Fired by update_participants() to trigger a full layout pass after tiles
    // are added, removed, or reordered. Distinct from repaint_requester_ so
    // push_video_frame() can stay repaint-only (no layout, full 30fps).
    // Falls back to repaint_requester_ when not set.
    void set_relayout_requester(std::function<void()> fn);
    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& mxc_url)> fn);
    void set_display_name_provider(
        std::function<std::string(const std::string& user_id)> fn);

    // Start/stop the elapsed-time counter.
    void start_timer();
    void stop_timer();

    // Diff the incoming participant list against `tiles_` and add/remove/update
    // children accordingly. Requests a repaint.
    void update_participants(const std::vector<tesseract::RtcParticipantInfo>& ps);

    // Deliver a decoded RGBA video frame for participant_id. Called on the UI
    // thread; the image is forwarded to the matching tile.
    void on_video_frame(const std::string& participant_id,
                        std::uint32_t w, std::uint32_t h,
                        const std::uint8_t* rgba, std::size_t rgba_size);

    void set_audio_muted(bool m);
    void set_video_muted(bool m);

    // Switch to a different display mode. The parent shell must re-arrange
    // after this call.
    void set_mode(Mode m);
    Mode mode() const { return mode_; }

    // Floating mode position (top-left corner, parent-local coordinates).
    void      set_float_position(float x, float y);
    tk::Point float_position() const { return {float_x_, float_y_}; }

    // Callbacks fired by control buttons and user interaction.
    std::function<void()>             on_hang_up;
    std::function<void(bool)>         on_toggle_audio;
    std::function<void(bool)>         on_toggle_video;
    std::function<void(Mode)>         on_mode_change_requested;
    std::function<void(float, float)> on_float_position_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_drag(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool cancelled) override;

private:
    void schedule_tick_();

    // ── Providers ─────────────────────────────────────────────────────────────
    PostDelayedFn                                        post_delayed_;
    std::function<void()>                                repaint_requester_;
    std::function<void()>                                relayout_requester_;
    std::function<const tk::Image*(const std::string&)> avatar_provider_;
    std::function<std::string(const std::string&)>      display_name_provider_;

    // ── Timer ─────────────────────────────────────────────────────────────────
    std::uint64_t timer_gen_       = 0;
    bool          timer_running_   = false;
    int           elapsed_seconds_ = 0;

    // ── Participant tiles ──────────────────────────────────────────────────────
    // Borrowed pointers into children_; ownership is in the widget tree.
    // tile_ids_ and tile_states_ are parallel to tiles_ and track participant
    // identity and non-video metadata (needed by on_video_frame to rebuild state).
    std::vector<ParticipantTile*>       tiles_;
    std::vector<std::string>            tile_ids_;
    std::vector<ParticipantTile::State> tile_states_;
    std::string                         pinned_participant_;

    // ── Mode / float position ─────────────────────────────────────────────────
    Mode  mode_    = Mode::Docked;
    float float_x_ = 40.0f;
    float float_y_ = 40.0f;

    // ── Drag state (Floating mode only) ───────────────────────────────────────
    bool      dragging_       = false;
    tk::Point drag_start_pos_ = {};
    float     drag_start_fx_  = 0.0f;
    float     drag_start_fy_  = 0.0f;

    // ── Local mute state ──────────────────────────────────────────────────────
    bool audio_muted_ = false;
    bool video_muted_ = false;

    // ── Control buttons (always present, managed by the widget tree) ──────────
    tk::Button* mute_btn_   = nullptr;
    tk::Button* video_btn_  = nullptr;
    tk::Button* hangup_btn_ = nullptr;
    tk::Button* expand_btn_ = nullptr; // Docked ↔ DockedExpanded toggle
    tk::Button* pip_btn_    = nullptr; // Popout (picture-in-picture) window

    // ── Icon caches ───────────────────────────────────────────────────────────
    tk::IconCache mic_icon_;
    tk::IconCache mic_off_icon_;
    tk::IconCache video_icon_;
    tk::IconCache video_off_icon_;
    tk::IconCache phone_off_icon_;
    tk::IconCache expand_icon_;   // lucide-expand (Docked → DockedExpanded)
    tk::IconCache minimize_icon_; // lucide-minimize (DockedExpanded → Docked)
    tk::IconCache pip_icon_;      // lucide-pip (→ Popout window)

    // ── Cached layout rects ───────────────────────────────────────────────────
    tk::Rect controls_rect_{};
    tk::Rect duration_rect_{};
    tk::Rect grid_rect_{};
    tk::Rect drag_header_rect_{}; // top 32 px strip, Floating mode only
};

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
