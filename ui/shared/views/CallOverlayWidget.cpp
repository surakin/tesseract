#ifdef TESSERACT_CALLS_ENABLED
#include "CallOverlayWidget.h"

#include "icons.h"

#include <algorithm>
#include <cstdio>

namespace tesseract::views
{

namespace
{
constexpr float kDockedH      = 220.0f;
constexpr float kFloatingW    = 320.0f;
constexpr float kFloatingH    = 240.0f;
constexpr float kControlsH    = 48.0f;
constexpr float kDurationH    = 22.0f;
constexpr float kBtnSz        = 40.0f;
constexpr float kCallOverlayBtnIconPx    = 20.0f;
constexpr float kCallOverlayBtnGap       = 16.0f;
constexpr float kExpandSz     = 28.0f;
constexpr float kExpandMargin = 4.0f;
constexpr float kDragHeaderH  = 32.0f;
constexpr float kPinnedFrac   = 0.70f; // width fraction for the pinned tile

constexpr tk::Color kOverlayBg{  0,   0,   0, 220};
constexpr tk::Color kCtrlBg   {  0,   0,   0, 180};
constexpr tk::Color kHeaderBg {  0,   0,   0, 160};
constexpr tk::Color kCallOverlayWhite    {255, 255, 255, 255};
constexpr tk::Color kCallOverlayWhiteSoft{255, 255, 255, 200};
constexpr tk::Color kCallOverlayMutedRed {220,  60,  60, 255};
} // namespace

// ── Constructor / destructor ───────────────────────────────────────────────

CallOverlayWidget::CallOverlayWidget()
{
    auto mute = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                             tk::Button::Variant::Icon);
    mute_btn_ = add_child(std::move(mute));
    mute_btn_->set_on_click([this] {
        audio_muted_ = !audio_muted_;
        if (on_toggle_audio) on_toggle_audio(audio_muted_);
        if (repaint_requester_) repaint_requester_();
    });

    auto vid = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    video_btn_ = add_child(std::move(vid));
    video_btn_->set_on_click([this] {
        video_muted_ = !video_muted_;
        if (on_toggle_video) on_toggle_video(video_muted_);
        if (repaint_requester_) repaint_requester_();
    });

    auto screen = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                               tk::Button::Variant::Icon);
    screen_btn_ = add_child(std::move(screen));
    screen_btn_->set_on_click([this] {
        screen_sharing_ = !screen_sharing_;
        if (on_toggle_screen_share) on_toggle_screen_share(screen_sharing_);
        if (repaint_requester_) repaint_requester_();
    });

    auto hang = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                             tk::Button::Variant::Icon);
    hangup_btn_ = add_child(std::move(hang));
    hangup_btn_->set_on_click([this] {
        if (on_hang_up) on_hang_up();
    });

    auto expand = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                               tk::Button::Variant::Icon);
    expand_btn_ = add_child(std::move(expand));
    expand_btn_->set_on_click([this] {
        if (mode_ == Mode::Docked)
        {
            if (on_mode_change_requested) on_mode_change_requested(Mode::DockedExpanded);
        }
        else if (mode_ == Mode::DockedExpanded)
        {
            if (on_mode_change_requested) on_mode_change_requested(Mode::Docked);
        }
    });

    auto pip = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    pip_btn_ = add_child(std::move(pip));
    pip_btn_->set_on_click([this] {
        if (!on_mode_change_requested) return;
        // Cycle: Docked/DockedExpanded → Floating → Popout → Docked
        Mode target = Mode::Docked;
        if (mode_ == Mode::Docked || mode_ == Mode::DockedExpanded)
            target = Mode::Floating;
        else if (mode_ == Mode::Floating)
            target = Mode::Popout;
        // else Popout → Docked (already initialised above)
        on_mode_change_requested(target);
    });
}

CallOverlayWidget::~CallOverlayWidget()
{
    stop_timer();
}

// ── Configuration ──────────────────────────────────────────────────────────

void CallOverlayWidget::set_post_delayed(PostDelayedFn fn)
{
    post_delayed_ = std::move(fn);
}

void CallOverlayWidget::set_repaint_requester(std::function<void()> fn)
{
    repaint_requester_ = std::move(fn);
}

void CallOverlayWidget::set_relayout_requester(std::function<void()> fn)
{
    relayout_requester_ = std::move(fn);
}

void CallOverlayWidget::set_avatar_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    avatar_provider_ = std::move(fn);
    for (auto* tile : tiles_)
        tile->set_avatar_provider(avatar_provider_);
}

void CallOverlayWidget::set_display_name_provider(
    std::function<std::string(const std::string&)> fn)
{
    display_name_provider_ = std::move(fn);
}

// ── Timer ──────────────────────────────────────────────────────────────────

double CallOverlayWidget::elapsed_seconds() const
{
    if (!timer_running_) return elapsed_offset_;
    using namespace std::chrono;
    const auto dur = duration_cast<duration<double>>(steady_clock::now() - start_time_);
    return elapsed_offset_ + dur.count();
}

CallOverlayWidget::OverlayState CallOverlayWidget::snapshot() const
{
    return {elapsed_seconds(), show_video_btn_, audio_muted_,
            video_muted_, screen_sharing_, local_user_id_};
}

void CallOverlayWidget::restore(const OverlayState& s)
{
    set_local_user_id(s.local_user_id);
    set_show_video_button(s.show_video_button);
    set_audio_muted(s.audio_muted);
    set_video_muted(s.video_muted);
    set_screen_sharing(s.screen_sharing);
    start_timer(s.elapsed_seconds);
}

void CallOverlayWidget::start_timer(double initial_seconds)
{
    // Cancel any existing chain before starting a new one. Without this,
    // calling start_timer() on a running timer creates a second chain that
    // shares the current timer_gen_ — both chains fire, rate doubles each call.
    stop_timer();
    elapsed_offset_ = initial_seconds;
    start_time_     = std::chrono::steady_clock::now();
    timer_running_  = true;
    schedule_tick_();
}

void CallOverlayWidget::stop_timer()
{
    ++timer_gen_;
    timer_running_ = false;
}

void CallOverlayWidget::schedule_tick_()
{
    if (!post_delayed_ || !timer_running_) return;
    const auto gen = timer_gen_;
    // Capture a weak_ptr to tick_alive_ so the closure can detect widget
    // destruction without touching `this`. When this widget is freed its
    // tick_alive_ shared_ptr is destroyed; weak.lock() returns nullptr and
    // the closure exits before accessing any member — safe even when the
    // allocator reuses the same address for a new widget (which would have
    // matching timer_gen_ == 1 and trigger a false-positive gen check alone).
    std::weak_ptr<bool> weak = tick_alive_;
    post_delayed_(1000, [this, gen, weak] {
        if (!weak.lock()) return;       // widget was destroyed
        if (gen != timer_gen_) return;  // timer stopped or restarted on live widget
        if (repaint_requester_) repaint_requester_();
        schedule_tick_();
    });
}

// ── Video button visibility ───────────────────────────────────────────────

void CallOverlayWidget::set_show_video_button(bool show)
{
    show_video_btn_ = show;
    if (video_btn_)
        video_btn_->set_visible(show);
}

// ── Mode / float position ─────────────────────────────────────────────────

void CallOverlayWidget::set_mode(Mode m)
{
    mode_ = m;
    // Keep button visibility consistent before the first arrange() pass.
    const bool show_expand = (mode_ == Mode::Docked || mode_ == Mode::DockedExpanded);
    if (expand_btn_) expand_btn_->set_visible(show_expand);
    // pip_btn_ is visible in all modes: cycles Docked→Floating→Popout→Docked.
    if (pip_btn_) pip_btn_->set_visible(true);
}

void CallOverlayWidget::set_float_position(float x, float y)
{
    float_x_ = x;
    float_y_ = y;
}

// ── Participant management ─────────────────────────────────────────────────

void CallOverlayWidget::update_participants(
    const std::vector<tesseract::RtcParticipantInfo>& ps)
{
    // ── Remove departed participants ───────────────────────────────────────

    // Diff incoming list against tile_ids_: remove departed tiles, add new ones.
    // Screen-share tiles use participant_id + ":screen" as their id; they are
    // removed when the participant leaves OR stops screen sharing.
    {
        std::vector<ParticipantTile*>       keep;
        std::vector<std::string>            keep_ids;
        std::vector<ParticipantTile::State> keep_states;
        keep.reserve(tiles_.size());
        keep_ids.reserve(tiles_.size());
        keep_states.reserve(tiles_.size());

        for (std::size_t i = 0; i < tiles_.size(); ++i)
        {
            const std::string& id = tile_ids_[i];
            const bool is_screen_tile = id.size() > 7 &&
                id.substr(id.size() - 7) == ":screen";
            bool still_present = false;
            if (is_screen_tile)
            {
                const std::string base_id = id.substr(0, id.size() - 7);
                still_present = std::any_of(ps.begin(), ps.end(),
                    [&](const tesseract::RtcParticipantInfo& p) {
                        return p.participant_id == base_id && p.is_screen_sharing;
                    });
            }
            else
            {
                still_present = std::any_of(ps.begin(), ps.end(),
                    [&](const tesseract::RtcParticipantInfo& p) {
                        return p.participant_id == id;
                    });
            }
            if (!still_present)
            {
                remove_child(tiles_[i]);
            }
            else
            {
                keep.push_back(tiles_[i]);
                keep_ids.push_back(tile_ids_[i]);
                keep_states.push_back(std::move(tile_states_[i]));
            }
        }

        tiles_       = std::move(keep);
        tile_ids_    = std::move(keep_ids);
        tile_states_ = std::move(keep_states);
    }

    // Helper lambda: add one tile (camera or screen-share) for a participant.
    auto add_tile = [&](const tesseract::RtcParticipantInfo& p,
                        const std::string& tile_id,
                        bool is_screen_tile)
    {
        auto tile_up = std::make_unique<ParticipantTile>();
        ParticipantTile* tile = add_child(std::move(tile_up));

        tile->set_repaint_requester(repaint_requester_);
        if (avatar_provider_)
            tile->set_avatar_provider(avatar_provider_);
        tile->on_pin_toggled = [this](const std::string& pid) {
            if (pinned_participant_ == pid)
                pinned_participant_.clear();
            else
                pinned_participant_ = pid;
            for (std::size_t i = 0; i < tiles_.size(); ++i)
                tiles_[i]->set_pinned(pinned_participant_ == tile_ids_[i]);
            if (relayout_requester_) relayout_requester_();
            else if (repaint_requester_) repaint_requester_();
        };

        std::string dname = p.user_id;
        if (display_name_provider_ && !p.user_id.empty())
        {
            const auto n = display_name_provider_(p.user_id);
            if (!n.empty()) dname = n;
        }
        if (dname.empty()) dname = p.participant_id;

        // Screen-share tiles carry their own media stream — the parent
        // participant's camera/mic mute flags are unrelated and must not
        // suppress rendering of arriving screen frames (ParticipantTile::paint
        // gates on video_muted before drawing pending_bgra).
        const bool audio_muted = !is_screen_tile && p.is_audio_muted;
        const bool video_muted = !is_screen_tile && p.is_video_muted;

        ParticipantTile::State s;
        s.participant_id     = tile_id;
        s.user_id            = p.user_id;
        s.display_name       = dname;
        s.audio_muted        = audio_muted;
        s.video_muted        = video_muted;
        s.is_self            = (!local_user_id_.empty() && p.user_id == local_user_id_);
        s.pinned             = (pinned_participant_ == tile_id);
        s.is_screen_share_tile = is_screen_tile;
        tile->set_state(std::move(s));

        tiles_.push_back(tile);
        tile_ids_.push_back(tile_id);
        ParticipantTile::State shadow;
        shadow.participant_id     = tile_id;
        shadow.user_id            = p.user_id;
        shadow.display_name       = dname;
        shadow.audio_muted        = audio_muted;
        shadow.video_muted        = video_muted;
        shadow.is_self            = s.is_self;
        shadow.pinned             = s.pinned;
        shadow.is_screen_share_tile = is_screen_tile;
        tile_states_.push_back(std::move(shadow));
    };

    // Add new participants and update existing ones.
    for (const auto& p : ps)
    {
        const auto it = std::find(tile_ids_.begin(), tile_ids_.end(),
                                  p.participant_id);
        if (it == tile_ids_.end())
        {
            add_tile(p, p.participant_id, /*is_screen_tile=*/false);
        }
        else
        {
            // Existing participant — update mute state.
            const std::size_t idx = static_cast<std::size_t>(it - tile_ids_.begin());
            ParticipantTile* tile = tiles_[idx];

            std::string dname = p.user_id;
            if (display_name_provider_ && !p.user_id.empty())
            {
                const auto n = display_name_provider_(p.user_id);
                if (!n.empty()) dname = n;
            }
            if (dname.empty()) dname = p.participant_id;

            const bool this_is_self = (!local_user_id_.empty() && p.user_id == local_user_id_);
            ParticipantTile::State s;
            s.participant_id     = p.participant_id;
            s.user_id            = p.user_id;
            s.display_name       = dname;
            s.audio_muted        = p.is_audio_muted;
            s.video_muted        = p.is_video_muted;
            s.is_self            = this_is_self;
            s.pinned             = (pinned_participant_ == p.participant_id);
            s.is_screen_share_tile = false;
            tile->set_state(std::move(s));
            tile_states_[idx].user_id            = p.user_id;
            tile_states_[idx].display_name       = dname;
            tile_states_[idx].audio_muted        = p.is_audio_muted;
            tile_states_[idx].video_muted        = p.is_video_muted;
            tile_states_[idx].is_self            = this_is_self;
            tile_states_[idx].pinned             = (pinned_participant_ == p.participant_id);
        }

        // Add or keep a screen-share tile for this participant when sharing.
        if (p.is_screen_sharing)
        {
            const std::string screen_id = p.participant_id + ":screen";
            const auto sit = std::find(tile_ids_.begin(), tile_ids_.end(), screen_id);
            if (sit == tile_ids_.end())
                add_tile(p, screen_id, /*is_screen_tile=*/true);
        }
    }

    // A relayout is required — new tiles have bounds_ = {0,0,0,0} until
    // arrange() runs. push_video_frame() uses repaint_requester_ (no layout).
    if (relayout_requester_) relayout_requester_();
    else if (repaint_requester_) repaint_requester_();
}

void CallOverlayWidget::on_video_frame(const std::string& participant_id,
                                       std::uint32_t w, std::uint32_t h,
                                       std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    const auto it = std::find(tile_ids_.begin(), tile_ids_.end(), participant_id);
    if (it == tile_ids_.end()) return;

    const std::size_t idx = static_cast<std::size_t>(it - tile_ids_.begin());
    // Forward the shared_ptr without copying; push_video_frame() stores it and
    // fires repaint_requester_. No second memcpy on the UI thread.
    tiles_[idx]->push_video_frame(w, h, std::move(bgra));
}

void CallOverlayWidget::on_screen_frame(const std::string& tile_id,
                                        std::uint32_t w, std::uint32_t h,
                                        std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    // tile_id already has ":screen" suffix; delegate to the same path as camera frames.
    const auto it = std::find(tile_ids_.begin(), tile_ids_.end(), tile_id);
    if (it == tile_ids_.end()) return;
    const std::size_t idx = static_cast<std::size_t>(it - tile_ids_.begin());
    tiles_[idx]->push_video_frame(w, h, std::move(bgra));
}

void CallOverlayWidget::set_screen_sharing(bool sharing)
{
    screen_sharing_ = sharing;
    if (repaint_requester_) repaint_requester_();
}

void CallOverlayWidget::set_audio_muted(bool m)
{
    audio_muted_ = m;
    if (repaint_requester_) repaint_requester_();
}

void CallOverlayWidget::set_video_muted(bool m)
{
    video_muted_ = m;
    if (repaint_requester_) repaint_requester_();
}

// ── Layout ─────────────────────────────────────────────────────────────────

tk::Size CallOverlayWidget::measure(tk::LayoutCtx&, tk::Size constraints)
{
    switch (mode_)
    {
    case Mode::Docked:
        return {constraints.w, kDockedH};
    case Mode::DockedExpanded:
    case Mode::Popout:
        return {constraints.w, constraints.h};
    case Mode::Floating:
        return {kFloatingW, kFloatingH};
    }
    return {constraints.w, kDockedH}; // unreachable
}

void CallOverlayWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // In Floating mode, the drag header occupies the top strip.
    drag_header_rect_ = (mode_ == Mode::Floating)
        ? tk::Rect{bounds_.x, bounds_.y, bounds_.w, kDragHeaderH}
        : tk::Rect{};

    const float grid_top = (mode_ == Mode::Floating)
        ? bounds_.y + kDragHeaderH
        : bounds_.y;

    // Controls bar anchored to the bottom.
    controls_rect_ = {bounds_.x,
                      bounds_.y + bounds_.h - kControlsH,
                      bounds_.w, kControlsH};

    // Duration label just above the controls.
    duration_rect_ = {bounds_.x,
                      controls_rect_.y - kDurationH,
                      bounds_.w, kDurationH};

    // Participant grid fills the remaining area between grid_top and duration.
    grid_rect_ = {bounds_.x, grid_top,
                  bounds_.w, duration_rect_.y - grid_top};
    if (grid_rect_.h < 0.0f) grid_rect_.h = 0.0f;

    // ── Tile layout ────────────────────────────────────────────────────────
    const int n = static_cast<int>(tiles_.size());
    if (n > 0)
    {
        const bool has_pin = !pinned_participant_.empty();
        const int  pin_idx = has_pin
            ? static_cast<int>(std::find(tile_ids_.begin(), tile_ids_.end(),
                                         pinned_participant_) - tile_ids_.begin())
            : -1;
        const bool valid_pin = has_pin && pin_idx < n;

        if (valid_pin && n > 1)
        {
            // Pinned tile takes kPinnedFrac of the width; sidebar takes the rest.
            const float main_w    = grid_rect_.w * kPinnedFrac;
            const float sidebar_w = grid_rect_.w - main_w;

            tiles_[pin_idx]->arrange(ctx, {grid_rect_.x, grid_rect_.y,
                                           main_w, grid_rect_.h});

            // Remaining tiles stacked vertically in the right column.
            const int rest = n - 1;
            const float cell_h = rest > 0 ? grid_rect_.h / static_cast<float>(rest) : grid_rect_.h;
            int row = 0;
            for (int i = 0; i < n; ++i)
            {
                if (i == pin_idx) continue;
                tiles_[i]->arrange(ctx, {grid_rect_.x + main_w,
                                         grid_rect_.y + row * cell_h,
                                         sidebar_w, cell_h});
                ++row;
            }
        }
        else
        {
            // Standard multi-column grid.
            const int cols = (n == 1) ? 1 : (n <= 4) ? 2 : 3;
            const int rows = (n + cols - 1) / cols;
            const float cw = grid_rect_.w / static_cast<float>(cols);
            const float ch = grid_rect_.h / static_cast<float>(rows);

            for (int i = 0; i < n; ++i)
            {
                const int col = i % cols;
                const int row = i / cols;
                tiles_[i]->arrange(ctx, {grid_rect_.x + col * cw,
                                          grid_rect_.y + row * ch,
                                          cw, ch});
            }
        }
    }

    // ── Control buttons ────────────────────────────────────────────────────
    // Order: mic | video (opt) | screen-share | hang-up
    int   btn_count = 2; // mic + hang-up always present
    if (show_video_btn_) ++btn_count; // video
    ++btn_count;                      // screen-share always shown
    const float total_w = btn_count * kBtnSz + (btn_count - 1) * kCallOverlayBtnGap;
    const float start_x = controls_rect_.x + (controls_rect_.w - total_w) * 0.5f;
    const float btn_y   = controls_rect_.y + (kControlsH - kBtnSz) * 0.5f;

    float bx = start_x;
    if (mute_btn_)
    {
        mute_btn_->arrange(ctx, {bx, btn_y, kBtnSz, kBtnSz});
        bx += kBtnSz + kCallOverlayBtnGap;
    }
    if (video_btn_ && show_video_btn_)
    {
        video_btn_->arrange(ctx, {bx, btn_y, kBtnSz, kBtnSz});
        bx += kBtnSz + kCallOverlayBtnGap;
    }
    if (screen_btn_)
    {
        screen_btn_->arrange(ctx, {bx, btn_y, kBtnSz, kBtnSz});
        bx += kBtnSz + kCallOverlayBtnGap;
    }
    if (hangup_btn_)
        hangup_btn_->arrange(ctx, {bx, btn_y, kBtnSz, kBtnSz});

    // ── Expand + pip buttons (top-right corner) ──────────────────────────────
    // expand_btn_: Docked ↔ DockedExpanded toggle; hidden otherwise.
    // pip_btn_:    cycles Docked/DockedExpanded → Floating → Popout → Docked;
    //              always visible so the user can advance through all modes.
    const bool show_expand = (mode_ == Mode::Docked || mode_ == Mode::DockedExpanded);
    const float ey = bounds_.y + kExpandMargin;
    // Right-edge anchor for the button strip; pip is rightmost, expand to its left.
    float right_edge = bounds_.x + bounds_.w - kExpandMargin;
    if (pip_btn_)
    {
        pip_btn_->set_visible(true);
        const float px = right_edge - kExpandSz;
        pip_btn_->arrange(ctx, {px, ey, kExpandSz, kExpandSz});
        right_edge = px - 4.0f;  // gap before next button
    }
    if (expand_btn_)
    {
        expand_btn_->set_visible(show_expand);
        if (show_expand)
        {
            const float ex = right_edge - kExpandSz;
            expand_btn_->arrange(ctx, {ex, ey, kExpandSz, kExpandSz});
        }
    }
}

// ── Paint ──────────────────────────────────────────────────────────────────

void CallOverlayWidget::paint(tk::PaintCtx& ctx)
{
    if (bounds_.h <= 0.0f) return;

    // Background.
    ctx.canvas.fill_rect(bounds_, kOverlayBg);

    // Drag header (Floating mode only).
    if (mode_ == Mode::Floating && drag_header_rect_.h > 0.0f)
        ctx.canvas.fill_rect(drag_header_rect_, kHeaderBg);

    // ── Participant tiles ──────────────────────────────────────────────────
    for (auto* tile : tiles_)
    {
        if (tile->visible()) tile->paint(ctx);
    }

    // ── Duration label ─────────────────────────────────────────────────────
    {
        const int total = static_cast<int>(elapsed_seconds());
        const int s = total % 60;
        const int m = (total / 60) % 60;
        const int h = total / 3600;
        char buf[16];
        if (h > 0)
            std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
        else
            std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);

        // Rebuild the text layout only when the formatted string changes
        // (at most 1 Hz). Video repaints arrive at ~30 Hz, so without caching
        // build_text() would be called on every frame even when the text is
        // the same.
        if (buf != cached_duration_str_)
        {
            cached_duration_str_ = buf;
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Body;
            cached_duration_layout_ = ctx.factory.build_text(cached_duration_str_, ts);
        }

        if (cached_duration_layout_)
        {
            const tk::Size sz = cached_duration_layout_->measure();
            const float tx = duration_rect_.x + (duration_rect_.w - sz.w) * 0.5f;
            const float ty = duration_rect_.y + (duration_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*cached_duration_layout_, {tx, ty}, kCallOverlayWhiteSoft);
        }
    }

    // ── Controls bar ───────────────────────────────────────────────────────
    ctx.canvas.fill_rect(controls_rect_, kCtrlBg);

    if (mute_btn_)
    {
        mute_btn_->paint(ctx);
        if (audio_muted_)
            mic_off_icon_.draw(ctx.canvas, ctx.factory, kMicOffSvg,
                               mute_btn_->bounds(), kCallOverlayBtnIconPx, kCallOverlayMutedRed);
        else
            mic_icon_.draw(ctx.canvas, ctx.factory, kMicSvg,
                           mute_btn_->bounds(), kCallOverlayBtnIconPx, kCallOverlayWhite);
    }

    if (video_btn_ && show_video_btn_)
    {
        video_btn_->paint(ctx);
        if (video_muted_)
            video_off_icon_.draw(ctx.canvas, ctx.factory, kVideoOffSvg,
                                 video_btn_->bounds(), kCallOverlayBtnIconPx, kCallOverlayMutedRed);
        else
            video_icon_.draw(ctx.canvas, ctx.factory, kVideoSvg,
                             video_btn_->bounds(), kCallOverlayBtnIconPx, kCallOverlayWhite);
    }

    if (screen_btn_)
    {
        screen_btn_->paint(ctx);
        const tk::Color scr_color = screen_sharing_ ? kCallOverlayMutedRed : kCallOverlayWhite;
        screen_icon_.draw(ctx.canvas, ctx.factory, kMonitorSvg,
                          screen_btn_->bounds(), kCallOverlayBtnIconPx, scr_color);
    }

    if (hangup_btn_)
    {
        hangup_btn_->paint(ctx);
        phone_off_icon_.draw(ctx.canvas, ctx.factory, kPhoneOffSvg,
                             hangup_btn_->bounds(), kCallOverlayBtnIconPx, kCallOverlayMutedRed);
    }

    // ── Expand / minimize button ───────────────────────────────────────────
    if (expand_btn_ && expand_btn_->visible())
    {
        expand_btn_->paint(ctx);
        if (mode_ == Mode::Docked)
            expand_icon_.draw(ctx.canvas, ctx.factory, kExpandSvg,
                              expand_btn_->bounds(), kExpandSz - 8.0f, kCallOverlayWhite);
        else
            minimize_icon_.draw(ctx.canvas, ctx.factory, kMinimizeSvg,
                                expand_btn_->bounds(), kExpandSz - 8.0f, kCallOverlayWhite);
    }

    // ── Pip / dock button ─────────────────────────────────────────────────────
    // In Popout mode the button docks the call back (show minimize icon);
    // otherwise it pops out to a secondary window (show pip icon).
    if (pip_btn_ && pip_btn_->visible())
    {
        pip_btn_->paint(ctx);
        if (mode_ == Mode::Popout)
            minimize_icon_.draw(ctx.canvas, ctx.factory, kMinimizeSvg,
                                pip_btn_->bounds(), kExpandSz - 8.0f, kCallOverlayWhite);
        else
            pip_icon_.draw(ctx.canvas, ctx.factory, kPipSvg,
                           pip_btn_->bounds(), kExpandSz - 8.0f, kCallOverlayWhite);
    }
}

// ── Pointer events ─────────────────────────────────────────────────────────

bool CallOverlayWidget::on_pointer_down(tk::Point local)
{
    if (mode_ == Mode::Floating)
    {
        // local is widget-relative (0,0 = widget top-left), so compare
        // directly to kDragHeaderH without adding bounds_.y.
        if (local.y < kDragHeaderH)
        {
            dragging_ = true;
            // Store drag origin in world coordinates so the delta stays
            // correct even after bounds_ is updated by a mid-drag relayout.
            drag_start_pos_ = {local.x + bounds_.x, local.y + bounds_.y};
            drag_start_fx_  = float_x_;
            drag_start_fy_  = float_y_;
            return true;
        }
    }
    return false;
}

void CallOverlayWidget::on_pointer_drag(tk::Point local)
{
    if (!dragging_) return;

    // Reconstruct world coordinates: local + bounds_ == world always,
    // regardless of whether bounds_ was updated by a relayout since the
    // last event. This keeps the drag 1:1 with the mouse.
    const tk::Point world_pos{local.x + bounds_.x, local.y + bounds_.y};
    const float dx = world_pos.x - drag_start_pos_.x;
    const float dy = world_pos.y - drag_start_pos_.y;
    float_x_ = drag_start_fx_ + dx;
    float_y_ = drag_start_fy_ + dy;

    if (on_float_position_changed) on_float_position_changed(float_x_, float_y_);
    if (repaint_requester_) repaint_requester_();
}

void CallOverlayWidget::on_pointer_up(tk::Point /*local*/, bool /*cancelled*/)
{
    dragging_ = false;
}

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
