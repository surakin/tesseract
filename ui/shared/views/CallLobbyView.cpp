#include "CallLobbyView.h"

#include "icons.h"
#include "tk/host.h"
#include "tk/i18n.h"

namespace tesseract::views
{

namespace
{
constexpr float kLobbyPadX        = 24.0f;
constexpr float kLobbyToggleBtnSz = 48.0f;
constexpr float kLobbyToggleBtnGap= 16.0f;
constexpr float kLobbyToggleRowH  = 64.0f;
constexpr float kLobbyActionBtnW  = 120.0f;
constexpr float kLobbyActionBtnH  = 40.0f;
constexpr float kLobbyActionBtnGap= 12.0f;
constexpr float kLobbyActionRowH  = 80.0f;
constexpr float kLobbyToggleIconPx= 22.0f;
} // namespace

CallLobbyView::CallLobbyView()
{
    auto tile = std::make_unique<ParticipantTile>();
    preview_tile_ = add_child(std::move(tile));

    auto mic = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    mic_btn_ = add_child(std::move(mic));
    mic_btn_->set_on_click(
        [this]
        {
            mic_enabled_ = !mic_enabled_;
            apply_preview_state_();
            if (repaint_requester_) repaint_requester_();
        });

    auto cam = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    cam_btn_ = add_child(std::move(cam));
    cam_btn_->set_on_click(
        [this]
        {
            cam_enabled_ = !cam_enabled_;
            if (cam_enabled_) start_camera_();
            else               stop_camera_();
            apply_preview_state_();
            if (repaint_requester_) repaint_requester_();
        });

    auto join = tk::create_widget<tk::Button>(this, tk::tr("Join call"),
                                              std::function<void()>{},
                                              tk::Button::Variant::Primary);
    join_btn_ = add_child(std::move(join));
    join_btn_->set_on_click(
        [this]
        {
            if (on_join)
                on_join(room_id_, slot_id_, /*audio_only=*/!cam_enabled_,
                        /*start_audio_muted=*/!mic_enabled_);
            close();
        });

    auto cancel = tk::create_widget<tk::Button>(this, tk::tr("Cancel"),
                                                std::function<void()>{},
                                                tk::Button::Variant::Subtle);
    cancel_btn_ = add_child(std::move(cancel));
    cancel_btn_->set_on_click(
        [this]
        {
            if (on_cancel) on_cancel();
            close();
        });
}

CallLobbyView::~CallLobbyView()
{
    stop_camera_();
}

void CallLobbyView::open(std::string room_id, std::string slot_id,
                         bool default_audio_only)
{
    room_id_ = std::move(room_id);
    slot_id_ = std::move(slot_id);
    mic_enabled_ = true;
    cam_enabled_ = !default_audio_only;
    open_ = true;
    set_visible(true);

    if (cam_enabled_) start_camera_();
    apply_preview_state_();
}

void CallLobbyView::close()
{
    if (!open_) return;
    open_ = false;
    stop_camera_();
    set_visible(false);
    room_id_.clear();
    slot_id_.clear();
}

void CallLobbyView::set_avatar_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    avatar_provider_ = std::move(fn);
    if (preview_tile_) preview_tile_->set_avatar_provider(avatar_provider_);
}

void CallLobbyView::set_repaint_requester(std::function<void()> fn)
{
    repaint_requester_ = std::move(fn);
    if (preview_tile_) preview_tile_->set_repaint_requester(repaint_requester_);
}

void CallLobbyView::start_camera_()
{
    if (capture_) return;

    capture_ = tk::VideoCapture::create();
    if (!capture_)
    {
        // No camera available — fall back to the avatar tile permanently.
        cam_enabled_ = false;
        return;
    }

    capture_->set_bgra_callback(
        [this](const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
        {
            auto buf = std::make_shared<std::vector<std::uint8_t>>(
                bgra, bgra + static_cast<std::size_t>(w) * h * 4);
            std::lock_guard<std::mutex> lk(frame_mu_);
            pending_frame_ = std::move(buf);
            frame_w_ = w;
            frame_h_ = h;
        });
    capture_->start();
}

void CallLobbyView::stop_camera_()
{
    if (capture_)
    {
        capture_->stop();
        capture_.reset();
    }
    std::lock_guard<std::mutex> lk(frame_mu_);
    pending_frame_.reset();
    frame_w_ = frame_h_ = 0;
}

void CallLobbyView::apply_preview_state_()
{
    if (!preview_tile_) return;

    ParticipantTile::State s;
    s.participant_id = "self";
    s.user_id         = local_user_id_;
    s.is_self          = true;
    s.audio_muted      = !mic_enabled_;
    s.video_muted      = !cam_enabled_;
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        s.pending_bgra = pending_frame_;
        s.pending_w    = frame_w_;
        s.pending_h    = frame_h_;
    }
    preview_tile_->set_state(std::move(s));
}

tk::Size CallLobbyView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void CallLobbyView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!open_) return;

    const float action_y  = bounds.y + bounds.h - kLobbyActionRowH;
    const float toggle_y  = action_y - kLobbyToggleRowH;
    const float preview_y = bounds.y;
    const float preview_h = std::max(0.0f, toggle_y - preview_y);

    if (preview_tile_)
        preview_tile_->arrange(ctx, {bounds.x + kLobbyPadX, preview_y,
                                     bounds.w - kLobbyPadX * 2.0f, preview_h});

    // Mic/camera toggle row, centred.
    const float toggle_total_w = kLobbyToggleBtnSz * 2.0f + kLobbyToggleBtnGap;
    float tx = bounds.x + (bounds.w - toggle_total_w) * 0.5f;
    const float toggle_btn_y = toggle_y + (kLobbyToggleRowH - kLobbyToggleBtnSz) * 0.5f;
    if (mic_btn_)
    {
        mic_btn_->arrange(ctx, {tx, toggle_btn_y, kLobbyToggleBtnSz, kLobbyToggleBtnSz});
        tx += kLobbyToggleBtnSz + kLobbyToggleBtnGap;
    }
    if (cam_btn_)
        cam_btn_->arrange(ctx, {tx, toggle_btn_y, kLobbyToggleBtnSz, kLobbyToggleBtnSz});

    // Join/Cancel row, centred, at the bottom.
    const float action_total_w = kLobbyActionBtnW * 2.0f + kLobbyActionBtnGap;
    float ax = bounds.x + (bounds.w - action_total_w) * 0.5f;
    const float action_btn_y = action_y + (kLobbyActionRowH - kLobbyActionBtnH) * 0.5f;
    if (cancel_btn_)
    {
        cancel_btn_->arrange(ctx, {ax, action_btn_y, kLobbyActionBtnW, kLobbyActionBtnH});
        ax += kLobbyActionBtnW + kLobbyActionBtnGap;
    }
    if (join_btn_)
        join_btn_->arrange(ctx, {ax, action_btn_y, kLobbyActionBtnW, kLobbyActionBtnH});
}

void CallLobbyView::paint(tk::PaintCtx& ctx)
{
    if (!open_ || bounds_.h <= 0.0f) return;

    // Themed panel background (not a fixed dark scrim) so the ordinary
    // Primary/Subtle button styling — designed for the app's own light/dark
    // palette, not a permanently-black backdrop — stays legible in both
    // themes.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Pull in the latest camera frame (captured on a background thread) and
    // hand it to the preview tile here, on the UI thread — mirrors
    // CameraWidget's paint-time frame pickup.
    if (cam_enabled_)
    {
        std::shared_ptr<std::vector<std::uint8_t>> frame;
        std::uint32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(frame_mu_);
            frame = pending_frame_;
            w = frame_w_;
            h = frame_h_;
        }
        if (frame && preview_tile_)
            preview_tile_->push_video_frame(w, h, std::move(frame));
    }

    if (preview_tile_) preview_tile_->paint(ctx);

    if (mic_btn_)
    {
        if (mic_enabled_)
        {
            mic_btn_->set_icon(kMicSvg, kLobbyToggleIconPx);
            mic_btn_->set_icon_color_override(std::nullopt);
        }
        else
        {
            mic_btn_->set_icon(kMicOffSvg, kLobbyToggleIconPx);
            mic_btn_->set_icon_color_override(ctx.theme.palette.destructive);
        }
        mic_btn_->set_accessible_name(mic_enabled_ ? tk::tr("Mute microphone")
                                                    : tk::tr("Unmute microphone"));
        mic_btn_->paint(ctx);
    }
    if (cam_btn_)
    {
        if (cam_enabled_)
        {
            cam_btn_->set_icon(kVideoSvg, kLobbyToggleIconPx);
            cam_btn_->set_icon_color_override(std::nullopt);
        }
        else
        {
            cam_btn_->set_icon(kVideoOffSvg, kLobbyToggleIconPx);
            cam_btn_->set_icon_color_override(ctx.theme.palette.destructive);
        }
        cam_btn_->set_accessible_name(cam_enabled_ ? tk::tr("Turn camera off")
                                                    : tk::tr("Turn camera on"));
        cam_btn_->paint(ctx);
    }
    if (cancel_btn_) cancel_btn_->paint(ctx);
    if (join_btn_)   join_btn_->paint(ctx);

    // Keep repainting while the camera is live so the preview stays smooth.
    if (cam_enabled_ && ctx.host)
        ctx.host->request_repaint();
}

bool CallLobbyView::on_pointer_down(tk::Point /*local*/)
{
    // Absorb any click on the lobby that no child (preview tile, toggle,
    // Join/Cancel) claimed, so it can't fall through to the timeline/
    // compose bar it covers.
    return open_;
}

bool CallLobbyView::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/,
                             bool /*is_touchpad*/)
{
    return open_;
}

} // namespace tesseract::views
