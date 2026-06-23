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
constexpr float kBtnIconPx    = 20.0f;
constexpr float kBtnGap       = 16.0f;
constexpr float kExpandSz     = 28.0f;
constexpr float kExpandMargin = 4.0f;
constexpr float kDragHeaderH  = 32.0f;
constexpr float kPinnedFrac   = 0.70f; // width fraction for the pinned tile

constexpr tk::Color kOverlayBg{  0,   0,   0, 220};
constexpr tk::Color kCtrlBg   {  0,   0,   0, 180};
constexpr tk::Color kHeaderBg {  0,   0,   0, 160};
constexpr tk::Color kWhite    {255, 255, 255, 255};
constexpr tk::Color kWhiteSoft{255, 255, 255, 200};
constexpr tk::Color kMutedRed {220,  60,  60, 255};
} // namespace

// ── Constructor / destructor ───────────────────────────────────────────────

CallOverlayWidget::CallOverlayWidget()
{
    auto mute = std::make_unique<tk::Button>("", std::function<void()>{},
                                             tk::Button::Variant::Icon);
    mute_btn_ = add_child(std::move(mute));
    mute_btn_->set_on_click([this] {
        audio_muted_ = !audio_muted_;
        if (on_toggle_audio) on_toggle_audio(audio_muted_);
        if (repaint_requester_) repaint_requester_();
    });

    auto vid = std::make_unique<tk::Button>("", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    video_btn_ = add_child(std::move(vid));
    video_btn_->set_on_click([this] {
        video_muted_ = !video_muted_;
        if (on_toggle_video) on_toggle_video(video_muted_);
        if (repaint_requester_) repaint_requester_();
    });

    auto hang = std::make_unique<tk::Button>("", std::function<void()>{},
                                             tk::Button::Variant::Icon);
    hangup_btn_ = add_child(std::move(hang));
    hangup_btn_->set_on_click([this] {
        if (on_hang_up) on_hang_up();
    });

    auto expand = std::make_unique<tk::Button>("", std::function<void()>{},
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

    auto pip = std::make_unique<tk::Button>("", std::function<void()>{},
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

void CallOverlayWidget::start_timer()
{
    elapsed_seconds_ = 0;
    timer_running_   = true;
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
    post_delayed_(1000, [this, gen] {
        if (gen != timer_gen_) return;
        ++elapsed_seconds_;
        if (repaint_requester_) repaint_requester_();
        schedule_tick_();
    });
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
            const bool still_present = std::any_of(ps.begin(), ps.end(),
                [&](const tesseract::RtcParticipantInfo& p) {
                    return p.participant_id == id;
                });
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

    // Add new participants and update existing ones.
    for (const auto& p : ps)
    {
        const auto it = std::find(tile_ids_.begin(), tile_ids_.end(),
                                  p.participant_id);
        if (it == tile_ids_.end())
        {
            // New participant.
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
                // Sync pinned flag on every tile immediately so the pin icon
                // recolors in the same frame as the layout change, without
                // waiting for the next update_participants() call.
                for (std::size_t i = 0; i < tiles_.size(); ++i)
                    tiles_[i]->set_pinned(pinned_participant_ == tile_ids_[i]);
                if (relayout_requester_) relayout_requester_();
                else if (repaint_requester_) repaint_requester_();
            };

            // Resolve display name.
            std::string dname = p.user_id;
            if (display_name_provider_ && !p.user_id.empty())
            {
                const auto n = display_name_provider_(p.user_id);
                if (!n.empty()) dname = n;
            }
            if (dname.empty()) dname = p.participant_id;

            ParticipantTile::State s;
            s.participant_id = p.participant_id;
            s.user_id        = p.user_id;
            s.display_name   = dname;
            s.audio_muted    = p.is_audio_muted;
            s.video_muted    = p.is_video_muted;
            s.pinned         = (pinned_participant_ == p.participant_id);
            tile->set_state(std::move(s));

            tiles_.push_back(tile);
            tile_ids_.push_back(p.participant_id);
            // Mirror non-video fields in tile_states_ to track current metadata.
            ParticipantTile::State shadow;
            shadow.participant_id = p.participant_id;
            shadow.user_id        = p.user_id;
            shadow.display_name   = dname;
            shadow.audio_muted    = p.is_audio_muted;
            shadow.video_muted    = p.is_video_muted;
            shadow.pinned         = (pinned_participant_ == p.participant_id);
            tile_states_.push_back(std::move(shadow));
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

            ParticipantTile::State s;
            s.participant_id = p.participant_id;
            s.user_id        = p.user_id;
            s.display_name   = dname;
            s.audio_muted    = p.is_audio_muted;
            s.video_muted    = p.is_video_muted;
            s.pinned         = (pinned_participant_ == p.participant_id);
            // set_state replaces the entire State (including pending_rgba).
            // The tile reverts to avatar until the next push_video_frame (~1 frame).
            tile->set_state(std::move(s));
            // Keep shadow in sync for reference by other code.
            tile_states_[idx].user_id        = p.user_id;
            tile_states_[idx].display_name   = dname;
            tile_states_[idx].audio_muted    = p.is_audio_muted;
            tile_states_[idx].video_muted    = p.is_video_muted;
            tile_states_[idx].pinned         = (pinned_participant_ == p.participant_id);
        }
    }

    // A relayout is required — new tiles have bounds_ = {0,0,0,0} until
    // arrange() runs. push_video_frame() uses repaint_requester_ (no layout).
    if (relayout_requester_) relayout_requester_();
    else if (repaint_requester_) repaint_requester_();
}

void CallOverlayWidget::on_video_frame(const std::string& participant_id,
                                       std::uint32_t w, std::uint32_t h,
                                       const std::uint8_t* rgba,
                                       std::size_t rgba_size)
{
    const auto it = std::find(tile_ids_.begin(), tile_ids_.end(), participant_id);
    if (it == tile_ids_.end()) return;

    const std::size_t idx = static_cast<std::size_t>(it - tile_ids_.begin());
    // Update only the pixel buffer — avoids full State reconstruction (string
    // copies, video_image destruction) on every frame. push_video_frame()
    // fires repaint_requester_ internally via set_repaint_requester().
    tiles_[idx]->push_video_frame(w, h, rgba, rgba_size);
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
    const float total_w = 3.0f * kBtnSz + 2.0f * kBtnGap;
    const float start_x = controls_rect_.x + (controls_rect_.w - total_w) * 0.5f;
    const float btn_y   = controls_rect_.y + (kControlsH - kBtnSz) * 0.5f;

    if (mute_btn_)
        mute_btn_->arrange(ctx, {start_x, btn_y, kBtnSz, kBtnSz});
    if (video_btn_)
        video_btn_->arrange(ctx, {start_x + kBtnSz + kBtnGap, btn_y, kBtnSz, kBtnSz});
    if (hangup_btn_)
        hangup_btn_->arrange(ctx, {start_x + 2.0f * (kBtnSz + kBtnGap), btn_y, kBtnSz, kBtnSz});

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
        const int s = elapsed_seconds_ % 60;
        const int m = (elapsed_seconds_ / 60) % 60;
        const int h = elapsed_seconds_ / 3600;
        char buf[16];
        if (h > 0)
            std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
        else
            std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);

        tk::TextStyle ts{};
        ts.role = tk::FontRole::Body;
        auto layout = ctx.factory.build_text(std::string(buf), ts);
        if (layout)
        {
            const tk::Size sz = layout->measure();
            const float tx = duration_rect_.x + (duration_rect_.w - sz.w) * 0.5f;
            const float ty = duration_rect_.y + (duration_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*layout, {tx, ty}, kWhiteSoft);
        }
    }

    // ── Controls bar ───────────────────────────────────────────────────────
    ctx.canvas.fill_rect(controls_rect_, kCtrlBg);

    if (mute_btn_)
    {
        mute_btn_->paint(ctx);
        if (audio_muted_)
            mic_off_icon_.draw(ctx.canvas, ctx.factory, kMicOffSvg,
                               mute_btn_->bounds(), kBtnIconPx, kMutedRed);
        else
            mic_icon_.draw(ctx.canvas, ctx.factory, kMicSvg,
                           mute_btn_->bounds(), kBtnIconPx, kWhite);
    }

    if (video_btn_)
    {
        video_btn_->paint(ctx);
        if (video_muted_)
            video_off_icon_.draw(ctx.canvas, ctx.factory, kVideoOffSvg,
                                 video_btn_->bounds(), kBtnIconPx, kMutedRed);
        else
            video_icon_.draw(ctx.canvas, ctx.factory, kVideoSvg,
                             video_btn_->bounds(), kBtnIconPx, kWhite);
    }

    if (hangup_btn_)
    {
        hangup_btn_->paint(ctx);
        phone_off_icon_.draw(ctx.canvas, ctx.factory, kPhoneOffSvg,
                             hangup_btn_->bounds(), kBtnIconPx, kMutedRed);
    }

    // ── Expand / minimize button ───────────────────────────────────────────
    if (expand_btn_ && expand_btn_->visible())
    {
        expand_btn_->paint(ctx);
        if (mode_ == Mode::Docked)
            expand_icon_.draw(ctx.canvas, ctx.factory, kExpandSvg,
                              expand_btn_->bounds(), kExpandSz - 8.0f, kWhite);
        else
            minimize_icon_.draw(ctx.canvas, ctx.factory, kMinimizeSvg,
                                expand_btn_->bounds(), kExpandSz - 8.0f, kWhite);
    }

    // ── Pip / dock button ─────────────────────────────────────────────────────
    // In Popout mode the button docks the call back (show minimize icon);
    // otherwise it pops out to a secondary window (show pip icon).
    if (pip_btn_ && pip_btn_->visible())
    {
        pip_btn_->paint(ctx);
        if (mode_ == Mode::Popout)
            minimize_icon_.draw(ctx.canvas, ctx.factory, kMinimizeSvg,
                                pip_btn_->bounds(), kExpandSz - 8.0f, kWhite);
        else
            pip_icon_.draw(ctx.canvas, ctx.factory, kPipSvg,
                           pip_btn_->bounds(), kExpandSz - 8.0f, kWhite);
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
