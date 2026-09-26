#pragma once

// CallLobbyView — pre-call confirmation step shown before joining a MatrixRTC
// call: a self camera preview (reusing ParticipantTile), mic/camera toggles,
// and Join/Cancel buttons.
//
// Mounted by RoomView below the header, covering the message list, compose
// bar, and any banners/search bar/docked call panel in that region — see
// RoomView::arrange()'s call_lobby_ handling. Every "start a call" entry
// point (auto-join on room switch, the call banner, the header call button)
// opens this instead of joining directly — see ShellBase::request_call_().
//
// Camera handling mirrors CameraWidget: owns its own tk::VideoCapture,
// started/stopped independently of any real CallSession (none exists yet
// at the lobby stage). The mic toggle is local-only UI state pre-join —
// there is no CallSession to mute and no audio level meter.
//
// Camera failures (none present, busy in another app, permission denied)
// switch the camera toggle off and show the reason under the preview;
// turning the camera back on retries with a fresh capture.

#include "ParticipantTile.h"

#include "tk/controls.h"
#include "tk/video_capture.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tesseract::views
{

class CallLobbyView : public tk::Widget
{
public:
    CallLobbyView();
    ~CallLobbyView() override;

    // Opens the lobby for room_id/slot_id. default_audio_only seeds the
    // camera toggle's initial state (off when true, on otherwise) — passed
    // by the "Audio call" header menu item; every other entry point passes
    // false so the camera starts on.
    void open(std::string room_id, std::string slot_id, bool default_audio_only);
    // Stops capture and hides the view. Safe to call when already closed.
    void close();
    bool is_open() const { return open_; }
    // Last camera failure since the camera was (re)enabled; None when fine.
    tk::VideoCapture::Error camera_error() const { return cam_error_; }
    bool camera_enabled() const { return cam_enabled_; }
    // The room this lobby is currently showing (empty while closed) — lets
    // callers distinguish "still the same room" repeat calls from an actual
    // switch away, since is_open() alone can't tell those apart.
    const std::string& room_id() const { return room_id_; }

    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& user_id)> fn);
    void set_local_user_id(std::string id) { local_user_id_ = std::move(id); }
    void set_repaint_requester(std::function<void()> fn);

    // room_id, slot_id, audio_only (camera was deliberately left off),
    // start_audio_muted (mic was left off), start_video_muted (the camera
    // failed — join as a video call with video off so it can be retried).
    std::function<void(const std::string& room_id, const std::string& slot_id,
                       bool audio_only, bool start_audio_muted,
                       bool start_video_muted)> on_join;
    std::function<void()> on_cancel;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad) override;

private:
    void start_camera_();
    void stop_camera_();
    void apply_preview_state_();
    void set_camera_error_(tk::VideoCapture::Error err);

    bool        open_ = false;
    std::string room_id_;
    std::string slot_id_;
    std::string local_user_id_;

    bool mic_enabled_ = true;
    bool cam_enabled_ = true;
    tk::VideoCapture::Error cam_error_ = tk::VideoCapture::Error::None;

    std::unique_ptr<tk::VideoCapture>          capture_;
    std::mutex                                 frame_mu_;
    std::shared_ptr<std::vector<std::uint8_t>> pending_frame_;
    std::uint32_t                              frame_w_ = 0, frame_h_ = 0;

    ParticipantTile* preview_tile_ = nullptr;
    tk::Button*      mic_btn_      = nullptr;
    tk::Button*      cam_btn_      = nullptr;
    tk::Button*      join_btn_     = nullptr;
    tk::Button*      cancel_btn_   = nullptr;
    tk::Label*       cam_error_label_ = nullptr;

    std::function<const tk::Image*(const std::string&)> avatar_provider_;
    std::function<void()>                                repaint_requester_;
};

} // namespace tesseract::views
