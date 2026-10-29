#include "app/ShellBase.h"
#include "views/MainAppWidget.h"
#include <algorithm>
#include <unordered_set>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace tesseract
{

namespace
{
// The slot label the legacy ring (a plain MSC4075 notification) arrives under;
// MatrixRTC invites carry their real slot id.
constexpr const char* kLegacyRingSlot = "call#default";
} // namespace

std::vector<std::string> ShellBase::call_invite_decline_targets_(
    const std::vector<PendingCallInvite>& invites)
{
    // A DM call is signalled twice: a legacy ring and a MatrixRTC invite, two
    // events the caller's clients may each be waiting on. Declining answers
    // every one of them (MatrixRTC invites first), so neither keeps ringing the
    // caller or leaves us half-declined. An `m.rtc.decline` is a plain reference
    // to the notification event (MSC4310), so a legacy ring takes it too.
    std::vector<std::string> targets;
    for (const bool legacy : {false, true})
    {
        for (const auto& inv : invites)
        {
            if (inv.event_id.empty() || (inv.slot_id == kLegacyRingSlot) != legacy)
            {
                continue;
            }
            if (std::find(targets.begin(), targets.end(), inv.event_id) == targets.end())
            {
                targets.push_back(inv.event_id);
            }
        }
    }
    return targets;
}

void ShellBase::handle_rtc_invitation_ui_(std::string room_id,
                                           std::string slot_id,
                                           std::string caller_user_id,
                                           std::string /*call_intent*/,
                                           std::uint64_t lifetime_ms,
                                           std::string notification_event_id)
{
    // The banner follows the room's call state (RoomInfo::call_members), which
    // the same sync tick updates. An invite with an event id and a lifetime
    // also gets a Decline button until it runs out; the same event id with a
    // zero lifetime cancels that one invite. A room can hold several at once
    // (the legacy ring and the MatrixRTC invite of one DM call, or invites from
    // different callers), and the Decline button stays while any remains.
    // Invites belong to the account the event came in on, which need not be the
    // active one.
    const CallInviteKey key{event_account_.empty() ? my_user_id_ : event_account_, room_id};
    if (!notification_event_id.empty() && lifetime_ms == 0)
    {
        // An invite we showed is no longer active (withdrawn by the caller,
        // answered on another device, or its slot closed).
        auto it = pending_call_invites_.find(key);
        if (it != pending_call_invites_.end())
        {
            auto& list = it->second;
            const auto removed = std::remove_if(
                list.begin(), list.end(),
                [&](const PendingCallInvite& i) { return i.event_id == notification_event_id; });
            if (removed != list.end())
            {
                list.erase(removed, list.end());
                if (list.empty())
                {
                    pending_call_invites_.erase(it);
                }
                refresh_call_banners_();
            }
        }
        return;
    }
    if (!notification_event_id.empty() && lifetime_ms > 0)
    {
        const auto life = std::chrono::milliseconds(lifetime_ms);
        PendingCallInvite inv{notification_event_id, std::move(slot_id), std::move(caller_user_id),
                              std::chrono::steady_clock::now() + life};
        auto& list = pending_call_invites_[key];
        const auto same = std::find_if(list.begin(), list.end(),
                                       [&](const PendingCallInvite& i)
                                       { return i.event_id == inv.event_id; });
        if (same != list.end())
        {
            *same = std::move(inv);
        }
        else
        {
            list.push_back(std::move(inv));
        }
        trim_pending_call_invites_(notification_event_id);
        // Take the Decline button away when the invite runs out.
        post_to_ui_after_(static_cast<int>(std::min<std::uint64_t>(lifetime_ms, 130000)) + 250,
                          guarded([this] { refresh_call_banners_(); }));
    }
    refresh_call_banners_();
}

void ShellBase::trim_pending_call_invites_(const std::string& keep_event_id)
{
    const auto now = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (auto it = pending_call_invites_.begin(); it != pending_call_invites_.end();)
    {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const PendingCallInvite& i) { return i.expires <= now; }),
                   list.end());
        if (list.empty())
        {
            it = pending_call_invites_.erase(it);
            continue;
        }
        total += list.size();
        ++it;
    }
    while (total > kMaxPendingCallInvites)
    {
        auto victim_list = pending_call_invites_.end();
        std::size_t victim_index = 0;
        for (auto it = pending_call_invites_.begin(); it != pending_call_invites_.end(); ++it)
        {
            for (std::size_t i = 0; i < it->second.size(); ++i)
            {
                if (it->second[i].event_id == keep_event_id)
                {
                    continue;
                }
                if (victim_list == pending_call_invites_.end() ||
                    it->second[i].expires < victim_list->second[victim_index].expires)
                {
                    victim_list = it;
                    victim_index = i;
                }
            }
        }
        if (victim_list == pending_call_invites_.end())
        {
            break;
        }
        victim_list->second.erase(victim_list->second.begin() +
                                  static_cast<std::ptrdiff_t>(victim_index));
        if (victim_list->second.empty())
        {
            pending_call_invites_.erase(victim_list);
        }
        --total;
    }
}

void ShellBase::drop_call_invites_of_account_(const std::string& uid)
{
    for (auto it = pending_call_invites_.begin(); it != pending_call_invites_.end();)
    {
        it = it->first.first == uid ? pending_call_invites_.erase(it) : std::next(it);
    }
    // The rosters are the active account's, rebuilt on demand.
    call_banner_rosters_.clear();
}

void ShellBase::drop_call_invites_of_missing_rooms_(const std::string& uid,
                                                     const std::vector<RoomInfo>& rooms)
{
    std::unordered_set<std::string> known;
    known.reserve(rooms.size());
    for (const auto& r : rooms)
    {
        known.insert(r.id);
    }
    for (auto it = pending_call_invites_.begin(); it != pending_call_invites_.end();)
    {
        it = (it->first.first == uid && known.count(it->first.second) == 0)
                 ? pending_call_invites_.erase(it)
                 : std::next(it);
    }
    if (uid == my_user_id_)
    {
        for (auto it = call_banner_rosters_.begin(); it != call_banner_rosters_.end();)
        {
            it = known.count(it->first) == 0 ? call_banner_rosters_.erase(it) : std::next(it);
        }
    }
}

std::vector<std::string> ShellBase::call_banner_ids_(const RoomInfo* room,
                                                      const std::vector<PendingCallInvite>& invites)
{
    // The caller's own membership may not be in the room state yet, so an
    // invite alone is enough to show the banner (with the callers as its members).
    std::vector<std::string> ids = room ? room->call_members : std::vector<std::string>{};
    if (ids.empty())
    {
        for (const auto& i : invites)
        {
            if (std::find(ids.begin(), ids.end(), i.caller) == ids.end())
            {
                ids.push_back(i.caller);
            }
        }
    }
    return ids;
}

std::vector<ShellBase::PendingCallInvite> ShellBase::call_invites_shown_by_banner_(
    const std::vector<std::string>& banner_ids, const std::vector<PendingCallInvite>& invites)
{
    std::vector<PendingCallInvite> shown;
    for (const auto& i : invites)
    {
        if (std::find(banner_ids.begin(), banner_ids.end(), i.caller) != banner_ids.end())
        {
            shown.push_back(i);
        }
    }
    return shown;
}

void ShellBase::decline_pending_call_invite_(const std::string& room_id)
{
    auto it = pending_call_invites_.find({my_user_id_, room_id});
    if (it == pending_call_invites_.end())
    {
        return;
    }
    // Decline what the banner showed the user, and only that: an invite from a
    // caller the banner does not list was never seen as part of this call, so
    // the click is not an answer to it. The declines go out one at a time, in
    // order, off the UI thread, and a failed one is retried by the SDK layer.
    auto& list = it->second;
    const auto shown = call_invites_shown_by_banner_(call_banner_ids_(room_by_id_(room_id), list), list);
    if (shown.empty())
    {
        return;
    }
    const auto targets = call_invite_decline_targets_(shown);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const PendingCallInvite& i)
                              {
                                  return std::any_of(shown.begin(), shown.end(),
                                                     [&](const PendingCallInvite& s)
                                                     { return s.event_id == i.event_id; });
                              }),
               list.end());
    if (list.empty())
    {
        pending_call_invites_.erase(it);
    }
    if (client_)
    {
        for (const auto& event_id : targets)
        {
            client_->decline_call_invite_async(room_id, event_id);
        }
    }
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

    // A pending invite ends when it expires or when we join the call.
    auto inv = pending_call_invites_.find({my_user_id_, room_id});
    if (inv != pending_call_invites_.end())
    {
        auto& list = inv->second;
        if (joined_here)
        {
            list.clear();
        }
        else
        {
            const auto now = std::chrono::steady_clock::now();
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const PendingCallInvite& i) { return i.expires <= now; }),
                       list.end());
        }
        if (list.empty())
        {
            pending_call_invites_.erase(inv);
            inv = pending_call_invites_.end();
        }
    }
    static const std::vector<PendingCallInvite> kNoInvites;
    const auto& pending = inv != pending_call_invites_.end() ? inv->second : kNoInvites;
    const std::vector<std::string> call_ids = call_banner_ids_(r, pending);
    // Decline is offered for the invites whose caller the banner lists.
    const bool invited = !call_invites_shown_by_banner_(call_ids, pending).empty();

    if (!r || !server_info_.supports_calls || bridged || joined_here || call_ids.empty())
    {
        rv->clear_call_banner();
        call_banner_rosters_.erase(room_id);
        return;
    }

    auto& roster = call_banner_rosters_[room_id];
    if (roster.ids != call_ids)
    {
        roster = {};
        roster.ids = call_ids;
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
    if (invited)
        rv->set_call_banner_decline([this, room_id] { decline_pending_call_invite_(room_id); });
    else
        rv->set_call_banner_decline({});
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
    cap->set_callback([account = call_account_](const tk::ScreenCapture::Frame& f) {
        // Runs on the capture thread: the call's own account, held weakly.
        if (auto s = account.lock(); s && s->client)
            s->client->rtc_push_screen_frame_i420(
                f.y, f.u, f.v, f.width, f.height,
                f.stride_y, f.stride_u, f.stride_v);
    });
    // The share is announced only once capture is running. A capture that
    // ends on its own (declined portal, failed step, closed window) stops the
    // share here, rather than leaving a live track with no frames behind it.
    const std::uint64_t serial = ++screen_share_serial_;
    cap->set_failure_callback([this, serial, alive = weak_flag()]() {
        // Runs on the capture thread, which can outlive this shell.
        if (alive.expired())
            return;
        post_to_ui_alive_([this, serial]() { on_screen_capture_failed_(serial); });
    });
    cap->start();
    screen_capture_ = std::move(cap);
    if (auto* ov = active_call_overlay_())
        ov->set_screen_sharing(true);
}

void ShellBase::on_screen_capture_failed_(std::uint64_t serial)
{
    if (serial != screen_share_serial_ || !screen_capture_)
        return;
    stop_screen_share_();
    show_status_message_(tk::tr("Screen sharing stopped: the screen could not be captured."));
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

void ShellBase::handle_notification_ui_(
    std::string user_id, std::string room_id, std::string room_name,
    std::string sender, std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes,
    std::string event_id)
{
    if (!ui_ready_() || !Settings::instance().notifications_enabled)
    {
        return;
    }
    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);

    const bool focused = is_main_window_focused_();
    // Already watching this exact room: say nothing.
    if (focused && is_main_window_visible_() && active_account_ &&
        active_account_->user_id == user_id && current_room_id_ == room_id)
    {
        return;
    }
    if (!focused)
    {
        request_notification_attention_();
    }

    tesseract::Notification n;
    n.room_id = std::move(room_id);
    n.room_name = std::move(room_name);
    n.sender = std::move(sender);
    n.body = std::move(body);
    n.is_mention = is_mention;
    n.avatar_bytes = std::move(avatar_bytes);
    n.image_bytes = std::move(image_bytes);
    n.event_id = std::move(event_id);
    deliver_notification_(user_id, n);
}

void ShellBase::deliver_notification_(const std::string& user_id,
                                      const tesseract::Notification& n)
{
    if (auto sess = account_manager_.find(user_id); sess && sess->notifier)
    {
        sess->notifier->notify(n);
    }
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
        [account = call_account_](const tk::VideoCapture::Frame& f)
        {
            // Runs on the capture thread: the call's own account, held weakly.
            if (auto s = account.lock(); s && s->client)
                s->client->rtc_push_video_frame_i420(
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

    // The call comes up in the background; its events already carry this id.
    // A failure to set it up arrives as a session-ended event.
    call_session_ = std::make_unique<CallSession>(client_, room_id, slot_id,
                                                  result.session_id);
    call_account_ = active_account_;
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
    update_call_lobby_gate_(room_id);
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

void ShellBase::end_call(bool wait_for_leave)
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
    {
        // Release the playback stream now: the session-ended event returns early
        // once call_session_ is reset, so it would hold the device until the next call.
        std::lock_guard<std::mutex> lock(call_audio_mutex_);
        call_audio_output_.reset();
    }
    if (call_session_)
    {
        if (wait_for_leave)
        {
            call_session_->hang_up_and_wait();
        }
        else
        {
            call_session_->hang_up();
        }
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

bool ShellBase::is_current_call_event_(std::uint64_t session_id) const
{
    if (!call_session_ || !call_session_->owns_event(session_id))
        return false;
    // The id is unique across accounts; the account check is a second guard.
    if (event_account_.empty())
        return true;
    const auto account = call_account_.lock();
    return !account || account->user_id == event_account_;
}

void ShellBase::handle_rtc_participant_joined_ui_(std::uint64_t session_id,
                                                   RtcParticipantInfo info)
{
    if (!is_current_call_event_(session_id))
        return;
    call_session_->on_participant_joined(info);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_participant_left_ui_(std::uint64_t session_id,
                                                 std::string participant_id)
{
    if (!is_current_call_event_(session_id))
        return;
    call_session_->on_participant_left(participant_id);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_participant_updated_ui_(std::uint64_t session_id,
                                                    RtcParticipantInfo info)
{
    if (!is_current_call_event_(session_id))
        return;
    call_session_->on_participant_updated(info);
    if (auto* ov = active_call_overlay_())
        ov->update_participants(call_session_->participants());
}

void ShellBase::handle_rtc_session_ended_ui_(std::uint64_t session_id,
                                              std::string reason)
{
    // Only the end of this very call counts. A late end of an earlier one (a
    // hang-up, another account's call) must not tear down a call started since.
    if (!is_current_call_event_(session_id))
        return;

    // Surface disconnect reason for non-normal ends.
    if (!reason.empty()
        && reason != "hangup"
        && reason != "user_action"
        && reason != "switch_device")
    {
        std::string msg;
        if (reason.rfind(kRtcStartFailedReason, 0) == 0)
            msg = tk::trf(tk::tr("Call failed: {0}"),
                          {reason.substr(sizeof(kRtcStartFailedReason) - 1)});
        else if (reason == "ice_failed" || reason == "dtls_failed" || reason == "network_error")
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
    // A screen share must not outlive the call: stop capture (and the OS
    // "recording" indicator) for any end reason, not only a local hang-up.
    stop_screen_share_();
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

namespace
{
// How long the call overlay reuses one member snapshot. Its name and avatar
// providers run per participant per paint, so the room's member list (which
// can be large) is read at most this often rather than on every lookup.
constexpr auto kCallOverlayRosterMaxAge = std::chrono::seconds(2);
} // namespace

const ShellBase::CallOverlayRoster&
ShellBase::call_overlay_roster_for_(const std::string& room_id)
{
    const auto now = std::chrono::steady_clock::now();
    auto& roster = call_overlay_roster_;
    if (roster.room_id == room_id && now - roster.built_at < kCallOverlayRosterMaxAge)
        return roster;

    roster.room_id = room_id;
    roster.built_at = now;
    roster.people.clear();
    if (client_)
    {
        for (const auto& mem : client_->get_room_members(room_id))
            roster.people.emplace(mem.user_id,
                                  std::make_pair(mem.display_name, mem.avatar_url));
    }
    return roster;
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
            const auto& people = call_overlay_roster_for_(call_session_->room_id()).people;
            const auto it = people.find(user_id);
            if (it != people.end() && !it->second.second.empty())
            {
                const std::string mxc = it->second.second;
                return avatar_image_(mxc);
            }
        }
        return nullptr;
    };
    auto name_fn = [this](const std::string& user_id) -> std::string
    {
        // Primary: room member list for the active call room.
        if (call_session_ && client_)
        {
            const auto& people = call_overlay_roster_for_(call_session_->room_id()).people;
            const auto it = people.find(user_id);
            if (it != people.end() && !it->second.first.empty())
                return it->second.first;
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
    // Debounced: a drag reports every step, and each step wrote the file.
    save_settings_debounced_();
    request_relayout_();
}
} // namespace tesseract
