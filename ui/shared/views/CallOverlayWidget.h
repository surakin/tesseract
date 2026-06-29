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

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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

    // Start/stop the elapsed-time counter. Pass initial_seconds to resume a
    // previously-running timer (e.g. after a mode switch). Using double allows
    // mode switches to preserve sub-second precision: elapsed_seconds() returns
    // the true wall-clock offset, not just the integer tick count.
    void   start_timer(double initial_seconds = 0.0);
    void   stop_timer();
    double elapsed_seconds() const;

    // Diff the incoming participant list against `tiles_` and add/remove/update
    // children accordingly. Requests a repaint.
    void update_participants(const std::vector<tesseract::RtcParticipantInfo>& ps);

    // Deliver a pre-converted premultiplied BGRA video frame for participant_id.
    // Called on the UI thread; bgra is forwarded zero-copy to the matching tile.
    void on_video_frame(const std::string& participant_id,
                        std::uint32_t w, std::uint32_t h,
                        std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Deliver a screen-share frame. tile_id must include the ":screen" suffix
    // (appended by ShellBase::handle_rtc_screen_frame_ui_ before calling here).
    void on_screen_frame(const std::string& tile_id,
                         std::uint32_t w, std::uint32_t h,
                         std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Reflect local screen-sharing state on the share button icon.
    void set_screen_sharing(bool sharing);

    void set_audio_muted(bool m);
    void set_video_muted(bool m);

    // Hide the video-mute toggle (call once after mounting for audio-only calls).
    void set_show_video_button(bool show);

    // Set the local user's Matrix user_id so the self tile can be mirrored.
    void set_local_user_id(std::string id) { local_user_id_ = std::move(id); }

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
    std::function<void(bool)>         on_toggle_screen_share;
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
    // tick_alive_ is a lifetime token: schedule_tick_() closures hold a weak_ptr
    // to it. When the widget is destroyed, tick_alive_ is destroyed and all
    // pending callbacks find weak.lock()==nullptr — preventing use-after-free
    // when the allocator reuses the widget's memory address for a new instance
    // whose timer_gen_ happens to equal the old callback's captured gen.
    std::shared_ptr<bool> tick_alive_{std::make_shared<bool>(true)};
    std::uint64_t         timer_gen_      = 0;
    bool                  timer_running_  = false;
    // Wall-clock start point + initial offset: elapsed_seconds() = elapsed_offset_
    // + seconds since start_time_. Storing as double lets mode switches preserve
    // sub-second precision when snapshotting mid-tick.
    double                elapsed_offset_ = 0.0;
    std::chrono::steady_clock::time_point start_time_{};

    // ── Participant tiles ──────────────────────────────────────────────────────
    // Borrowed pointers into children_; ownership is in the widget tree.
    // tile_ids_ and tile_states_ are parallel to tiles_ and track participant
    // identity and non-video metadata (needed by on_video_frame to rebuild state).
    std::vector<ParticipantTile*>       tiles_;
    std::vector<std::string>            tile_ids_;
    std::vector<ParticipantTile::State> tile_states_;
    std::string                         pinned_participant_;
    std::string                         local_user_id_;

    // ── Mode / float position ─────────────────────────────────────────────────
    Mode  mode_    = Mode::Docked;
    float float_x_ = 40.0f;
    float float_y_ = 40.0f;

    // ── Drag state (Floating mode only) ───────────────────────────────────────
    bool      dragging_       = false;
    tk::Point drag_start_pos_ = {};
    float     drag_start_fx_  = 0.0f;
    float     drag_start_fy_  = 0.0f;

    // ── Local mute / share state ──────────────────────────────────────────────
    bool audio_muted_      = false;
    bool video_muted_      = false;
    bool show_video_btn_   = true;
    bool screen_sharing_   = false;

    // ── Control buttons (always present, managed by the widget tree) ──────────
    tk::Button* mute_btn_   = nullptr;
    tk::Button* video_btn_  = nullptr;
    tk::Button* screen_btn_ = nullptr; // Share screen toggle
    tk::Button* hangup_btn_ = nullptr;
    tk::Button* expand_btn_ = nullptr; // Docked ↔ DockedExpanded toggle
    tk::Button* pip_btn_    = nullptr; // Popout (picture-in-picture) window

    // ── Duration label cache ──────────────────────────────────────────────────
    std::string                     cached_duration_str_;
    std::unique_ptr<tk::TextLayout> cached_duration_layout_;

    // ── Icon caches ───────────────────────────────────────────────────────────
    tk::IconCache mic_icon_;
    tk::IconCache mic_off_icon_;
    tk::IconCache video_icon_;
    tk::IconCache video_off_icon_;
    tk::IconCache screen_icon_;   // lucide-monitor (screen share active/inactive)
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
