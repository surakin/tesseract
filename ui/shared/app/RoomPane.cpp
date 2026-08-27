#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "views/ForwardRoomPicker.h"
#include "views/ImageViewerOverlay.h"
#include "views/RoomMediaView.h"
#include "views/VideoViewerOverlay.h"
#include "views/media_drop.h"
#include "views/text_util.h"
#include "tk/i18n.h"
#include <tesseract/client.h>
#include <tesseract/mentions.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace tesseract
{

RoomPane::RoomPane(Deps deps, std::string room_id)
    : deps_(std::move(deps)), shell_(deps_.shell), room_id_(std::move(room_id))
{
    vid_fetch_group_ = shell_ ? shell_->alloc_media_group_() : 0;

    // Fetch this room's own MSC2545 pack (and every ancestor space's) once,
    // the same way ShellBase does on a main-window room switch — see
    // Client::set_active_room's doc comment. Harmless/idempotent to repeat
    // for the main window's own initial room; ShellBase's existing switch
    // path already re-issues this on every subsequent retarget().
    if (shell_ && shell_->client_)
    {
        shell_->client_->set_active_room(room_id_);
        for (const auto& space_id : shell_->parent_spaces_for_room_(room_id_))
            shell_->client_->set_active_room(space_id);
    }
}

RoomPane::~RoomPane()
{
    invalidate_weak_self();
    *media_extract_alive_ = false; // signal any in-flight background lambdas to abort
}

void RoomPane::attach(Widgets w)
{
    widgets_ = std::move(w);
    room_view_ = widgets_.room_view;
    img_viewer_ = widgets_.img_viewer;
    vid_viewer_ = widgets_.vid_viewer;
    wire_room_view_();
}

void RoomPane::retarget(const std::string& new_room_id)
{
    if (new_room_id == room_id_)
        return;
    // Deliberately minimal: wire_room_view_()'s installed callbacks all read
    // room_id_ live through `this` (not a captured snapshot), so no
    // re-wiring is needed — only the room-scoped caches below and the id
    // itself. Everything else a room switch needs (RoomView's own in-place
    // reset via RoomView::set_room(), pinned-events refresh, thread-panel
    // transition, subscription/warm-LRU bookkeeping, nav history) stays the
    // caller's responsibility, run immediately after this returns.
    cached_room_members_.clear();
    cached_members_room_.clear();
    visible_media_prepped_.clear();
    displayed_once_ = false;
    room_id_ = new_room_id;
}

void RoomPane::finish_init()
{
    // NOTE: registry registration (ShellBase::register_room_window_) and
    // subscription acquisition (ShellBase::acquire_room_subscription_) are
    // NOT done here — those are pop-out-only concepts that stay on
    // RoomWindowBase's own construction path (Phase 2 wires them around the
    // call to this method). This intentionally mirrors only the
    // per-room-*display* seeding half of the old RoomWindowBase::finish_init_.
    for (const auto& r : shell_->rooms_)
    {
        if (r.id == room_id_)
        {
            if (room_view_)
            {
                room_view_->set_room(r);
                // set_room() clears the pinned-messages banner, so seed it
                // from the cached room info right after.
                room_view_->set_pinned(r.pinned_events);
                room_view_->set_can_pin(
                    shell_->client_ && shell_->client_->can_pin_in_room(room_id_));
                room_view_->set_can_redact_others(
                    shell_->client_ && shell_->client_->can_redact_in_room(room_id_));
            }
            deps_.update_window_title(r.name);
            break;
        }
    }
    if (room_view_)
    {
        // Apply header button states that the main window receives via
        // on_server_info_ready_ui_() / apply_threads_list_() but that
        // pop-out windows must seed themselves at construction time.
        if (auto* h = room_view_->header())
            h->set_jump_to_date_enabled(shell_->server_info_.supports_msc3030);
        if (shell_->client_)
            room_view_->set_show_threads_button(
                !shell_->client_->list_room_threads(room_id_).empty());
    }
    if (room_view_ && room_view_->message_list())
    {
        room_view_->message_list()->on_retry_send =
            [this](const std::string& txn_id)
        {
            retry_send_(txn_id);
        };
        room_view_->message_list()->on_abort_send =
            [this](const std::string& txn_id)
        {
            abort_send_(txn_id);
        };
        room_view_->message_list()->on_tile_needed = [this](int z, int x, int y)
        {
            shell_->ensure_tile_async(z, x, y);
        };
    }
    // Pasting an image attaches it to the composer rather than inserting the
    // bitmap as text. Wired here (not in wire_room_view_) because the caller
    // creates its native text area after wire_room_view_ runs; by finish_init
    // (called after that) compose_text_area_() is ready.
    if (auto* ta = compose_text_area_())
    {
        ta->set_on_image_paste(
            [this](std::vector<std::uint8_t> bytes, std::string mime)
            {
                if (room_view_)
                {
                    room_view_->compose_bar()->set_pending_image(
                        std::move(bytes), std::move(mime));
                }
            });
        // Up-arrow in an empty composer re-edits the user's last own message.
        ta->set_on_edit_last(
            [this] { return room_view_ && room_view_->edit_last_own(); });
        // Inline pill-image resolution while composing (e.g. custom-emoji
        // inserts on Windows' BetterText control). make_static_image_provider_with_fetch_
        // is a ShellBase method, not MainWindow-specific, so this benefits
        // every platform; harmless no-op wherever insert_emoticon doesn't
        // need a resolver.
        ta->set_image_resolver(shell_->make_static_image_provider_with_fetch_(28, 28));
    }
}

void RoomPane::wire_room_view_()
{
    auto* rv = room_view_;

    // Pop-out windows share the shell's singleton capture_ but don't wire
    // voice recording — the main window owns that interaction. Hide the mic
    // button so pop-outs present a clean compose bar. The main window's own
    // wire_voice_capture_ call (each platform's MainWindow constructor, run
    // after main_room_pane_->attach()) re-enables it there when a capture
    // device is available.
    rv->compose_bar()->set_mic_available(false);

    // ── RoomView providers ────────────────────────────────────────────────
    rv->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_avatar_(mxc);
        });
    rv->on_room_avatar_needed =
        [this](const tesseract::RoomInfo& r) { shell_->ensure_room_avatar_(r); };
    rv->room_info_panel()->set_presence_provider(
        [this](const std::string& uid) -> tesseract::PresenceState
        {
            return shell_->presence_for_(uid);
        });
    // Lazy avatar fetching: set_avatar_provider above (shell_avatar_) is a
    // pure cache peek, so the panel requests a member's avatar only when
    // their row is actually visible in the open panel.
    rv->room_info_panel()->on_member_avatar_needed =
        [this](const tesseract::RoomMember& m)
    {
        shell_->ensure_user_avatar_(
            m.avatar_url, shell_->media_group_for_room_(room_id_));
    };
    rv->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            if (const auto* f = shell_->account_manager_.anim_cache().current_frame(mxc))
            {
                shell_->start_anim_tick_();
                return f;
            }
            if (const auto* img = shell_->account_manager_.image_cache().peek(mxc))
                return img;
            if (const auto* img = shell_->account_manager_.thumbnail_cache().peek(mxc))
                return img;
            // "thumb::"-prefixed keys are the client-generated video-
            // thumbnail sentinel, never a real mxc:///JSON MediaSource —
            // on_video_thumbnail_needed below handles regenerating those.
            if (mxc.starts_with("thumb::"))
                return nullptr;
            shell_->ensure_media_image_(mxc, visual::kMaxInlineImageWidth,
                                        visual::kMaxInlineImageHeight,
                                        shell_->media_group_for_room_(room_id_));
            return nullptr;
        });
    if (auto* ml = rv->message_list())
    {
        ml->on_video_thumbnail_needed =
            [this](const std::string& event_id, const std::string& source_token)
        {
            shell_->request_video_thumbnail_(event_id, source_token);
        };
    }
    rv->set_image_acquirer(
        [this](const std::string& mxc) -> tk::ImageRef
        {
            if (auto ref = shell_->account_manager_.image_cache().acquire(mxc))
                return ref;
            return shell_->account_manager_.thumbnail_cache().acquire(mxc);
        });
    rv->set_shortcode_provider(
        [this](const std::string& mxc) -> std::string
        {
            return shell_->shortcode_for_mxc_(mxc);
        });
    // Avatar inside received mention pills: resolve user id -> member avatar
    // mxc -> cached image, kicking a fetch on miss (room-scoped, so it
    // cancels along with the rest of this room's media on switch/close). The
    // row repaints when the bytes arrive.
    rv->message_list()->set_mention_avatar_provider(
        [this](const std::string& user_id) -> const tk::Image*
        {
            for (const auto& m : cached_room_members_)
            {
                if (m.user_id != user_id)
                    continue;
                if (m.avatar_url.empty())
                    return nullptr;
                shell_->ensure_user_avatar_(
                    m.avatar_url, shell_->media_group_for_room_(room_id_));
                return shell_->account_manager_.thumbnail_cache().peek(
                    m.avatar_url);
            }
            return nullptr;
        });
    rv->set_preview_provider(
        [this](
            const std::string& url) -> const tesseract::views::UrlPreviewData*
        {
            return preview_lookup_(url);
        });
    {
        // The provider is invoked on the UI thread (during pointer handling in
        // MessageListView), so it uses the non-blocking voice_bytes_or_fetch_:
        // warmed bytes or empty + an async warm that repaints on arrival. The
        // on_ready closure can outlive this pane's owner, so guard it (the
        // window/pane may close while a voice clip is still downloading).
        rv->set_voice_bytes_provider(
            [this](
                const std::string& source_json) -> std::vector<std::uint8_t>
            {
                return shell_->voice_bytes_or_fetch_(
                    source_json,
                    guarded([this] { deps_.repaint(); }));
            });
    }

    // ── Repaint ──────────────────────────────────────────────────────────
    rv->set_repaint_requester(
        [this]
        {
            deps_.repaint();
        });

    // ── Per-room notification mode ────────────────────────────────────────
    rv->on_fetch_notification_mode = [this, rv](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_(guarded([this, rv, sess, room_id = std::move(room_id)]() mutable {
            if (!sess || !sess->client) return;
            auto mode = sess->client->get_room_notification_mode(room_id);
            post_to_ui_(guarded([rv, mode = std::move(mode)]() mutable {
                rv->room_info_panel()->set_notification_mode(std::move(mode));
            }));
        }));
    };
    rv->on_notification_mode_changed = [this](std::string room_id,
                                               std::string mode) {
        shell_->set_room_notification_mode_(room_id, mode);
    };
    rv->on_favourite_changed = [this](std::string room_id, bool on) {
        shell_->set_room_favourite_(room_id, on);
    };
    rv->on_low_priority_changed = [this](std::string room_id, bool on) {
        shell_->set_room_low_priority_(room_id, on);
    };

    // ── Room info panel: members + topic / leave / ignore ─────────────────
    // The room info panel (and the mention-pill avatar provider above) fetch
    // their member list lazily through this callback. Only names/avatar_urls
    // are cached here — no avatar bytes are fetched until a mention pill or
    // the info panel actually needs one, via set_mention_avatar_provider /
    // shell_avatar_'s own on-miss fetch.
    rv->on_fetch_room_members = [this, rv](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_(guarded([this, rv, sess, room_id = std::move(room_id)]() mutable {
            if (!sess || !sess->client) return;
            auto members = sess->client->get_room_members(room_id);
            post_to_ui_(guarded([this, rv, room_id, members = std::move(members)]() mutable {
                cached_room_members_ = members;
                cached_members_room_ = room_id;
                rv->set_room_members(std::move(members));
            }));
        }));
    };
    rv->on_save_topic = [this](std::string room_id, std::string topic) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_(guarded([this, sess, room_id = std::move(room_id),
                        topic = std::move(topic)]() mutable {
            if (!sess || !sess->client) return;
            auto res = sess->client->set_room_topic(room_id, topic);
            if (res.ok)
                return;
            post_to_ui_(guarded([this, message = res.message]() mutable {
                shell_show_status_message_(
                    tk::trf("Failed to set topic: {0}", {message}));
            }));
        }));
    };
    rv->on_leave_room = [this](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_(guarded([this, sess, room_id = std::move(room_id)]() mutable {
            if (!sess || !sess->client) return;
            auto res = sess->client->leave_room(room_id);
            post_to_ui_(guarded([this, ok = res.ok,
                         room_id = std::move(room_id)]() mutable {
                if (!ok) return;
                deps_.on_left_room(room_id);
            }));
        }));
    };
    rv->on_ignore_user = [this](std::string user_id) {
        if (!shell_->client_) return;
        shell_->client_->ignore_user_async(std::move(user_id));
    };
    rv->on_room_settings_opened = [this, rv](std::string room_id) {
        auto* v = rv->room_settings_view();
        if (!v) return;
        if (!shell_->client_)
        {
            v->set_field_permissions(false, false, false);
            v->set_security_field_permissions(false, false, false, false);
            v->set_permissions_field_permissions(false);
            v->set_calls_supported(false);
            v->set_image_pack_field_permissions(false);
            v->set_own_power_level({});
            shell_->seed_room_media_section_(room_id);
            return;
        }
        v->set_field_permissions(shell_->client_->can_set_room_name(room_id),
                                 shell_->client_->can_set_room_topic(room_id),
                                 shell_->client_->can_set_room_avatar(room_id));
        v->set_security_field_permissions(
            shell_->client_->can_set_room_encryption(room_id),
            shell_->client_->can_set_room_join_rules(room_id),
            shell_->client_->can_set_room_guest_access(room_id),
            shell_->client_->can_set_room_history_visibility(room_id));
        v->set_permissions_field_permissions(
            shell_->client_->can_set_room_power_levels(room_id));
        v->set_permissions_state(shell_->client_->room_power_levels(room_id));
        v->set_own_power_level(shell_->client_->room_own_power_level(room_id));
        v->set_calls_supported(shell_->server_info_.supports_calls);
        shell_->seed_room_media_section_(room_id);
        shell_->fetch_room_security_state_(room_id);
        shell_->seed_image_pack_tab_(room_id, v);
    };
    rv->on_room_settings_avatar_upload_requested =
        [this, rv](std::string room_id) {
        shell_->stage_room_settings_avatar_upload_(room_id, rv->room_settings_view());
    };

    // ── Requests to join (MSC2403, admin side) ──────────────────────────────
    // ShellBase's knock_requests_panel_room_id_/current_room_knock_requests_
    // are single (not per-RoomPane) state, and on_knock_requests_panel_updated_
    // only pushes into main_app_'s own RoomView — so this only works reliably
    // for the main window. A popout's KnockRequestsPanel can still be opened
    // (subscribe/accept/decline/ban all work by room_id) but won't receive
    // live updates unless the main window happens to show the same room.
    rv->on_room_info_opened = [this](std::string room_id) {
        auto* v = room_view_->room_info_panel();
        if (!v) return;
        if (!shell_->client_)
        {
            v->set_knock_requests_visible(false);
            return;
        }
        const tesseract::RoomInfo* info = shell_->room_by_id_(room_id);
        const bool knockable = info && (info->join_rule == "knock" ||
                                        info->join_rule == "knock_restricted");
        const bool can_moderate =
            knockable && (shell_->client_->can_invite_users(room_id) ||
                         shell_->client_->can_kick_users(room_id));
        v->set_knock_requests_visible(can_moderate);
    };
    rv->on_knock_requests_opened = [this](std::string room_id) {
        shell_->subscribe_knock_requests_panel_(room_id);
        if (auto* v = room_view_->knock_requests_panel())
        {
            v->set_can_ban(shell_->client_ && shell_->client_->can_ban_users(room_id));
        }
    };
    rv->on_knock_requests_closed = [this]() {
        shell_->unsubscribe_knock_requests_panel_();
    };
    if (auto* krp = rv->knock_requests_panel())
    {
        krp->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                return shell_avatar_(mxc);
            });
        krp->on_accept = [this](std::string user_id) {
            shell_->accept_knock_request_async_(shell_->knock_requests_panel_room_id_,
                                                user_id);
        };
        krp->on_decline = [this](std::string user_id) {
            shell_->decline_knock_request_async_(shell_->knock_requests_panel_room_id_,
                                                 user_id);
        };
    }
    // Deny & Ban is destructive (irreversible from the target's perspective)
    // and gated on a confirm dialog — see RoomView's own wiring of
    // knock_requests_panel_->on_decline_and_ban, which forwards here only
    // after the user confirms.
    rv->on_decline_and_ban_knock_request =
        [this](std::string room_id, std::string user_id, std::string reason) {
        shell_->decline_and_ban_knock_request_async_(room_id, user_id, reason);
    };
    rv->room_settings_view()->on_accept =
        [this, rv](std::string room_id, views::RoomSettingsChanges changes) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_(guarded(
            [this, rv, sess, room_id = std::move(room_id),
             changes = std::move(changes)]() mutable {
                ShellBase::RoomSettingsCommitOutcome outcome;
                if (!sess || !sess->client)
                {
                    outcome.error = "not logged in";
                }
                else
                {
                    outcome = ShellBase::apply_room_settings_(
                        sess->client.get(), room_id, changes);
                }
                post_to_ui_(guarded(
                    [this, rv, outcome, room_id,
                     media_override = changes.media_override]() mutable {
                        if (auto* v = rv->room_settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            shell_->commit_room_media_preview_override_(
                                room_id, media_override->has_override,
                                media_override->mode);
                    }));
            }));
    };

    // ── Compose callbacks ────────────────────────────────────────────────
    rv->on_send = [this](const std::string& body)
    {
        // Build from the composer's mention draft when a native text area is
        // present so inline pills become matrix.to links + m.mentions; fall
        // back to the plain body otherwise.
        std::string out_body = body;
        std::string formatted;
        bool has_mention = false;
        if (auto* ta = compose_text_area_())
        {
            auto draft = ta->composer_draft();
            for (const auto& seg : draft)
            {
                if (seg.kind == tesseract::MentionSeg::Kind::Mention)
                {
                    has_mention = true;
                }
            }
            if (!draft.empty())
            {
                auto msg = tesseract::build_mention_message(draft);
                out_body = msg.body;
                formatted = msg.formatted_body;
            }
        }
        std::string trimmed = tesseract::text::trim(out_body);
        if (trimmed.empty() && !has_mention)
        {
            return;
        }
        send_message_(out_body, formatted);
        if (auto* ta = compose_text_area_())
        {
            ta->set_text("");
        }
        room_view_->set_current_text({});
    };
    rv->on_send_reply =
        [this](const std::string& reply_id, const std::string& body)
    {
        if (body.empty())
        {
            return;
        }
        send_reply_(reply_id, body);
        if (auto* ta = compose_text_area_())
        {
            ta->set_text("");
        }
        room_view_->set_current_text({});
    };
    rv->on_send_edit =
        [this](const std::string& event_id, const std::string& new_body,
               bool is_caption)
    {
        if (new_body.empty() && !is_caption)
        {
            return;
        }
        send_edit_(event_id, new_body, is_caption);
        if (auto* ta = compose_text_area_())
        {
            ta->set_text("");
        }
        room_view_->set_current_text({});
    };
    rv->on_edit_cancelled = [this]
    {
        if (auto* ta = compose_text_area_())
        {
            ta->set_text("");
        }
        room_view_->set_current_text({});
    };
    rv->on_edit_prefill = [this](const std::string& body)
    {
        if (auto* ta = compose_text_area_())
        {
            ta->set_text(body);
        }
        else
        {
            room_view_->set_current_text(body);
        }
    };
    rv->on_delete_requested = [this](const std::string& event_id)
    {
        delete_event_(event_id);
    };
    rv->on_copy_event_source_requested = [this](const std::string& event_id)
    {
        copy_event_source_to_clipboard_(event_id);
    };
    rv->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key,
               const std::string& source_mxc)
    {
        toggle_reaction_(event_id, key, source_mxc);
    };
    rv->on_receipt_needed = [this](const std::string& event_id)
    {
        send_receipt_(event_id);
    };
    rv->on_member_pronoun_needed = [this](const std::string& user_id)
    {
        shell_->request_member_pronoun_ui_(user_id);
    };
    rv->on_link_clicked = [this](const std::string& url)
    {
        if (tesseract::Client::parse_matrix_link(url).kind
            != tesseract::Client::MatrixLink::Kind::Unknown)
        {
            shell_->open_matrix_link(url);
        }
        else
        {
            tesseract::Client::open_in_browser(url);
        }
    };
    rv->on_near_top = [this]
    {
        request_pagination_back_();
    };
    // Forward pagination / return-to-live so scrolling up into history then
    // back down re-attaches this pane's timeline to the live edge.
    rv->on_near_bottom = [this]
    {
        if (!room_id_.empty())
        {
            shell_->request_forward_history_(room_id_);
        }
    };
    rv->on_return_to_live = [this]
    {
        if (!room_id_.empty())
        {
            shell_->return_to_live_(room_id_);
        }
    };
    // Jump from a reply/edit preview back to the original event — same
    // begin_focused_subscription_/subscribe_room_at pattern used by
    // apply_thread_transition_ below for thread-root jumps.
    rv->on_scroll_to_original = [this](const std::string& original_event_id)
    {
        if (room_id_.empty() || !shell_->client_)
        {
            return;
        }
        const std::string eid = original_event_id;
        const std::string rid = room_id_;
        shell_->begin_focused_subscription_(rid, eid);
        auto sess = shell_->active_account_;
        run_async_mut_([sess, rid, eid]() {
            if (!sess || !sess->client) return;
            sess->client->subscribe_room_at(rid, eid);
        });
    };
    rv->on_pin_requested = [this](const std::string& event_id)
    {
        pin_event_(event_id);
    };
    rv->on_unpin_requested = [this](const std::string& event_id)
    {
        unpin_event_(event_id);
    };
    // Lazy media/avatar fetch for rows newly scrolled into view. Skips the
    // main window's active_media_group_/prioritize_media reordering (that's
    // a single main-window-wide optimization not worth generalizing here) —
    // just the "fetch it now instead of waiting for the next prefetch pass"
    // half, scoped to this pane's own room/messages.
    rv->on_visible_range_changed = [this](const std::vector<std::string>&)
    {
        if (!room_view_ || !room_view_->message_list())
        {
            return;
        }
        auto* ml = room_view_->message_list();
        auto [first, last] = ml->visible_range();
        if (first < 0)
        {
            return;
        }
        const auto& msgs = ml->messages();
        for (int i = first; i <= last && i < static_cast<int>(msgs.size()); ++i)
        {
            const auto& row = msgs[static_cast<std::size_t>(i)];
            if (visible_media_prepped_.insert(row.event_id).second)
            {
                shell_->ensure_row_media_(row, /*fetch_avatars=*/true);
            }
        }
    };
    rv->on_visible_avatars_changed = [this](const std::vector<std::string>& urls)
    {
        auto group = shell_->media_group_for_room_(room_id_);
        for (const auto& url : urls)
        {
            shell_->ensure_user_avatar_(url, group);
        }
    };
    rv->on_has_dm = [this](const std::string& user_id)
    {
        return !shell_->find_existing_dm_(user_id).empty();
    };
    rv->on_open_dm = [this](std::string user_id)
    {
        open_dm_(std::move(user_id));
    };

    // Answering the incoming-call banner (or starting a call) — start_call is
    // a singleton (one call process-wide), so this is safe to wire
    // identically for every pane; ShellBase resolves the banner/dismiss
    // target per-room via room_view_for_room_.
    rv->on_start_call = [this](const std::string& room_id,
                               const std::string& slot_id, bool audio_only)
    {
        shell_->start_call(room_id, slot_id, audio_only);
    };

    // Forward picker: stable providers wired once so open() always has rooms.
    if (auto* fp = forward_picker_())
    {
        fp->set_rooms_provider(
            [this]() -> std::vector<tesseract::RoomInfo> { return shell_->rooms_; });
        fp->set_avatar_provider(
            [this](const std::string& mxc) { return shell_avatar_(mxc); });
        fp->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { shell_->ensure_room_avatar_(r); };
        fp->on_close = [this] { hide_forward_picker_field_(); deps_.relayout(); };
    }
    rv->on_forward_requested = [this](const std::string& event_id)
    {
        auto* fp = forward_picker_();
        if (!fp || room_id_.empty() || fp->is_open())
        {
            return;
        }
        fp->on_confirmed =
            [this, source_room = room_id_, event_id](std::vector<std::string> room_ids)
        {
            if (!shell_->client_) return;
            auto* fp_ptr = forward_picker_();
            if (!fp_ptr) return;
            fp_ptr->set_forwarding(static_cast<int>(room_ids.size()));
            for (const auto& rid : room_ids)
            {
                const auto req_id = shell_->next_request_id_++;
                pending_forwards_[req_id] = rid;
                shell_->client_->forward_event(req_id, source_room, event_id, rid);
            }
        };
        fp->open(room_id_);
        focus_forward_picker_field_();
        deps_.relayout();
    };

    // Room media gallery: opening/closing/pagination need no per-shell
    // platform specifics (unlike on_image_clicked/on_video_clicked below,
    // which restore native keyboard focus per shell), so they live here.
    rv->on_media_view_requested = [this](std::string /*room_id*/)
    {
        open_room_media_view_();
    };
    if (auto* rmv = room_media_view_())
    {
        rmv->on_close = [this] { close_room_media_view_(); };
        rmv->on_load_older_media = [this](std::string room_id)
        {
            on_media_view_load_older_(room_id);
        };
        rmv->set_image_provider(
            [this](const std::string& key) -> const tk::Image*
            {
                if (const auto* f = shell_->account_manager_.anim_cache().current_frame(key))
                {
                    shell_->start_anim_tick_();
                    return f;
                }
                if (const auto* img = shell_->account_manager_.image_cache().peek(key))
                    return img;
                if (const auto* img = shell_->account_manager_.thumbnail_cache().peek(key))
                    return img;
                if (shell_->media_fetches_in_flight_.size() <
                    ShellBase::kMaxConcurrentMediaFetches)
                {
                    shell_->ensure_media_thumbnail_(
                        key, static_cast<int>(views::RoomMediaView::kCellSize),
                        static_cast<int>(views::RoomMediaView::kCellSize),
                        false, media_view_group_);
                }
                return nullptr;
            });
    }

    // ── Media send (attachments) ──────────────────────────────────────────
    // Without these the compose bar drops a pending attachment on send. Clears
    // the composer on success via compose_text_area_() + room_view_. on_send_
    // image normalises/compresses via the surface-bound encode_for_send.
    auto clear_composer = [this]
    {
        if (auto* ta = compose_text_area_())
        {
            ta->set_text("");
        }
        if (room_view_)
        {
            room_view_->set_current_text({});
        }
    };
    rv->on_send_image =
        [this, clear_composer](std::vector<std::uint8_t> bytes, std::string mime,
                               std::string filename, std::string caption, int w,
                               int h, bool is_animated,
                               std::string reply_event_id)
    {
        if (room_id_.empty() || !shell_->client_)
            return;

        std::vector<std::uint8_t> send_bytes;
        std::string send_mime;
        std::string send_name;
        std::uint32_t send_w = static_cast<std::uint32_t>(w < 0 ? 0 : w);
        std::uint32_t send_h = static_cast<std::uint32_t>(h < 0 ? 0 : h);

        if (is_animated)
        {
            send_bytes = std::move(bytes);
            send_mime  = std::move(mime);
            send_name  = std::move(filename);
        }
        else
        {
            const bool compress =
                tesseract::Settings::instance().image_quality ==
                tesseract::Settings::ImageQuality::Compressed;
            auto enc = deps_.host->encode_for_send(bytes.data(), bytes.size(), compress);
            if (enc.bytes.empty())
            {
                shell_show_status_message_(tk::tr("Image decode failed"));
                return;
            }
            send_bytes = std::move(enc.bytes);
            send_mime  = std::move(enc.mime);
            send_w     = enc.width;
            send_h     = enc.height;
            send_name  = std::move(filename);
            if (send_mime == "image/jpeg")
            {
                auto dot = send_name.find_last_of('.');
                if (dot != std::string::npos)
                    send_name = send_name.substr(0, dot);
                send_name += ".jpg";
            }
        }

        clear_composer();
        const auto request_id = shell_->account_manager_.next_upload_request_id();
        shell_->client_->send_image_async(request_id, room_id_, send_bytes, send_mime,
                                          send_name, caption, send_w, send_h,
                                          is_animated, reply_event_id);
    };
    rv->on_send_video =
        [this, clear_composer](std::vector<std::uint8_t> bytes, std::string mime,
                               std::string filename, std::string caption, int w,
                               int h, std::vector<std::uint8_t> thumb_bytes,
                               int thumb_w, int thumb_h,
                               std::uint64_t duration_ms,
                               std::string reply_event_id)
    {
        if (room_id_.empty() || !shell_->client_)
            return;
        clear_composer();
        const auto request_id = shell_->account_manager_.next_upload_request_id();
        shell_->client_->send_video_async(
            request_id, room_id_, bytes, mime, filename, caption,
            static_cast<std::uint32_t>(w < 0 ? 0 : w),
            static_cast<std::uint32_t>(h < 0 ? 0 : h), thumb_bytes,
            static_cast<std::uint32_t>(thumb_w < 0 ? 0 : thumb_w),
            static_cast<std::uint32_t>(thumb_h < 0 ? 0 : thumb_h), duration_ms,
            reply_event_id);
    };
    rv->on_send_audio =
        [this, clear_composer](std::vector<std::uint8_t> bytes, std::string mime,
                               std::string filename, std::string caption,
                               std::uint64_t duration_ms,
                               std::string reply_event_id)
    {
        if (room_id_.empty() || !shell_->client_)
            return;
        clear_composer();
        const auto request_id = shell_->account_manager_.next_upload_request_id();
        shell_->client_->send_audio_async(request_id, room_id_, bytes, mime, filename,
                                          caption, duration_ms, reply_event_id);
    };
    rv->on_send_file =
        [this, clear_composer](std::vector<std::uint8_t> bytes, std::string mime,
                               std::string filename, std::string caption,
                               std::string reply_event_id)
    {
        if (room_id_.empty() || !shell_->client_)
            return;
        clear_composer();
        const auto request_id = shell_->account_manager_.next_upload_request_id();
        shell_->client_->send_file_async(request_id, room_id_, bytes, mime, filename,
                                         caption, reply_event_id);
    };

    // ── Local image / video overlays ─────────────────────────────────────
    // img_viewer_ / vid_viewer_ are set via attach() (from Widgets) when the
    // caller wants local media playback.
    if (img_viewer_)
    {
        img_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                return shell_image_(url);
            });
        img_viewer_->set_repaint_requester(
            [this]
            {
                deps_.repaint();
            });
        // Do NOT call close() here — close() fires on_close(), causing
        // recursion. The overlay has already done its close work before
        // calling on_close.
        img_viewer_->on_close = [this]
        {
            if (img_viewer_)
            {
                img_viewer_->set_visible(false);
            }
            deps_.relayout();
            if (auto* ta = compose_text_area_())
            {
                ta->set_focused(true);
            }
        };
        // Copy-to-clipboard: fetch the original encoded bytes (shared)
        // and hand them to the surface host via
        // copy_source_to_clipboard_.
        img_viewer_->on_copy =
            [this](std::string source_url, std::string /*body*/)
        {
            copy_source_to_clipboard_(std::move(source_url));
        };

        rv->on_image_clicked =
            [this](const views::MessageListView::ImageHit& hit)
        {
            if (!img_viewer_)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            img_viewer_->open(src_tok, thumb_tok, hit.body,
                              hit.natural_w, hit.natural_h);
            img_viewer_->set_visible(true);
            deps_.relayout();
            deps_.grab_surface_focus();
            ensure_viewer_image_(src_tok);
        };

        rv->on_avatar_clicked =
            [this](std::string url, std::string name)
        {
            if (!img_viewer_ || url.empty())
                return;
            // Pass the avatar mxc URL as both source and display_key so the
            // already-cached small avatar shows immediately while a full-res
            // fetch runs. natural_{w,h}=0 lets the viewer pick a placeholder
            // size until bytes arrive.
            img_viewer_->open(url, url, name, 0, 0);
            img_viewer_->set_visible(true);
            deps_.relayout();
            deps_.grab_surface_focus();
            ensure_viewer_image_(url);
        };
    }

    if (vid_viewer_)
    {
        vid_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                return shell_image_(url);
            });
        vid_viewer_->set_repaint_requester(
            [this]
            {
                deps_.repaint();
            });
        // Do NOT call close() here — close() fires on_close(), causing
        // recursion.
        vid_viewer_->on_close = [this]
        {
            if (vid_viewer_)
            {
                vid_viewer_->set_visible(false);
            }
            // Drop any still-in-flight full-file fetch for the video that
            // was just closed, so its bytes can't arrive later and start
            // playback (with audio) against a hidden overlay.
            if (shell_)
            {
                shell_->cancel_media_group_(vid_fetch_group_);
            }
            deps_.relayout();
            if (auto* ta = compose_text_area_())
            {
                ta->set_focused(true);
            }
        };

        rv->on_video_clicked =
            [this](const views::MessageListView::VideoHit& hit)
        {
            if (!vid_viewer_)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            vid_viewer_->open(src_tok, thumb_tok,
                              hit.mime_type, hit.duration_ms, hit.natural_w,
                              hit.natural_h, hit.loop, hit.no_audio,
                              hit.hide_controls);
            vid_viewer_->set_visible(true);
            deps_.relayout();
            deps_.grab_surface_focus();
            fetch_and_play_video_(src_tok);
        };
        // The gallery opens the same lightboxes on click — reuse the exact
        // handlers just installed above rather than duplicating them.
        if (auto* rmv = room_media_view_())
        {
            rmv->on_image_clicked = rv->on_image_clicked;
            rmv->on_video_clicked = rv->on_video_clicked;
        }
    }

    // ── Jump-to-date (MSC3030) ────────────────────────────────────────────
    rv->on_date_jump = [this](std::uint64_t ts_ms)
    {
        shell_->handle_date_jump_(room_id_, ts_ms);
    };

    // ── In-room search ────────────────────────────────────────────────────
    rv->on_room_search_query =
        [this, rv](const std::string& q)
    {
        shell_->in_room_search_active_rv_  = rv;
        deps_.on_search_activated();
        shell_->in_room_search_room_id_    = room_id_;
        shell_->handle_in_room_search_query_(q);
    };
    rv->on_room_search_navigate =
        [this](int delta) { shell_->in_room_search_navigate_(delta); };
    rv->on_room_search_paginate_toggled =
        [this](bool enabled) { shell_->set_in_room_search_paginate_(enabled); };
    rv->on_room_search_closed = [this]()
    {
        shell_->in_room_search_clear_();
        deps_.relayout(); // hides the native search text field via layout cb
    };

    // ── Thread panel ──────────────────────────────────────────────────────
    rv->on_threads_button_clicked = [this]()
    {
        auto t = ShellBase::compute_thread_transition_(
            thread_panel_, thread_panel_prev_,
            thread_root_, ThreadTrigger::ToggleList, {});
        apply_thread_transition_(t);
    };
    rv->on_thread_open_requested = [this](const std::string& root_event_id)
    {
        const auto trigger = (thread_panel_ == ThreadPanel::List)
                                 ? ThreadTrigger::OpenFromList
                                 : ThreadTrigger::OpenFromMain;
        auto t = ShellBase::compute_thread_transition_(
            thread_panel_, thread_panel_prev_,
            thread_root_, trigger, root_event_id);
        apply_thread_transition_(t);
    };
    rv->on_thread_close_requested = [this]()
    {
        auto t = ShellBase::compute_thread_transition_(
            thread_panel_, thread_panel_prev_,
            thread_root_, ThreadTrigger::CloseThread, {});
        apply_thread_transition_(t);
    };
    rv->on_thread_send = [this, rv](const std::string& body,
                                    const std::string& /*formatted*/)
    {
        if (!shell_->client_ || room_id_.empty() || thread_root_.empty())
            return;
        // RoomView passes an always-empty `formatted` (it has no access to the
        // native text area's draft) — rebuild it here the same way on_send
        // does, so thread sends keep mentions and MSC2545 custom emoji.
        auto msg = draft_outgoing_message_(body);
        auto sess = shell_->active_account_;
        auto rid  = room_id_;
        auto root = thread_root_;
        run_async_mut_([sess, rid, root, msg]() mutable {
            if (!sess || !sess->client) return;
            sess->client->send_thread_message(rid, root, msg.body,
                                              msg.formatted_body);
        });
        if (auto* ta = compose_text_area_())
            ta->set_text("");
        rv->set_current_text({});
    };
    rv->on_thread_send_reply = [this, rv](const std::string& reply_id,
                                          const std::string& body,
                                          const std::string& /*formatted*/)
    {
        if (!shell_->client_ || room_id_.empty() || thread_root_.empty() ||
            reply_id.empty())
            return;
        auto msg = draft_outgoing_message_(body);
        auto sess = shell_->active_account_;
        auto rid  = room_id_;
        auto root = thread_root_;
        run_async_mut_([sess, rid, root, reply_id, msg]() mutable {
            if (!sess || !sess->client) return;
            sess->client->send_thread_reply(rid, root, reply_id, msg.body,
                                            msg.formatted_body);
        });
        if (auto* ta = compose_text_area_())
            ta->set_text("");
        rv->set_current_text({});
    };

    // ── Drop-into-compose-bar wiring ────────────────────────────────────────
    // RoomView::on_file_drop (the tree-dispatched catch-all reached when a
    // drop doesn't land on anything more specific) routes through these.
    rv->media_upload_limit_provider = [this]() -> std::uint64_t
    {
        if (auto* c = shell_client_())
            return c->media_upload_limit();
        return 0;
    };
    rv->media_info_extractor =
        [this, rv](std::uint32_t gen, std::vector<std::uint8_t> b, std::string m)
    {
        shell_->extract_drop_media_(gen, std::move(b), std::move(m),
                                    rv->compose_bar(), media_extract_alive_);
    };
    rv->on_file_drop_outcome = [this](views::FileDropOutcome outcome)
    {
        if (outcome == views::FileDropOutcome::TooLarge)
            shell_->show_status_message_(tk::tr("File exceeds the upload limit"));
    };

    // ── Emoji/sticker pickers ────────────────────────────────────────────
    // RoomView owns and hosts both pickers itself (register_popup, not a
    // per-platform native window) — the only piece that still needs to leave
    // RoomView is the sticker send, which needs Client access.
    rv->set_client(shell_->client_);
    if (rv->emoji_picker())
        rv->emoji_picker()->set_image_provider(
            shell_->make_picker_image_provider_(false));
    if (rv->sticker_picker())
        rv->sticker_picker()->set_image_provider(
            shell_->make_picker_image_provider_(true));
    rv->on_sticker_picked = [this](const tesseract::ImagePackImage& img)
    {
        send_sticker_(img.body.empty() ? img.shortcode : img.body, img.url,
                      img.info_json);
    };
}

void RoomPane::apply_thread_transition_(
    const ThreadPanelController::ThreadTransition& t)
{
    if (shell_->client_)
    {
        for (const auto& root : t.threads_to_unsubscribe)
            shell_->client_->unsubscribe_thread(room_id_, root);
        if (t.unsubscribe_room_threads_)
            shell_->client_->unsubscribe_room_threads(room_id_);
        if (t.subscribe_room_threads_)
            shell_->client_->subscribe_room_threads(room_id_);
        for (const auto& root : t.threads_to_subscribe)
            shell_->client_->subscribe_thread(room_id_, root);
    }

    thread_panel_      = t.new_state;
    thread_panel_prev_ = t.new_prev;
    thread_root_       = t.new_root;

    if (room_view_)
    {
        using S = views::RoomView::ThreadPanelState;
        const S vs = (t.new_state == ThreadPanel::Closed) ? S::Closed
                   : (t.new_state == ThreadPanel::List)   ? S::List
                                                          : S::Open;
        room_view_->set_thread_panel(vs, t.new_root);

        if (auto* tlv = room_view_->thread_list_view())
            tlv->on_near_top = [this] { paginate_threads_(); };

        // When opening a thread, scroll the main message list to the root so
        // the user can see which thread they opened.
        if (t.new_state == ThreadPanel::Open && !t.new_root.empty())
            if (auto* ml = room_view_->message_list())
                ml->set_pending_scroll_event_id(t.new_root);

        deps_.relayout();
    }

    // If the thread root event isn't in the loaded timeline, subscribe_room_at
    // to fetch the surrounding context so the scroll can resolve.
    if (t.new_state == ThreadPanel::Open && !t.new_root.empty() && shell_->client_)
    {
        auto* ml = room_view_ ? room_view_->message_list() : nullptr;
        bool found = false;
        if (ml)
            for (const auto& m : ml->messages())
                if (m.event_id == t.new_root) { found = true; break; }
        if (!found)
        {
            const std::string eid = t.new_root;
            const std::string rid = room_id_;
            shell_->begin_focused_subscription_(rid, eid);
            auto sess = shell_->active_account_;
            run_async_mut_([sess, rid, eid]() {
                if (!sess || !sess->client) return;
                sess->client->subscribe_room_at(rid, eid);
            });
        }
    }

    if (shell_->client_ && t.new_state == ThreadPanel::List)
    {
        auto threads = shell_->client_->list_room_threads(room_id_);
        if (room_view_ && room_view_->thread_list_view())
        {
            room_view_->thread_list_view()->set_threads(std::move(threads));
            room_view_->thread_list_view()->scroll_to_bottom();
        }
        thread_ctl_.rearm_backfill();
        paginate_threads_();
    }
}

void RoomPane::paginate_threads_()
{
    auto* c = shell_->client_;
    auto sess = shell_->active_account_;
    thread_ctl_.set_run_paginate(
        guarded([this, c, sess, room_id = room_id_]
        {
            run_async_mut_(guarded([this, c, sess, room_id]
            {
                if (!sess || !sess->client) return;
                auto r = sess->client->paginate_room_threads(room_id);
                post_to_ui_(guarded([this, c, room_id, reached = r.reached_start]
                {
                    if (shell_->client_ != c) return;
                    const bool want_more =
                        (thread_panel_ == ThreadPanel::List);
                    if (thread_ctl_.on_paginate_result(reached, want_more))
                        paginate_threads_();
                    if (room_view_ && room_view_->thread_list_view() &&
                        shell_->client_)
                    {
                        auto threads =
                            shell_->client_->list_room_threads(room_id);
                        room_view_->thread_list_view()->set_threads(
                            std::move(threads));
                        deps_.relayout();
                    }
                }));
            }));
        }));
    thread_ctl_.begin_paginate(thread_panel_ == ThreadPanel::List);
}

void RoomPane::apply_thread_reset_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->set_messages(std::move(rows), /*room_switch=*/true);
    deps_.relayout();
}

void RoomPane::apply_thread_prepend_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl || rows.empty()) return;
    tl->prepend_messages(std::move(rows));
    deps_.relayout();
}

void RoomPane::apply_thread_append_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl || rows.empty()) return;
    tl->append_messages(std::move(rows));
    deps_.relayout();
}

void RoomPane::apply_thread_insert_(std::size_t index,
                                    views::MessageRowData row)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->insert_message(index, std::move(row));
    deps_.relayout();
}

void RoomPane::apply_thread_update_(std::size_t index,
                                    views::MessageRowData row)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->update_message(index, std::move(row));
    deps_.relayout();
}

void RoomPane::apply_thread_remove_(std::size_t index)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->remove_message(index);
    deps_.relayout();
}

void RoomPane::on_room_info_updated(const RoomInfo& r)
{
    if (room_view_)
    {
        room_view_->set_room(r);
        // set_room() clears the pinned-messages banner, so re-seed it from
        // the fresh room info (mirrors finish_init's initial seed).
        room_view_->set_pinned(r.pinned_events);
        room_view_->set_can_pin(
            shell_->client_ && shell_->client_->can_pin_in_room(room_id_));
        room_view_->set_can_redact_others(
            shell_->client_ && shell_->client_->can_redact_in_room(room_id_));
    }
    deps_.update_window_title(r.name);
    deps_.relayout();
}

bool RoomPane::on_timeline_reset(std::vector<views::MessageRowData> rows)
{
    // A genuine first display, OR a re-population of an emptied view (e.g.
    // logout -> login -> same room): both warrant the display gate.
    const auto* ml = room_view_ ? room_view_->message_list() : nullptr;
    const bool room_switch =
        !displayed_once_ || (ml && ml->messages().empty());
    displayed_once_ = true;

    // Never let a previous room's withheld tail leak into this one, switch
    // or not. `rows` is oldest-first (see on_message_inserted's doc comment
    // on backward-pagination ordering) — on a genuine switch with more rows
    // than comfortably fills a viewport, hold the oldest excess back rather
    // than handing tk::ListView the whole snapshot to measure synchronously
    // on first paint. request_pagination_back_() drains this buffer (via the
    // ordinary single-row insert path) before ever issuing a real SDK
    // pagination call, so scrolling up still reveals real history with no
    // gap at the cap boundary.
    withheld_older_rows_.clear();
    if (room_switch && rows.size() > ShellBase::kSwitchDisplayCap)
    {
        const auto split = rows.begin() +
            static_cast<std::ptrdiff_t>(rows.size() - ShellBase::kSwitchDisplayCap);
        withheld_older_rows_.assign(std::make_move_iterator(rows.begin()),
                                    std::make_move_iterator(split));
        rows.erase(rows.begin(), split);
    }

    if (room_view_)
    {
        room_view_->set_messages(std::move(rows), room_switch);
    }
    deps_.relayout();
    return room_switch;
}

void RoomPane::on_message_inserted(std::size_t idx,
                                   views::MessageRowData row)
{
    // idx == 0 is a backward-pagination insert (oldest-first within a
    // batch, but delivered one at a time here); anything else is a new live
    // message appended at the end. feed_gallery_live_ checks room_id_ ==
    // media_view_room_id_ itself (always true for pop-outs; may differ for
    // the main window if its gallery is pinned open on a room other than
    // the one currently displayed) and self-filters to Image/Video.
    if (row.kind == views::MessageRowData::Kind::Image ||
        row.kind == views::MessageRowData::Kind::Video)
    {
        feed_gallery_live_(room_id_, row, /*prepend=*/idx == 0);
    }
    if (room_view_)
    {
        room_view_->insert_message(idx, std::move(row));
    }
    deps_.relayout();
}

void RoomPane::on_message_updated(std::size_t idx,
                                  views::MessageRowData row)
{
    if (room_view_)
    {
        room_view_->update_message(idx, std::move(row));
    }
    deps_.relayout();
}

void RoomPane::on_message_removed(std::size_t idx)
{
    if (room_view_)
    {
        room_view_->remove_message(idx);
    }
    deps_.relayout();
}

void RoomPane::on_typing_changed(const std::string& text, bool visible)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
    deps_.relayout();
}

tesseract::MarkdownResult
RoomPane::draft_outgoing_message_(const std::string& fallback_body)
{
    if (auto* ta = compose_text_area_())
    {
        auto draft = ta->composer_draft();
        if (!draft.empty())
        {
            return tesseract::build_mention_message(draft);
        }
    }
    return {fallback_body, ""};
}

tesseract::Client* RoomPane::shell_client_() const
{
    return shell_->client_;
}

void RoomPane::send_message_(const std::string& body)
{
    if (body.empty())
        return;
    send_message_(body, "");
}

void RoomPane::send_message_(const std::string& body,
                             const std::string& formatted_body)
{
    // Slash-command ladder + normal send are centralized in ShellBase; the
    // on_send caller clears the compose bar after we return.
    shell_->dispatch_room_send_(room_id_, body, formatted_body);
}

void RoomPane::send_current_location_()
{
    shell_->send_current_location_(room_id_);
}

void RoomPane::send_reply_(const std::string& reply_event_id,
                           const std::string& body)
{
    if (body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto reply_id = reply_event_id;
    auto body_copy = body;
    run_async_mut_(guarded([this, sess, rid, reply_id, body_copy]() mutable {
        if (!sess || !sess->client) return;
        auto res = sess->client->send_reply(rid, reply_id, body_copy);
        if (res)
            return;
        post_to_ui_(guarded([this, message = res.message]() mutable {
            shell_show_status_message_(
                tk::trf("Send reply failed: {0}", {message}));
        }));
    }));
}

void RoomPane::send_sticker_(const std::string& body,
                             const std::string& image_url,
                             const std::string& info_json)
{
    if (room_id_.empty() || !shell_->client_)
        return;
    auto* cb = room_view_ ? room_view_->compose_bar() : nullptr;
    std::string reply_event_id;
    if (cb && cb->has_reply())
        reply_event_id = cb->reply_event_id();
    shell_->client_->send_sticker(room_id_, body, image_url, info_json,
                                  reply_event_id);
    if (cb)
        cb->clear_reply();
}

void RoomPane::send_edit_(const std::string& event_id,
                          const std::string& new_body, bool is_caption)
{
    if ((new_body.empty() && !is_caption) || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto eid = event_id;
    auto body_copy = new_body;
    run_async_mut_(guarded([this, sess, rid, eid, body_copy, is_caption]() mutable {
        if (!sess || !sess->client) return;
        auto res = is_caption
            ? sess->client->send_caption_edit(rid, eid, body_copy)
            : sess->client->send_edit(rid, eid, body_copy);
        if (res)
            return;
        post_to_ui_(guarded([this, message = res.message]() mutable {
            shell_show_status_message_(
                tk::trf("Edit failed: {0}", {message}));
        }));
    }));
}

void RoomPane::delete_event_(const std::string& event_id)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto eid = event_id;
    run_async_mut_([sess, rid, eid]() mutable {
        if (!sess || !sess->client) return;
        sess->client->redact_event(rid, eid);
    });
}

void RoomPane::copy_event_source_to_clipboard_(std::string event_id)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    if (!sess || !sess->client)
    {
        return;
    }
    // Synchronous, local-cache-only — no network roundtrip (see
    // Client::get_event_source's own doc comment) — so unlike
    // delete_event_/toggle_reaction_ this doesn't need run_async_mut_.
    std::string json = sess->client->get_event_source(room_id_, event_id);
    if (json.empty())
    {
        return;
    }
    deps_.host->set_clipboard_text(json);
    deps_.host->show_toast(tk::tr("Copied to clipboard"));
}

void RoomPane::toggle_reaction_(const std::string& event_id,
                                const std::string& key,
                                const std::string& source_mxc)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    if (!source_mxc.empty())
    {
        // For MSC4027 chips matrix-sdk aggregates by the mxc:// key (so the
        // incoming `key` IS the mxc URI). Look up the shortcode locally so
        // the outgoing event carries `:shortcode:` rather than the URI; if
        // unknown, send an empty shortcode (MSC4027 allows omitting it).
        // shortcode_for_mxc_ reads a UI-thread cache — call it now, before
        // crossing to mut_pool_.
        std::string sc = shell_->shortcode_for_mxc_(source_mxc);
        std::string shortcode =
            sc.empty() ? std::string() : ":" + sc + ":";
        auto mxc = source_mxc;
        auto eid = event_id;
        run_async_mut_([sess, rid, eid, mxc, shortcode]() mutable {
            if (!sess || !sess->client) return;
            sess->client->send_reaction_custom(rid, eid, mxc, shortcode);
        });
        return;
    }
    auto eid = event_id;
    auto k = key;
    run_async_mut_([sess, rid, eid, k]() mutable {
        if (!sess || !sess->client) return;
        sess->client->send_reaction(rid, eid, k);
    });
}

void RoomPane::send_receipt_(const std::string& event_id)
{
    shell_->maybe_send_read_receipt_(room_id_, event_id);
}

void RoomPane::send_typing_notice_(bool typing)
{
    if (room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->send_typing_notice(room_id_, typing);
}

void RoomPane::retry_send_(const std::string& /*txn_id*/)
{
    if (room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto res = shell_->client_->retry_send(room_id_);
    if (!res.ok)
    {
        shell_show_status_message_(
            tk::trf("Failed to retry sending: {0}", {res.message}));
    }
}

void RoomPane::abort_send_(const std::string& txn_id)
{
    if (txn_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto res = shell_->client_->abort_send(room_id_, txn_id);
    if (!res.ok)
    {
        shell_show_status_message_(
            tk::trf("Failed to cancel message: {0}", {res.message}));
    }
}

void RoomPane::pin_event_(const std::string& event_id)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto eid = event_id;
    run_async_mut_([sess, rid, eid]() mutable {
        if (!sess || !sess->client) return;
        auto r = sess->client->pin_event(rid, eid);
        if (!r.ok)
        {
            // TODO: surface this via a transient status mechanism once one exists
            std::fprintf(stderr, "[pin] pin failed for %s in %s: %s\n",
                         eid.c_str(), rid.c_str(), r.message.c_str());
        }
    });
}

void RoomPane::unpin_event_(const std::string& event_id)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto r = shell_->client_->unpin_event(room_id_, event_id);
    if (!r.ok)
    {
        // TODO: surface this via a transient status mechanism once one exists
        std::fprintf(stderr, "[pin] unpin failed for %s in %s: %s\n",
                     event_id.c_str(), room_id_.c_str(), r.message.c_str());
    }
}

void RoomPane::open_dm_(std::string user_id)
{
    if (user_id.empty() || !shell_->client_)
    {
        return;
    }

    // Fast path: DM already known — open (or focus) its own window.
    if (auto existing = shell_->find_existing_dm_(user_id); !existing.empty())
    {
        if (room_view_)
        {
            room_view_->close_user_profile();
        }
        shell_->open_room_in_new_window(existing);
        return;
    }

    if (shell_->dm_in_flight_user_ids_.count(user_id))
    {
        return;
    }
    shell_->dm_in_flight_user_ids_.insert(user_id);

    if (room_view_)
    {
        room_view_->set_dm_button_state(
            views::UserProfilePanel::DmButtonState::Sending);
        deps_.relayout();
    }

    auto sess = shell_->active_account();
    run_async_mut_(guarded([this, sess, user_id]() mutable {
        if (!sess || !sess->client)
        {
            return;
        }
        auto dm_id = sess->client->get_or_create_dm(user_id);
        // Built synchronously here, still inside this (already-guarded)
        // worker body — NOT inside the post_to_ui_ closure below, which runs
        // later on the UI thread with no such protection. guarded() captures
        // its weak token now, while `this` is confirmed alive; the resulting
        // closure is then just copied (never re-derived) into that closure.
        auto finish = guarded([this, dm_id]() mutable
        {
            if (!dm_id.empty())
            {
                if (room_view_)
                {
                    room_view_->close_user_profile();
                }
                shell_->open_room_in_new_window(dm_id);
            }
            else if (room_view_)
            {
                room_view_->set_dm_button_state(
                    views::UserProfilePanel::DmButtonState::Normal);
                deps_.relayout();
            }
        });
        shell_->post_to_ui_(
            [shell = shell_, user_id, finish]() mutable
            {
                // Always runs, even if this pane is gone by now — it clears
                // shell_-owned bookkeeping, not this pane's own state.
                shell->dm_in_flight_user_ids_.erase(user_id);
                finish();
            });
    }));
}

void RoomPane::open_room_media_view_()
{
    auto* rmv = room_media_view_();
    if (!rmv || !room_view_ || room_id_.empty())
    {
        return;
    }
    // The gallery fully covers the chat panel, so the timeline underneath
    // is invisible for as long as it's open — stop paying for its relayout
    // work (which the gallery's own aggressive backward pagination would
    // otherwise keep triggering many times over, since every paginated
    // batch still lands in message_list_ too — there is only one shared
    // room Timeline subscription).
    room_view_->set_message_list_relayout_suppressed(true);

    media_view_room_id_ = room_id_;
    // Distinct salt from media_group_for_room_(room_id_) (the room's normal
    // inline-media group) so closing the gallery never cancels unrelated
    // fetches — mirrors ShellBase::open_room_media_view_'s own salt.
    media_view_group_ = shell_->media_group_for_room_(room_id_) ^
                        0x9E3779B97F4A7C15ull;
    media_view_retries_left_ = ShellBase::kMediaViewMaxRetries;
    media_view_paginate_pending_ = false;
    media_view_known_media_count_ = 0;

    std::string room_name = room_id_;
    for (const auto& r : shell_->rooms_)
    {
        if (r.id == room_id_ && !r.name.empty())
        {
            room_name = r.name;
            break;
        }
    }
    rmv->open(room_id_, room_name);
    if (auto* ml = room_view_->message_list())
    {
        rmv->set_media(ml->messages());
    }
    auto pit = shell_->pagination_.find(room_id_);
    const bool reached_start =
        pit != shell_->pagination_.end() && pit->second.reached_start;
    rmv->set_reached_start(reached_start);
    // Most rooms have no media at all in the initially-synced window, so
    // the gallery frequently opens with item_count() == 0. tk::ListView's
    // on_wheel/on_near_top both no-op on an empty adapter (nothing to
    // scroll), so scrolling alone can never kick off the first pagination
    // round in that state — proactively start it here instead. Once this
    // round lands, handle_media_view_paginate_result_'s retry chain keeps
    // going automatically until enough media is found or history ends,
    // without needing any more wheel input.
    //
    // Deliberately item_count(), not content_fills_viewport(): rmv->open()
    // above just made the widget visible for the first time this session,
    // so it hasn't had its own arrange() pass yet and its bounds_ is still
    // {0,0,0,0} — content_fills_viewport() would trivially report "already
    // full" and skip the kickoff no matter how little media is actually
    // known. estimated_capacity() is likewise 0 here for the same reason,
    // so this falls back to kMediaViewMinTotal — the same expression used
    // by handle_media_view_paginate_result_'s retry loop, which picks up
    // the real target once a genuine arrange() pass has happened.
    const std::uint64_t kickoff_target = std::max<std::uint64_t>(
        rmv->estimated_capacity(), ShellBase::kMediaViewMinTotal);
    if (!reached_start && rmv->item_count() < kickoff_target)
    {
        request_media_view_pagination_back_();
    }
    deps_.relayout();
}

void RoomPane::close_room_media_view_()
{
    // Called from rmv->on_close (fired by the widget's own close button /
    // backdrop click), so this must NOT call rmv->close() itself — that
    // would re-fire on_close and recurse. Just clean up fetch state.
    if (media_view_group_ != 0)
    {
        shell_->cancel_media_group_(media_view_group_);
    }
    // Cancel the gallery's own in-flight backward pagination, if any,
    // rather than just abandoning it: the Rust-side tokio task otherwise
    // keeps running to completion, and a stale result landing after a
    // same-room reopen would be misattributed to the new session.
    if (media_view_pending_request_id_ != 0)
    {
        if (shell_->client_)
        {
            shell_->client_->cancel_paginate_back(media_view_pending_request_id_);
        }
        shell_->pending_paginates_.erase(media_view_pending_request_id_);
        shell_->media_view_paginate_owners_.erase(media_view_pending_request_id_);
        auto pit = shell_->pagination_.find(media_view_room_id_);
        if (pit != shell_->pagination_.end())
        {
            pit->second.in_flight = false;
        }
        media_view_pending_request_id_ = 0;
    }
    media_view_room_id_.clear();
    media_view_group_ = 0;
    media_view_retries_left_ = 0;
    media_view_paginate_pending_ = false;
    // Lifting suppression leaves whatever dirty state accumulated from
    // mutations while hidden in place; deps_.relayout() below is what
    // actually consumes it, in a single catch-up pass.
    if (room_view_)
    {
        room_view_->set_message_list_relayout_suppressed(false);
    }
    deps_.relayout();
}

void RoomPane::request_media_view_pagination_back_()
{
    // Any fire attempt (manual scroll, the automatic chain, or the deferred
    // resume) accounts for whatever the pending flag was tracking — clear
    // it unconditionally so a stale pending state can never cause a
    // redundant extra fire later. Harmless if it was already false.
    media_view_paginate_pending_ = false;
    if (!shell_->client_ || media_view_room_id_.empty())
    {
        return;
    }
    auto& state = shell_->pagination_[media_view_room_id_];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    if (media_view_retries_left_ <= 0)
    {
        return;
    }
    state.in_flight = true;
    --media_view_retries_left_;
    const auto req_id = shell_->next_paginate_id_++;
    shell_->pending_paginates_[req_id] = {media_view_room_id_, /*is_backward=*/true};
    shell_->media_view_paginate_owners_[req_id] = this;
    media_view_pending_request_id_ = req_id;
    shell_->client_->paginate_media_view_back_async(
        req_id, media_view_room_id_, ShellBase::kPaginationBatch);
}

void RoomPane::on_media_view_load_older_(const std::string& room_id)
{
    // tk::ListView's arrange-time autofill (list_view.cpp) fires on_near_top
    // whenever the loaded content doesn't fill the viewport, with no
    // visibility check — the widget tree keeps re-arranging the gallery
    // every app-wide relayout even after close() hides it. RoomMediaView::
    // close() clears its own room_id_ as defense in depth, but the gallery
    // can also be reopened for a DIFFERENT room before a stale call lands —
    // that arrives with a genuine (non-empty) room_id that just doesn't
    // match this pane's own media_view_room_id_ anymore, which close()'s
    // clear alone wouldn't catch. Bail unless it's still the actively-open
    // gallery's room; otherwise either case would re-arm
    // media_view_retries_left_ below and re-fire a real
    // paginate_media_view_back_async forever.
    if (room_id != media_view_room_id_)
    {
        return;
    }
    // A round for this room is already running — either the automatic
    // retry/accumulate chain in handle_media_view_paginate_result_, or an
    // earlier call to this same handler. Rearming the budget here too would
    // let a user who scrolls repeatedly while waiting (very natural when a
    // media-sparse room shows no visible progress yet) keep bumping
    // media_view_retries_left_ back up to kMediaViewMaxRetries on every
    // such gesture, so the chain never actually stops at its intended cap
    // — it just keeps finding a freshly-topped-up budget each time the
    // in-flight round completes. Let the in-flight round finish and
    // consult the *current* budget on its own instead of blindly resetting
    // it.
    if (shell_->pagination_[room_id].in_flight)
    {
        return;
    }
    // No round is running: either the automatic chain never started (the
    // gallery already had enough media on open) or it ran out its budget
    // and stopped. Either way this is a genuine new scroll-to-top gesture,
    // so it gets its own fresh retry budget.
    media_view_retries_left_ = ShellBase::kMediaViewMaxRetries;
    request_media_view_pagination_back_();
}

void RoomPane::handle_media_view_paginate_result_(std::uint64_t request_id,
                                                   bool ok, bool reached_start,
                                                   std::uint64_t media_count)
{
    // The router (ShellBase::handle_media_view_paginate_result_ui_) already
    // erased request_id from pending_paginates_/media_view_paginate_owners_
    // and updated shell_->pagination_[media_view_room_id_].in_flight/
    // reached_start (which reached_start above already reflects) before
    // calling here.
    (void)ok;
    if (request_id == media_view_pending_request_id_)
    {
        media_view_pending_request_id_ = 0;
    }

    auto* rmv = room_media_view_();
    if (!rmv || media_view_room_id_.empty())
    {
        return;
    }

    rmv->set_reached_start(reached_start);
    media_view_known_media_count_ = media_count;
    // media_count is an authoritative snapshot read directly from the SDK's
    // timeline (see Client::paginate_media_view_back_async's doc comment)
    // — unlike a per-round yield count, it doesn't depend on the separate,
    // slower diff-streaming task having already delivered rows to this
    // widget, so the retry loop's stopping decision can't race ahead of
    // stale state. The target itself is the widget's real, geometry-derived
    // capacity (falling back to kMediaViewMinTotal only while that geometry
    // isn't known yet) so this actually keeps going until the visible area
    // is filled, not until some small fixed count is reached regardless of
    // how big the viewport really is.
    const std::uint64_t target = std::max<std::uint64_t>(
        rmv->estimated_capacity(), ShellBase::kMediaViewMinTotal);
    const bool need_more = !reached_start && media_count < target &&
                           media_view_retries_left_ > 0;
    if (!need_more)
    {
        return;
    }

    // Firing the next round immediately whenever need_more is true races
    // ahead of the separate, much slower diff-streaming task: dozens of
    // rounds can resolve (often local-store hits) before that task converts
    // and delivers even the first one's rows, so nothing appears to happen
    // and then everything lands in one big batch once it drains. Only fire
    // immediately if rendering is roughly caught up; otherwise defer and
    // let feed_gallery_live_/feed_gallery_prepend_batch_ resume this once
    // real rows land.
    const auto rendered = static_cast<std::uint64_t>(rmv->item_count());
    const auto gap = media_count > rendered ? media_count - rendered : 0;
    if (gap <= ShellBase::kMediaViewMaxRenderGap)
    {
        request_media_view_pagination_back_();
        return;
    }
    media_view_paginate_pending_ = true;
    std::string target_room = media_view_room_id_;
    shell_->post_to_ui_after_(
        ShellBase::kMediaViewPauseFallbackMs,
        guarded([this, target_room]
        {
            // The gallery may have been closed (or reopened for a
            // different room) while this was pending.
            if (target_room == media_view_room_id_)
            {
                maybe_resume_media_view_pagination_(/*force=*/true);
            }
        }));
}

void RoomPane::maybe_resume_media_view_pagination_(bool force)
{
    auto* rmv = room_media_view_();
    if (!media_view_paginate_pending_ || !rmv || media_view_room_id_.empty())
    {
        return;
    }

    const auto rendered = static_cast<std::uint64_t>(rmv->item_count());
    const auto gap = media_view_known_media_count_ > rendered
                          ? media_view_known_media_count_ - rendered
                          : 0;
    if (!force && gap > ShellBase::kMediaViewMaxRenderGap)
    {
        return; // still too far behind — wait for more rows, or the fallback timer
    }

    request_media_view_pagination_back_();
}

void RoomPane::feed_gallery_live_(const std::string& event_room_id,
                                  views::MessageRowData row, bool prepend)
{
    auto* rmv = room_media_view_();
    if (!rmv || !rmv->is_open() || event_room_id != media_view_room_id_)
    {
        return;
    }
    if (prepend)
    {
        std::vector<views::MessageRowData> batch{std::move(row)};
        rmv->prepend_media(std::move(batch));
    }
    else
    {
        rmv->append_live_media(std::move(row));
    }
    maybe_resume_media_view_pagination_(/*force=*/false);
}

void RoomPane::feed_gallery_prepend_batch_(
    const std::string& event_room_id, std::vector<views::MessageRowData> rows)
{
    auto* rmv = room_media_view_();
    if (!rmv || !rmv->is_open() || event_room_id != media_view_room_id_)
    {
        return;
    }
    std::vector<views::MessageRowData> media_rows;
    media_rows.reserve(rows.size());
    for (auto& row : rows)
    {
        if (row.kind == views::MessageRowData::Kind::Image ||
            row.kind == views::MessageRowData::Kind::Video)
        {
            media_rows.push_back(std::move(row));
        }
    }
    if (media_rows.empty())
    {
        return;
    }
    rmv->prepend_media(std::move(media_rows));
    maybe_resume_media_view_pagination_(/*force=*/false);
}

void RoomPane::feed_gallery_reset_(const std::string& event_room_id,
                                   std::vector<views::MessageRowData> rows)
{
    auto* rmv = room_media_view_();
    if (!rmv || event_room_id != media_view_room_id_)
    {
        return;
    }
    rmv->set_media(std::move(rows));
}

bool RoomPane::handle_forward_done_(std::uint64_t request_id)
{
    if (!pending_forwards_.count(request_id))
    {
        return false;
    }
    pending_forwards_.erase(request_id);
    if (pending_forwards_.empty())
    {
        if (auto* fp = forward_picker_())
        {
            fp->close();
        }
    }
    return true;
}

bool RoomPane::handle_forward_failed_(std::uint64_t request_id,
                                      const std::string& message)
{
    auto it = pending_forwards_.find(request_id);
    if (it == pending_forwards_.end())
    {
        return false;
    }
    const std::string target_room = it->second;
    pending_forwards_.erase(it);
    if (auto* fp = forward_picker_())
    {
        std::string target_name = target_room;
        for (const auto& r : shell_->rooms_)
        {
            if (r.id == target_room && !r.name.empty())
            {
                target_name = r.name;
                break;
            }
        }
        fp->add_forward_error(target_name, message);
        if (pending_forwards_.empty())
        {
            fp->mark_complete();
        }
    }
    return true;
}

const tk::Image* RoomPane::shell_avatar_(const std::string& mxc) const
{
    return shell_->account_manager_.thumbnail_cache().peek(mxc);
}

std::vector<tesseract::ImagePackImage>
RoomPane::shell_emoticons_() const
{
    return shell_->emoticons_for_room_(room_id_);
}

std::vector<std::string> RoomPane::shell_parent_spaces_for_room_() const
{
    return shell_ ? shell_->parent_spaces_for_room_(room_id_) : std::vector<std::string>{};
}

void RoomPane::shell_ensure_media_image_(const std::string& url, int w,
                                         int h)
{
    shell_->ensure_media_image_(url, w, h);
}

const tk::Image*
RoomPane::shell_gif_strip_image_(const GifResult& result,
                                 const std::function<void()>& repaint)
{
    return shell_->gif_strip_image_(result, repaint);
}

std::vector<std::uint8_t>
RoomPane::shell_cached_gif_bytes_(const std::string& url)
{
    return shell_->cached_gif_source_bytes_(url);
}

void RoomPane::shell_show_status_message_(std::string msg,
                                          int auto_clear_ms)
{
    shell_->show_status_message_(std::move(msg), auto_clear_ms);
}

void RoomPane::wire_mention_hooks_(
    views::MentionPopup* popup, views::MentionController::Hooks& hooks)
{
    if (popup)
    {
        popup->set_image_provider(
            [this](const std::string& mxc) { return shell_avatar_(mxc); });
    }
    hooks.room_id = [this] { return room_id_; };
    // Live client getter (robust against account switches while this pane is
    // open) and avatar prefetch into the shared cache.
    hooks.client = [this] { return shell_client_(); };
    hooks.fetch_avatar =
        [this](const std::string& mxc) { shell_->ensure_user_avatar_(mxc); };
    hooks.run_async = [this](std::function<void()> fn)
    { run_async_(std::move(fn)); };
    hooks.post_to_ui = [this](std::function<void()> fn)
    { post_to_ui_(std::move(fn)); };
}

void RoomPane::wire_slash_hooks_(views::SlashCommandController::Hooks& hooks)
{
    hooks.room_id = [this] { return room_id_; };
    hooks.client = [this] { return shell_client_(); };
    hooks.clear_composer = [this]
    {
        if (room_view_)
            room_view_->clear_compose_text();
    };
    // on_selfie is intentionally left unset here: it needs a main-window-only
    // selfie-camera overlay this class has no knowledge of. on_location is
    // shared, since send_current_location_ works identically for every pane.
    hooks.on_location = [this] { send_current_location_(); };
    hooks.bot_commands = [this]() -> std::vector<tesseract::CommandDescription>
    {
        auto* c = shell_client_();
        return c ? c->list_room_bot_commands(room_id_)
                 : std::vector<tesseract::CommandDescription>{};
    };
}

void RoomPane::wire_shortcode_hooks_(
    views::ShortcodePopup* popup, views::ShortcodeController::Hooks& hooks)
{
    if (popup)
    {
        popup->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            { return shell_image_(url); });
    }
    hooks.emoticons = [this]() { return shell_emoticons_(); };
    hooks.fetch_image = [this](const std::string& url)
    { shell_ensure_media_image_(url, 28, 28); };
    hooks.resolve_image = [this](const std::string& url) -> const tk::Image*
    { return shell_image_(url); };
}

void RoomPane::wire_gif_hooks_(views::GifController::Hooks& hooks)
{
    hooks.room_id = [this] { return room_id_; };
    hooks.client = [this] { return shell_client_(); };
    hooks.run_async = [this](std::function<void()> fn)
    { run_async_(std::move(fn)); };
    hooks.post_to_ui = [this](std::function<void()> fn)
    { post_to_ui_(std::move(fn)); };
    hooks.post_delayed = [this](int ms, std::function<void()> fn)
    {
        if (deps_.host)
            deps_.host->post_delayed(ms, std::move(fn));
    };
    hooks.api_key = []() -> std::string
    { return tesseract::Settings::instance().gif_api_key; };
    hooks.client_key = []() -> std::string { return "tesseract"; };
    hooks.clear_composer = [this]
    {
        if (auto* ta = compose_text_area_())
            ta->set_text("");
        if (room_view_)
            room_view_->clear_compose_text();
    };
    hooks.get_cached_gif_bytes =
        [this](const std::string& url) -> std::vector<std::uint8_t>
    { return shell_cached_gif_bytes_(url); };
}

void RoomPane::position_dropdown_popup_(tk::PopupSurfaceHandle* popup,
                                        tk::Rect cursor_local, int rows,
                                        float row_height, float width)
{
    if (!popup)
        return;
    const float h = static_cast<float>(rows) * row_height;
    popup->set_rect(cursor_local, {width, h}, tk::PopupPlacement::PreferAbove);
    popup->set_visible(true);
}

const tk::Image* RoomPane::shell_image_(const std::string& mxc) const
{
    // Full-resolution lightbox cache first, then anim → image → thumbnail.
    // Shared with the main-window viewers. viewer_image_lookup_ is non-const
    // (a hit may restart the anim tick), but shell_ is a mutable pointer so
    // the const-ness of this accessor is preserved — only *shell_ is mutated.
    return shell_->viewer_image_lookup_(mxc);
}

const views::UrlPreviewData*
RoomPane::preview_lookup_(const std::string& url)
{
    auto it = shell_->url_preview_data_.find(url);
    return it == shell_->url_preview_data_.end() ? nullptr : &it->second;
}

void RoomPane::request_pagination_back_()
{
    // Reveal rows a room switch withheld (see on_timeline_reset's
    // kSwitchDisplayCap comment) before ever asking the SDK for more —
    // they're already in memory, so this is synchronous and needs no
    // network round-trip. Mirrors how a real backward-pagination batch
    // lands: oldest-of-the-batch at index 0, each subsequent (newer) row at
    // the next index, so repeated increasing-index inserts land in the
    // correct final chronological order.
    if (!withheld_older_rows_.empty())
    {
        const std::size_t batch = std::min(withheld_older_rows_.size(),
                                           static_cast<std::size_t>(ShellBase::kPaginationBatch));
        const std::size_t start = withheld_older_rows_.size() - batch;
        for (std::size_t i = 0; i < batch; ++i)
        {
            if (room_view_)
                room_view_->insert_message(i, std::move(withheld_older_rows_[start + i]));
        }
        withheld_older_rows_.resize(start);
        if (room_view_)
            if (auto* ml = room_view_->message_list())
                ml->reset_near_top_latch();
        return;
    }

    if (room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto& state = shell_->pagination_[room_id_];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    state.in_flight = true;
    if (room_view_)
        room_view_->set_paginating(true);
    shell_->start_anim_tick_();
    shell_->run_async_(
        guarded([this, shell = shell_, sess = shell_->active_account(),
         room_id = room_id_]
        {
            if (!sess || !sess->client) return;
            auto pr = sess->client->paginate_back_with_status(
                room_id, ShellBase::kPaginationBatch);
            // Built synchronously here, still inside this (already-guarded)
            // worker body — NOT inside the post_to_ui_ closure below, which
            // runs later on the UI thread with no such protection.
            auto finish = guarded([this]
            {
                if (room_view_)
                {
                    room_view_->set_paginating(false);
                    if (auto* ml = room_view_->message_list())
                        ml->reset_near_top_latch();
                }
            });
            shell->post_to_ui_(
                [shell, room_id, pr, finish]() mutable
                {
                    // Always runs, even if this pane is gone by now — it's
                    // shell_-owned bookkeeping, not this pane's own state.
                    shell->push_paginate_result_(room_id, pr.reached_start);
                    // push_paginate_result_ only clears set_paginating(false)
                    // for the main window's own currently-displayed room; do
                    // it here too so a pop-out (whose room_id_ is never
                    // current_room_id_) still un-latches its own spinner and
                    // near-top scroll trigger.
                    finish();
                });
        }));
}

void RoomPane::run_async_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->run_async_(std::move(fn));
    }
}

void RoomPane::run_async_mut_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->run_async_mut_(std::move(fn));
    }
}

void RoomPane::post_to_ui_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->post_to_ui_(std::move(fn));
    }
}

void RoomPane::save_source_to_file_(std::string source_json,
                                     std::string dest_path)
{
    if (!shell_->client_) return;
    auto req_id = shell_->begin_media_req_(0,
        guarded([dest = std::move(dest_path)](std::vector<std::uint8_t> bytes) mutable
        {
            if (!bytes.empty())
            {
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            }
        }));
    shell_->client_->fetch_source_bytes_async(req_id, source_json);
}

void RoomPane::fetch_source_bytes_(
    const std::string& src, std::function<void(std::vector<std::uint8_t>)> on_ready)
{
    if (!shell_->client_)
    {
        return;
    }
    auto req_id = shell_->begin_media_req_(0,
        guarded([on_ready = std::move(on_ready)](
            std::vector<std::uint8_t> bytes) mutable
        {
            on_ready(std::move(bytes));
        }));
    shell_->client_->fetch_source_bytes_async(req_id, src);
}

namespace
{
// Mirrors ShellBase::fullres_key_'s "fullres:" + url convention — a distinct
// namespace so a video's full-file cache entry never collides with any
// other media_disk_cache_ key.
std::string video_cache_key_(const std::string& src)
{
    return "video-full:" + src;
}

// Cap on how large a video the streaming path will keep a second in-RAM
// copy of purely to write it to the disk cache once complete. Matches
// MAX_MEDIA_BYTES's existing 64 MiB rationale on the Rust side — streaming
// itself tolerates up to MAX_STREAM_BYTES (512 MiB) since it never buffers
// the whole thing, but caching deliberately doesn't extend that same
// tolerance to a second full copy held only for this purpose.
constexpr std::uint64_t kVideoCacheMaxBytes = 64ull * 1024 * 1024;

// Shared between a streaming fetch's on_chunk and on_done callbacks (see
// RoomPane::fetch_and_play_video_uncached_) — a shared_ptr so both closures
// observe the same accumulator/valid flag; independent per-lambda captures
// would each get their own copy and on_done would never see on_chunk's
// mutations.
struct VideoCacheAccum
{
    std::vector<std::uint8_t> bytes;
    bool valid = true;
};
} // namespace

void RoomPane::fetch_and_play_video_(std::string src)
{
    if (!shell_ || !vid_viewer_)
    {
        return;
    }
    // Built on the UI thread (weak_self() must be) and only invoked later,
    // also on the UI thread, via post_to_ui_ below — copying the resulting
    // closure into the background lambda just copies a weak_ptr + function,
    // it never touches `this` off the UI thread.
    auto on_looked_up = guarded(
        [this, src](std::vector<std::uint8_t> cached) mutable
        {
            if (!vid_viewer_)
            {
                return;
            }
            if (!cached.empty())
            {
                vid_viewer_->load_bytes(cached.data(), cached.size());
                deps_.relayout();
                return;
            }
            fetch_and_play_video_uncached_(std::move(src));
        });
    auto* shell = shell_;
    const std::string cache_key = video_cache_key_(src);
    shell->run_async_(
        [shell, cache_key, on_looked_up]() mutable
        {
            auto cached = shell->account_manager_.media_disk_cache().load(cache_key);
            shell->post_to_ui_(
                [on_looked_up, cached = std::move(cached)]() mutable
                {
                    on_looked_up(std::move(cached));
                });
        });
}

void RoomPane::fetch_and_play_video_uncached_(std::string src)
{
    if (!shell_ || !shell_->client_ || !vid_viewer_)
    {
        return;
    }
    // Cancel any fetch still in flight from a previously opened video —
    // otherwise its bytes could arrive after this one and hijack playback
    // of the video that's actually open now. Covers both stages below
    // (classification and the stream/buffer fetch itself), since both are
    // tagged with the same group id.
    shell_->cancel_media_group_(vid_fetch_group_);
    const std::string cache_key = video_cache_key_(src);

    auto play_buffered = [this, cache_key](std::string src)
    {
        if (!shell_->client_)
        {
            return;
        }
        auto req_id = shell_->begin_media_req_(vid_fetch_group_,
            guarded([this, cache_key](std::vector<std::uint8_t> bytes) mutable
            {
                if (vid_viewer_)
                    vid_viewer_->load_bytes(bytes.data(), bytes.size());
                deps_.relayout();
                if (!bytes.empty() && bytes.size() <= kVideoCacheMaxBytes)
                {
                    auto* shell = shell_;
                    shell->run_async_(
                        [shell, cache_key, bytes = std::move(bytes)]() mutable
                        {
                            shell->account_manager_.media_disk_cache().store(
                                cache_key, bytes);
                        });
                }
            }));
        shell_->client_->fetch_source_bytes_async(req_id, src, vid_fetch_group_);
    };

    // Classify a small prefix first: streaming only works for a "fast-start"
    // MP4/MOV whose moov index box precedes mdat — a non-fast-start file (or
    // anything not MP4-family) would just stall waiting for data that
    // arrives last, so those keep using the classic full-buffer fetch.
    auto prefix_req_id = shell_->begin_media_req_(vid_fetch_group_,
        guarded([this, src, play_buffered, cache_key](std::vector<std::uint8_t> prefix) mutable
        {
            if (!vid_viewer_ || !shell_->client_)
            {
                return;
            }
            const std::uint8_t classification =
                shell_->client_->classify_media_container(prefix);
            if (classification !=
                1 /* CONTAINER_FAST_START, see sdk/src/client/media.rs */)
            {
                play_buffered(std::move(src));
                return;
            }
            vid_viewer_->begin_stream_or_buffer();
            auto cache_accum = std::make_shared<VideoCacheAccum>();
            auto req_id = shell_->begin_media_stream_req_(
                vid_fetch_group_,
                guarded([this, cache_accum](std::vector<std::uint8_t> chunk,
                                            std::uint64_t total_size) mutable
                {
                    if (vid_viewer_)
                    {
                        // The real HTTP Content-Length, once known, lets the
                        // player report a true final length to its decoder
                        // instead of a growing partial size — see
                        // VideoViewerOverlay::set_stream_length's doc
                        // comment. Cheap/idempotent to call every chunk.
                        if (total_size > 0)
                            vid_viewer_->set_stream_length(total_size);
                        vid_viewer_->feed_stream_chunk(chunk.data(), chunk.size());
                    }
                    if (cache_accum->valid)
                    {
                        if (total_size == 0 || total_size > kVideoCacheMaxBytes)
                        {
                            // Unknown or too large to justify a second full
                            // in-RAM copy purely for caching — give up on
                            // caching this one, keep streaming normally.
                            cache_accum->valid = false;
                            cache_accum->bytes.clear();
                            cache_accum->bytes.shrink_to_fit();
                        }
                        else
                        {
                            cache_accum->bytes.insert(cache_accum->bytes.end(),
                                                      chunk.begin(), chunk.end());
                        }
                    }
                    deps_.relayout();
                }),
                guarded([this, cache_key, cache_accum]() mutable
                {
                    if (vid_viewer_)
                        vid_viewer_->end_stream();
                    if (cache_accum->valid && !cache_accum->bytes.empty())
                    {
                        auto* shell = shell_;
                        shell->run_async_(
                            [shell, cache_key, bytes = std::move(cache_accum->bytes)]() mutable
                            {
                                shell->account_manager_.media_disk_cache().store(
                                    cache_key, bytes);
                            });
                    }
                    deps_.relayout();
                }),
                guarded([this](std::uint8_t /*status*/) mutable
                {
                    if (vid_viewer_)
                        vid_viewer_->fail_stream();
                    deps_.relayout();
                }));
            shell_->client_->fetch_source_stream_async(req_id, src, vid_fetch_group_);
        }));
    shell_->client_->fetch_source_prefix_async(
        prefix_req_id, src, tesseract::visual::kVideoThumbnailPrefixBytes);
}

void RoomPane::copy_source_to_clipboard_(std::string source_json)
{
    if (!shell_->client_) return;
    auto req_id = shell_->begin_media_req_(0,
        guarded([this](
            std::vector<std::uint8_t> bytes) mutable
        {
            if (!bytes.empty() && deps_.host->set_clipboard_image(bytes))
            {
                deps_.host->show_toast(tk::tr("Copied to clipboard"));
            }
        }));
    shell_->client_->fetch_source_bytes_async(req_id, source_json);
}

void RoomPane::ensure_viewer_image_(const std::string& url)
{
    if (shell_ && !url.empty())
    {
        shell_->ensure_viewer_fullres_(url);
    }
}

} // namespace tesseract
