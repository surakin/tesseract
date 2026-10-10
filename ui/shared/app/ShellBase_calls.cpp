#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/Launch.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/MediaPlaybackHub.h"
#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/SearchBackend.h"
#include "app/SlashCommands.h"
#include "app/shell_helpers.h"
#include "app/UnreadPrefetch.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/JoinRoomView.h"
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/InviteDialog.h"
#include "views/text_util.h"
#include "views/RoomSearchBar.h"
#include "views/SettingsView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "tk/image_sniff.h"
#include "views/html_spans.h"
#include "views/image_pack_order.h"
#include "views/map_tiles.h"
#include "views/pronoun_utils.h"
#include "views/thread_unread.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/secret_store.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace tesseract
{

void ShellBase::handle_rtc_invitation_ui_(std::string /*room_id*/,
                                           std::string /*slot_id*/,
                                           std::string /*caller_user_id*/,
                                           std::string /*call_intent*/,
                                           std::uint64_t /*lifetime_ms*/,
                                           std::string /*notification_event_id*/)
{
    // The banner follows the room's call state (RoomInfo::call_members), which
    // the same sync tick updates; nothing event-specific to show here.
    refresh_call_banners_();
}

void ShellBase::refresh_call_banners_()
{
    if (room_view_ && !current_room_id_.empty())
        refresh_call_banner_(room_view_, current_room_id_);
    for (auto& [rid, w] : active_account_popouts_())
        refresh_call_banner_(w->room_view(), rid);
}

void ShellBase::refresh_call_banner_(views::RoomView* rv, const std::string& room_id)
{
    if (!rv)
        return;
    const auto* r = room_by_id_(room_id);
    const bool bridged = r && r->is_bridged && !r->bridge_overridden;
    const bool joined_here = call_session_ && call_session_->room_id() == room_id;
    if (!r || !server_info_.supports_calls || bridged || joined_here ||
        r->call_members.empty())
    {
        rv->clear_call_banner();
        call_banner_rosters_.erase(room_id);
        return;
    }

    auto& roster = call_banner_rosters_[room_id];
    if (roster.ids != r->call_members)
    {
        roster = {};
        roster.ids = r->call_members;
        std::unordered_map<std::string, tesseract::RoomMember> by_id;
        if (client_)
        {
            for (auto& m : client_->get_room_members(room_id))
            {
                if (std::find(roster.ids.begin(), roster.ids.end(), m.user_id) !=
                    roster.ids.end())
                {
                    by_id.emplace(m.user_id, std::move(m));
                }
            }
        }
        for (const auto& id : roster.ids)
        {
            std::string name;
            auto it = by_id.find(id);
            if (it != by_id.end())
            {
                name = it->second.display_name;
                if (!it->second.avatar_url.empty())
                    roster.avatar_urls[id] = it->second.avatar_url;
            }
            if (name.empty())
            {
                const auto colon = id.find(':');
                name = (!id.empty() && id[0] == '@' && colon != std::string::npos)
                           ? id.substr(1, colon - 1)
                           : id;
            }
            roster.names.emplace_back(id, std::move(name));
        }
    }

    // Fetch any avatar not decoded yet; the redraw when it lands (via
    // on_media_bytes_ready_) picks it up through the provider below.
    for (const auto& [uid, mxc] : roster.avatar_urls)
        ensure_user_avatar_(mxc);

    std::vector<views::CallBanner::Member> members;
    members.reserve(roster.names.size());
    for (const auto& [uid, name] : roster.names)
        members.push_back({uid, name});

    rv->set_call_banner_avatar_provider(
        [this, room_id](const std::string& user_id) -> const tk::Image*
        {
            auto rit = call_banner_rosters_.find(room_id);
            if (rit == call_banner_rosters_.end())
                return nullptr;
            auto ait = rit->second.avatar_urls.find(user_id);
            if (ait == rit->second.avatar_urls.end())
                return nullptr;
            return avatar_image_(ait->second);
        });
    // Join is disabled while the user is in a different call.
    rv->set_call_banner(room_id, r->call_intent, std::move(members), call_session_ == nullptr);
}

void ShellBase::handle_rtc_video_frame_ui_(
    std::uint64_t /*session_id*/,
    const std::string& participant_id,
    std::uint32_t width,
    std::uint32_t height,
    std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    if (auto* ov = active_call_overlay_())
        ov->on_video_frame(participant_id, width, height, std::move(bgra));
}

void ShellBase::handle_rtc_screen_frame_ui_(
    std::uint64_t /*session_id*/,
    const std::string& participant_id,
    std::uint32_t width,
    std::uint32_t height,
    std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    if (auto* ov = active_call_overlay_())
        ov->on_screen_frame(participant_id + ":screen", width, height, std::move(bgra));
}

void ShellBase::start_screen_share_()
{
    if (!call_session_)
        return;
    auto cap = tk::ScreenCapture::create();
    if (!cap)
        return;
    auto sources = cap->enumerate_sources();
    if (sources.empty())
        return;

    if (sources.size() == 1)
    {
        // Only one source (single monitor, no windows) — skip the picker.
        do_start_screen_share_(sources[0].id, std::move(cap));
        return;
    }

    // Multiple sources: show the picker modal. Transfer cap ownership into the
    // lambda so it is ready to use immediately when the user selects a source.
    auto cap_ptr = std::make_shared<std::unique_ptr<tk::ScreenCapture>>(std::move(cap));
    if (main_app_)
    {
        auto* picker = main_app_->mount_screen_picker(
            sources,
            [this, cap_ptr](std::string source_id)
            {
                // Join the thumbnail worker before touching *cap_ptr: it may
                // still be mid-capture on this same ScreenCapture instance
                // (e.g. DXGI only allows one duplication handle per monitor
                // at a time), and it also reads/moves-from *cap_ptr without
                // synchronization otherwise.
                if (screen_thumb_worker_.joinable())
                    screen_thumb_worker_.join();
                if (*cap_ptr)
                    do_start_screen_share_(source_id, std::move(*cap_ptr));
            },
            [this]()
            {
                // User cancelled — reset the button state in the overlay.
                if (auto* ov = active_call_overlay_())
                    ov->set_screen_sharing(false);
            });

        // Populate tile thumbnails off the UI thread: enumeration is cheap,
        // but a real capture per source (PrintWindow per window, a DXGI
        // duplication per monitor) is not — doing it inline here would
        // visibly hitch the UI while the picker is opening. The picker mounts
        // immediately with plain tiles; thumbnails fill in as they complete.
        if (picker)
        {
            if (screen_thumb_worker_.joinable())
                screen_thumb_worker_.join(); // stale worker from a previous picker
            auto shell_alive   = weak_flag();
            auto picker_alive  = picker->alive_token();
            screen_thumb_worker_ = std::thread(
                [this, shell_alive, picker_alive, picker, cap_ptr, sources]()
                {
                    for (std::size_t i = 0; i < sources.size() && !shell_alive.expired(); ++i)
                    {
                        // Picker was unmounted (selected or cancelled) — stop
                        // as soon as the in-flight capture below finishes, so
                        // the join() on the selection path (see above, which
                        // must complete before the real share can start on
                        // this same ScreenCapture instance) doesn't wait for
                        // every remaining source.
                        if (picker_alive.expired())
                            break;
                        if (!*cap_ptr)
                            return;
                        std::vector<std::uint8_t> rgba;
                        std::uint32_t w = 0, h = 0;
                        if (!(*cap_ptr)->capture_thumbnail(sources[i].id, rgba, w, h))
                            continue;
                        post_to_ui_alive_(
                            [picker_alive, picker, i, rgba = std::move(rgba), w, h, this]() mutable
                            {
                                if (picker_alive.lock())
                                {
                                    picker->set_thumbnail(i, std::move(rgba), w, h);
                                    request_repaint_();
                                }
                            });
                    }
                });
        }
    }
}

void ShellBase::do_start_screen_share_(const std::string& source_id,
                                        std::unique_ptr<tk::ScreenCapture> cap)
{
    // Publish the LiveKit track first (blocks until SDP round-trip completes)
    // so screen_source is set before the capture callback fires. Starting
    // capture before publish_track finishes causes all early frames to be
    // silently dropped inside push_screen_frame_i420 (screen_source == None).
    // Must check the result: if the publish itself fails, screen_source is
    // never set, so arming the capture anyway would silently drop every
    // frame forever with no visible symptom beyond a black preview tile.
    if (auto res = call_session_->start_screen_share(); !res)
    {
        fprintf(stderr, "[screenshare] failed to publish screen-share track: %s\n",
                res.message.c_str());
        if (auto* ov = active_call_overlay_())
            ov->set_screen_sharing(false);
        return;
    }
    cap->set_source(source_id);
    cap->set_callback([this](const tk::ScreenCapture::Frame& f) {
        if (client_)
            client_->rtc_push_screen_frame_i420(
                f.y, f.u, f.v, f.width, f.height,
                f.stride_y, f.stride_u, f.stride_v);
    });
    cap->start();
    screen_capture_ = std::move(cap);
    if (auto* ov = active_call_overlay_())
        ov->set_screen_sharing(true);
}

void ShellBase::stop_screen_share_()
{
    // The picker is always mounted on main_app_ (it has no overlay stack of
    // its own when the call is popped out into a CallWindowBase — that window
    // only hosts the CallOverlayWidget). So if the user re-toggles the share
    // button off before picking a source — the only way to cancel when the
    // picker isn't even visible in a popout window — it must be torn down
    // here too, not just when its own Cancel button fires.
    if (main_app_ && main_app_->screen_picker_open())
        main_app_->unmount_screen_picker();

    if (screen_capture_)
    {
        screen_capture_->stop();
        screen_capture_.reset();
    }
    if (call_session_)
        call_session_->stop_screen_share();
    if (auto* ov = active_call_overlay_())
        ov->set_screen_sharing(false);
}

void ShellBase::notify_reply_failed_(const std::string& user_id,
                                     const std::string& room_id,
                                     std::string reason)
{
    auto sess = account_manager_.find(user_id);
    if (!sess || !sess->notifier)
        return;

    tesseract::Notification n;
    n.room_id = room_id;
    n.sender = tk::tr("Message not sent");
    n.body = std::move(reason);
    sess->notifier->notify(n);
}

void ShellBase::push_call_audio_bgnd_(const std::int16_t* samples,
                                       std::size_t sample_count,
                                       std::uint32_t sample_rate,
                                       std::uint32_t num_channels)
{
    std::lock_guard<std::mutex> lock(call_audio_mutex_);
    if (call_audio_output_)
        call_audio_output_->push_frame(samples, sample_count, sample_rate, num_channels);
}

void ShellBase::start_call_video_capture_()
{
    if (call_video_capture_)
        return;
    const std::uint64_t gen = ++call_video_capture_gen_;
    // Errors can fire synchronously from start() or later from a capture
    // thread; either way handle them on a fresh UI-thread turn.
    auto on_error = [this, gen](tk::VideoCapture::Error err)
    {
        post_to_ui_alive_(
            [this, gen, err]
            {
                if (gen == call_video_capture_gen_)
                    handle_call_video_error_(err);
            });
    };

    auto vc = tk::VideoCapture::create();
    if (!vc)
    {
        on_error(tk::VideoCapture::Error::NoDevice);
        return;
    }
    vc->set_callback(
        [this](const tk::VideoCapture::Frame& f)
        {
            client_->rtc_push_video_frame_i420(
                f.y, f.u, f.v, f.width, f.height,
                f.stride_y, f.stride_u, f.stride_v);
        });
    vc->set_error_callback(on_error);
    call_video_capture_ = std::move(vc);
    call_video_capture_->start();
}

void ShellBase::stop_call_video_capture_()
{
    ++call_video_capture_gen_;
    if (call_video_capture_)
    {
        call_video_capture_->stop();
        call_video_capture_.reset();
    }
}

void ShellBase::handle_call_video_error_(tk::VideoCapture::Error err)
{
    if (!call_session_)
        return;
    stop_call_video_capture_();
    call_session_->mute_video(true);
    call_overlay_state_.video_muted = true;
    if (auto* ov = active_call_overlay_())
        ov->set_video_muted(true);
    show_status_message_(tk::VideoCapture::describe(err));
}

views::CallOverlayWidget::Mode ShellBase::overlay_mode_from_settings_() const
{
    switch (Settings::instance().call_overlay_mode)
    {
    case Settings::CallOverlayMode::DockedExpanded:
        return views::CallOverlayWidget::Mode::DockedExpanded;
    case Settings::CallOverlayMode::Floating:
        return views::CallOverlayWidget::Mode::Floating;
    case Settings::CallOverlayMode::Popout:
        return views::CallOverlayWidget::Mode::Popout;
    default:
        return views::CallOverlayWidget::Mode::Docked;
    }
}

// start_call creates a CallSession, wires audio (and video if a camera is
// available) capture routing, and calls rtc_start_call on the client.
// No-op when a call is already active. start_audio_muted mutes the mic
// immediately after joining — the lobby's mic toggle feeds this.
// start_video_muted joins as a video call with the camera off (video
// button still available) — used when the lobby's camera failed, so the
// user can retry once the device is free instead of being stuck in an
// audio-only call.
void ShellBase::start_call(const std::string& room_id, const std::string& slot_id,
                           bool audio_only, bool start_audio_muted,
                           bool start_video_muted)
{
    if (call_session_ || !client_)
        return;

    auto result = client_->rtc_start_call(room_id, slot_id, audio_only);
    if (!result.ok)
    {
        show_status_message_(tk::trf(tk::tr("Call failed: {0}"), {result.message}));
        return;
    }

    call_session_ = std::make_unique<CallSession>(client_, room_id, slot_id);
    call_audio_output_ = make_call_audio_output_();
    if (start_audio_muted)
        call_session_->mute_audio(true);

    // Initialise overlay state for this call. elapsed_seconds starts at 0;
    // audio_only/start_audio_muted are only known here, so they must be
    // captured in the struct now.
    call_overlay_state_ = {};
    call_overlay_state_.show_video_button = !audio_only;
    call_overlay_state_.audio_muted       = start_audio_muted;
    call_overlay_state_.video_muted       = !audio_only && start_video_muted;
    call_overlay_state_.local_user_id     = my_user_id_;

    if (!audio_only && !start_video_muted)
    {
        // The camera track is published muted; this asks for it to go live
        // on the first real frame, so a missing or busy camera never sends
        // placeholder video.
        start_call_video_capture_();
        call_session_->mute_video(false);
    }
    else
    {
        // Audio-only or camera off: keep the video track muted.
        call_session_->mute_video(true);
    }

    // Mount + wire the call overlay in the mode resolved from saved settings.
    on_call_overlay_mode_requested_(overlay_mode_from_settings_());

    // Restore saved float position; hide video button for audio-only calls.
    if (auto* ov = active_call_overlay_())
    {
        ov->set_float_position(Settings::instance().call_overlay_float_x,
                               Settings::instance().call_overlay_float_y);
        if (audio_only)
            ov->set_show_video_button(false);
        if (start_audio_muted)
            ov->set_audio_muted(true);
    }

    // The user is now in this call: hides its banner (and disables Join on
    // other rooms' banners), wherever it shows, main window or a pop-out.
    refresh_call_banners_();

    // Flip the call button to active state in main window and all pop-outs.
    if (room_view_ && room_view_->header())
        room_view_->header()->set_call_active(true);
    for (auto& w : owned_secondary_windows_)
    {
        if (w->room_view() && w->room_view()->header())
            w->room_view()->header()->set_call_active(true);
    }
}

CallRoomAction decide_call_room_action(bool has_active_call,
                                       bool active_call_room_is_new_room,
                                       bool new_room_is_call_room,
                                       bool call_auto_floated,
                                       bool overlay_is_docked)
{
    if (!has_active_call)
        return new_room_is_call_room ? CallRoomAction::AutoJoin
                                     : CallRoomAction::None;

    if (active_call_room_is_new_room)
        return call_auto_floated ? CallRoomAction::AutoRestore
                                 : CallRoomAction::None;

    if (new_room_is_call_room)
        return CallRoomAction::LeaveAndJoin;

    return overlay_is_docked ? CallRoomAction::AutoFloat : CallRoomAction::NoOp;
}

// Opens room_id's pre-call lobby (camera preview + mic/cam toggles +
// Join/Cancel) instead of joining directly — the single seam every
// "start a call" entry point (auto-join, the call banner, the header
// call button, LeaveAndJoin) now goes through. Wires the lobby's
// on_join to start_call() and falls back to start_call() directly if
// room_id isn't currently displayed in any window (defensive; shouldn't
// happen since callers only ever target the room being viewed).
void ShellBase::request_call_(const std::string& room_id, const std::string& slot_id,
                              bool audio_only)
{
    views::RoomView* rv = room_view_for_room_(room_id);
    views::CallLobbyView* lobby = rv ? rv->call_lobby() : nullptr;
    if (!lobby)
    {
        // Defensive fallback — shouldn't happen since callers only ever
        // target the room currently being viewed.
        start_call(room_id, slot_id, audio_only);
        return;
    }

    lobby->set_local_user_id(my_user_id_);

    if (room_id == current_room_id_)
    {
        lobby->set_repaint_requester([this] { request_repaint_(); });
    }
    else if (RoomWindowBase* win = find_event_secondary_(room_id))
    {
        lobby->set_repaint_requester([win] { if (win) win->request_relayout(); });
    }

    lobby->set_avatar_provider(
        [this, room_id](const std::string& user_id) -> const tk::Image*
        {
            if (!client_) return nullptr;
            const auto members = client_->get_room_members(room_id);
            for (const auto& mem : members)
            {
                if (mem.user_id == user_id && !mem.avatar_url.empty())
                    return avatar_image_(mem.avatar_url);
            }
            return nullptr;
        });

    lobby->on_join = [this](const std::string& rid, const std::string& sid,
                            bool ao, bool muted, bool video_muted)
    {
        // If a different room's call is still active (LeaveAndJoin), end it
        // before joining — start_call() no-ops while call_session_ is
        // already set. Confirmed only here, on actual Join, not on the
        // room switch that opened this lobby.
        if (call_session_ && call_session_->room_id() != rid)
            end_call();
        start_call(rid, sid, ao, muted, video_muted);
    };
    lobby->on_cancel = [] {};

    lobby->open(room_id, slot_id, audio_only);
}

void ShellBase::handle_call_room_navigation_()
{
    const auto* new_room = room_by_id_(current_room_id_);
    // Not in rooms_ yet (just created/joined) — retried once from the rooms
    // update that delivers it, see pending_call_room_nav_id_.
    pending_call_room_nav_id_ = new_room ? std::string() : current_room_id_;
    if (!new_room || new_room->is_space)
        return;

    auto* ov = active_call_overlay_();
    const bool overlay_docked =
        ov && (ov->mode() == views::CallOverlayWidget::Mode::Docked ||
               ov->mode() == views::CallOverlayWidget::Mode::DockedExpanded);

    switch (decide_call_room_action(
        /*has_active_call=*/call_session_ != nullptr,
        /*active_call_room_is_new_room=*/
        call_session_ && call_session_->room_id() == current_room_id_,
        /*new_room_is_call_room=*/new_room->is_call_room,
        call_auto_floated_, overlay_docked))
    {
    case CallRoomAction::AutoJoin:
        // First switch into a call room with no active call: open the
        // lobby instead of joining immediately.
        request_call_(current_room_id_);
        break;
    case CallRoomAction::AutoRestore:
        // Returning to the room hosting the active call: undo whatever
        // auto-float was forced when the user left it, restoring their
        // real (non-floating) mode preference.
        call_auto_floated_ = false;
        on_call_overlay_mode_requested_(overlay_mode_from_settings_(),
                                        /*persist=*/false);
        break;
    case CallRoomAction::LeaveAndJoin:
        // Switching directly into another call room (whether the previous
        // call was auto-joined or manually joined via the "call in
        // progress" banner): open the new room's lobby rather than ending
        // the old call immediately — the old call only ends once the user
        // actually confirms Join (see request_call_()'s on_join, which
        // ends whatever call is active in a different room before
        // starting the new one). Cancel leaves the original call untouched.
        request_call_(current_room_id_);
        break;
    case CallRoomAction::AutoFloat:
        // Leaving the active call's room for an ordinary room: float it
        // rather than leave it docked to a room no longer shown.
        call_auto_floated_ = true;
        on_call_overlay_mode_requested_(views::CallOverlayWidget::Mode::Floating,
                                        /*persist=*/false);
        break;
    case CallRoomAction::None:
    case CallRoomAction::NoOp:
        break;
    }

    if (auto* ov2 = active_call_overlay_())
        ov2->set_room_active(call_session_ &&
                             call_session_->room_id() == current_room_id_);
}

void ShellBase::end_call()
{
    // Clear active indicator before tearing down session.
    if (room_view_ && room_view_->header())
        room_view_->header()->set_call_active(false);
    for (auto& w : owned_secondary_windows_)
    {
        if (w->room_view() && w->room_view()->header())
            w->room_view()->header()->set_call_active(false);
    }

    stop_call_video_capture_();
    if (screen_capture_)
    {
        screen_capture_->stop();
        screen_capture_.reset();
    }
    // cancel() stops capture without firing on_stopped (which would try to send a voice msg).
    if (capture_ && capture_->is_recording())
        capture_->cancel();
    if (call_session_)
    {
        call_session_->hang_up();
        call_session_.reset();
    }
    if (call_window_)
    {
        call_window_->on_window_closed = nullptr;
        call_window_->close_window();
        call_window_.release()->schedule_delete(); // defer Qt delete past event handler
    }
    if (main_app_) main_app_->unmount_call_overlay();
    call_overlay_state_ = {};
    call_auto_floated_ = false;
    // The call may still be running for others: bring its banner back.
    refresh_call_banners_();
}

void ShellBase::handle_rtc_participant_joined_ui_(std::uint64_t session_id,
                                                   RtcParticipantInfo info)
{
    if (!call_session_)
        return;
    call_session_->set_session_id(session_id);
    call_session_->on_participant_joined(info);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_participant_left_ui_(std::uint64_t session_id,
                                                 std::string participant_id)
{
    if (!call_session_ || call_session_->session_id() != session_id)
        return;
    call_session_->on_participant_left(participant_id);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_participant_updated_ui_(std::uint64_t session_id,
                                                    RtcParticipantInfo info)
{
    if (!call_session_ || call_session_->session_id() != session_id)
        return;
    call_session_->on_participant_updated(info);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_session_ended_ui_(std::uint64_t session_id,
                                              std::string reason)
{
    if (!call_session_)
        return;
    // Accept the end event if session_id matches, OR if session_id_ is still 0
    // (no participant joined yet — the session never got its id confirmed).
    if (call_session_->session_id() != 0 &&
        call_session_->session_id() != session_id)
        return;

    // Surface disconnect reason for non-normal ends.
    if (!reason.empty()
        && reason != "hangup"
        && reason != "user_action"
        && reason != "switch_device")
    {
        std::string msg;
        if (reason == "ice_failed" || reason == "dtls_failed" || reason == "network_error")
            msg = tk::tr("Call disconnected due to a network error.");
        else if (reason == "media_error" || reason == "transport_failure")
            msg = tk::tr("Call ended due to a media error.");
        else if (reason == "codec_mismatch" || reason == "unsupported_features")
            msg = tk::tr("This call is not supported on your device.");
        else if (reason == "encryption_error")
            msg = tk::tr("Call ended due to an encryption error.");
        else
            msg = tk::tr("The call ended unexpectedly.");
        show_status_message_(std::move(msg));
    }

    call_session_->on_session_ended({});
    stop_call_video_capture_();
    {
        std::lock_guard<std::mutex> lock(call_audio_mutex_);
        call_audio_output_.reset();
    }
    if (capture_ && capture_->is_recording())
        capture_->cancel();
    call_session_.reset();
    if (call_window_)
    {
        call_window_->on_window_closed = nullptr;
        call_window_->close_window();
        call_window_.release()->schedule_delete(); // defer Qt delete past event handler
    }
    if (main_app_) main_app_->unmount_call_overlay();
    call_auto_floated_ = false;
    if (room_view_ && room_view_->header())
        room_view_->header()->set_call_active(false);
    for (auto& w : owned_secondary_windows_)
    {
        if (w->room_view() && w->room_view()->header())
            w->room_view()->header()->set_call_active(false);
    }
    // The call may still be running for others: bring its banner back.
    refresh_call_banners_();
}

views::CallOverlayWidget* ShellBase::active_call_overlay_() const
{
    if (call_window_)
        return call_window_->call_overlay_widget();
    if (main_app_)
        return main_app_->call_panel_for_room();
    return nullptr;
}

// Tear down the current overlay, switch to the requested mode, remount,
// rewire all callbacks, and persist the new mode to Settings. Docked/
// DockedExpanded are clamped to Floating if the call's room isn't
// current_room_id_ (the UI already hides those mode options in that
// state, but the state machine doesn't rely on that). Pass persist=false
// for transitions Tesseract itself drives (auto-float on room-leave,
// auto-restore on room-return) so they don't clobber the user's actual
// CallOverlayMode preference in Settings.
void ShellBase::on_call_overlay_mode_requested_(views::CallOverlayWidget::Mode m,
                                                bool persist)
{
    if (!main_app_ || !call_session_)
        return;

    // Docked/DockedExpanded only make sense while the call's room is the one
    // currently being viewed; the UI already hides those options otherwise,
    // but callers (e.g. the popout window's on_window_closed) don't all
    // check this, so clamp defensively rather than dock into a room that
    // isn't shown.
    if ((m == views::CallOverlayWidget::Mode::Docked ||
         m == views::CallOverlayWidget::Mode::DockedExpanded) &&
        call_session_->room_id() != current_room_id_)
        m = views::CallOverlayWidget::Mode::Floating;

    // Snapshot mutable overlay state into the persistent struct before teardown.
    if (auto* ov = active_call_overlay_())
        call_overlay_state_ = ov->snapshot();

    // Tear down whatever is currently active.
    if (call_window_)
    {
        // Null the callback before closing to prevent re-entrancy (GTK4
        // fires "destroy" synchronously; Qt6 would re-enter closeEvent).
        call_window_->on_window_closed = nullptr;
        call_window_->close_window();
        // Release ownership and defer destruction. Deleting a QWidget (or
        // GTK window) synchronously while inside one of its event handlers
        // (closeEvent / mouseReleaseEvent) causes Qt to crash during its
        // own post-event cleanup — schedule_delete() defers via deleteLater()
        // on Qt6, fires delete immediately on other platforms.
        call_window_.release()->schedule_delete();
    }
    main_app_->unmount_call_overlay();

    // Provider lambdas reused for all non-Popout mount calls.
    auto post_delayed_fn = [this](int ms, std::function<void()> fn)
    {
        post_to_ui_after_(ms, std::move(fn));
    };
    // repaint_fn is overridden for Popout below — the popout window lives in a
    // separate OS surface; calling request_repaint_() would repaint the main
    // window instead, so video frames would never update the popout.
    auto repaint_fn = [this] { request_repaint_(); };
    auto avatar_fn  = [this](const std::string& user_id) -> const tk::Image*
    {
        if (call_session_ && client_)
        {
            const auto members = client_->get_room_members(call_session_->room_id());
            for (const auto& mem : members)
            {
                if (mem.user_id == user_id && !mem.avatar_url.empty())
                    return avatar_image_(mem.avatar_url);
            }
        }
        return nullptr;
    };
    auto name_fn = [this](const std::string& user_id) -> std::string
    {
        // Primary: room member list for the active call room.
        if (call_session_ && client_)
        {
            const auto members = client_->get_room_members(call_session_->room_id());
            for (const auto& mem : members)
            {
                if (mem.user_id == user_id && !mem.display_name.empty())
                    return mem.display_name;
            }
        }
        // Secondary: global known-users roster.
        auto it = known_users_.find(user_id);
        if (it != known_users_.end() && !it->second.display_name.empty())
            return it->second.display_name;
        // Fallback: localpart of the Matrix ID (@alice:server → alice).
        if (!user_id.empty() && user_id.front() == '@')
        {
            const auto colon = user_id.find(':');
            if (colon != std::string::npos)
                return user_id.substr(1, colon - 1);
        }
        return user_id;
    };

    if (m == views::CallOverlayWidget::Mode::Popout)
    {
        // Create the OS window and wire its overlay.
        auto* win = create_call_window_();
        call_window_.reset(win);
        // Override repaint_fn: the popout window is a separate OS surface;
        // request_repaint_() would repaint the main window, not the popout.
        // call_window_->request_repaint() targets the popout's own surface.
        call_window_->wire_call_overlay(
            std::move(post_delayed_fn),
            [this] { if (call_window_) call_window_->request_repaint(); },
            std::move(avatar_fn),
            std::move(name_fn));
        call_window_->on_window_closed = [this]
        {
            on_call_overlay_mode_requested_(views::CallOverlayWidget::Mode::Docked);
        };
        call_window_->bring_to_front();
    }
    else
    {
        // Docked / DockedExpanded / Floating.
        main_app_->mount_call_overlay(
            m,
            std::move(post_delayed_fn),
            std::move(repaint_fn),
            std::move(avatar_fn),
            std::move(name_fn));
    }

    // Apply all persistent overlay state to the newly mounted widget.
    if (auto* ov = active_call_overlay_())
    {
        // Restore all mutable overlay state from the persistent struct.
        // restore() sets local_user_id first so is_self is applied when
        // update_participants() creates/refreshes tiles below.
        ov->restore(call_overlay_state_);
        ov->set_room_active(call_session_ &&
                            call_session_->room_id() == current_room_id_);

        ov->on_hang_up = [this] { end_call(); };
        ov->on_toggle_audio = [this](bool muted)
        {
            if (call_session_) call_session_->mute_audio(muted);
        };
        ov->on_toggle_video = [this](bool muted)
        {
            if (!call_session_) return;
            if (muted)
            {
                call_session_->mute_video(true);
                stop_call_video_capture_();
            }
            else
            {
                start_call_video_capture_();
                call_session_->mute_video(false);
            }
        };
        ov->on_toggle_screen_share = [this](bool sharing)
        {
            if (sharing)
                start_screen_share_();
            else
                stop_screen_share_();
        };
        ov->on_mode_change_requested = [this](views::CallOverlayWidget::Mode nm)
        {
            // A deliberate user choice must never be later "undone" by the
            // auto-restore path in handle_call_room_navigation_().
            call_auto_floated_ = false;
            on_call_overlay_mode_requested_(nm);
        };
        ov->on_float_position_changed = [this](float x, float y)
        {
            on_call_float_position_changed_(x, y);
        };

        // Wire the relayout requester so update_participants() can trigger a
        // layout pass after adding/removing tiles (tiles have zero bounds until
        // arrange() runs). The popout window has no incidental redraws to rely
        // on, so a dedicated relayout path is essential.
        if (m == views::CallOverlayWidget::Mode::Popout)
            ov->set_relayout_requester(
                [this] { if (call_window_) call_window_->request_relayout(); });
        else
            ov->set_relayout_requester([this] { request_relayout_(); });

        // Seed with current participants for mid-call mode switches.
        if (call_session_ && !call_session_->participants().empty())
            ov->update_participants(call_session_->participants());

        // Hide the docked panel immediately if the user is viewing a different
        // room. The room-switch path keeps it in sync from here on, but it
        // won't fire again until the next navigation — without this the panel
        // appears briefly in the wrong room after a Floating/Popout → Docked
        // transition.
        if (m != views::CallOverlayWidget::Mode::Popout &&
            m != views::CallOverlayWidget::Mode::Floating)
        {
            if (auto* panel = room_view_ ? room_view_->call_panel() : nullptr)
            {
                const bool in_call_room = call_session_ &&
                    call_session_->room_id() == current_room_id_;
                panel->set_visible(in_call_room);
            }
        }
    }

    // Persist the new mode (Settings::CallOverlayMode has the same ordinal
    // order) unless this transition was driven by Tesseract itself
    // (auto-float / auto-restore), which must not overwrite the user's
    // actual preference.
    if (persist)
    {
        Settings::instance().call_overlay_mode =
            static_cast<Settings::CallOverlayMode>(static_cast<int>(m));
        Settings::instance().save_to_disk(tesseract::config_dir());
    }
    request_relayout_();
}

void ShellBase::on_call_float_position_changed_(float x, float y)
{
    Settings::instance().call_overlay_float_x = x;
    Settings::instance().call_overlay_float_y = y;
    Settings::instance().save_to_disk(tesseract::config_dir());
    request_relayout_();
}
} // namespace tesseract
