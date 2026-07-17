#include "app/RoomWindowBase.h"
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

#include <cmath>
#include <cstdio>
#include <fstream>

namespace tesseract
{

RoomWindowBase::RoomWindowBase(ShellBase* shell, std::string room_id)
    : shell_(shell), room_id_(std::move(room_id))
{
    // Pop-outs show one fixed room for their whole lifetime and never go
    // through ShellBase::after_active_room_changed_() — keep this room's
    // own MSC2545 pack fetched the same way the main window does on switch
    // (see Client::set_active_room's doc comment), along with every
    // ancestor space's own pack.
    if (shell_ && shell_->client_)
    {
        shell_->client_->set_active_room(room_id_);
        for (const auto& space_id : shell_->parent_spaces_for_room_(room_id_))
            shell_->client_->set_active_room(space_id);
    }
}

RoomWindowBase::~RoomWindowBase()
{
    *alive_ = false; // signal any in-flight background lambdas to abort
    if (shell_)
    {
        remove_popout_from_settings_();
        shell_->unregister_room_window_(this);
        shell_->release_room_subscription_(room_id_);
    }
}

void RoomWindowBase::finish_init_()
{
    shell_->register_room_window_(this);
    shell_->acquire_room_subscription_(room_id_);
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
            }
            // Set the real title immediately from the cached room info; the
            // subclass created the window with the room_id as a placeholder,
            // and the first on_room_info_updated may be seconds away.
            update_window_title_(r.name);
            break;
        }
    }
    if (room_view_)
    {
        // Apply header button states that the main window receives via
        // on_server_info_ready_ui_() / apply_threads_list_() but that popout
        // windows must seed themselves at construction time.
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
    // bitmap as text. Wired here (not in wire_room_view_) because the subclass
    // creates its native text area after wire_room_view_ runs; by finish_init_
    // (called at the end of the subclass ctor) compose_text_area_() is ready.
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
        // Some platforms' RoomWindow already wire this themselves before
        // finish_init_ runs; setting the same lambda again here is harmless.
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

void RoomWindowBase::wire_room_view_(views::RoomView* rv)
{
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
            shell_->ensure_media_image_(mxc, visual::kMaxInlineImageWidth,
                                        visual::kMaxInlineImageHeight,
                                        shell_->media_group_for_room_(room_id_));
            return nullptr;
        });
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
        // on_ready closure can outlive this popout window, so guard it with
        // alive_ (the window may close while a voice clip is still downloading).
        std::weak_ptr<bool> alive_weak = alive_;
        rv->set_voice_bytes_provider(
            [this, alive_weak](
                const std::string& source_json) -> std::vector<std::uint8_t>
            {
                return shell_->voice_bytes_or_fetch_(
                    source_json,
                    [this, alive_weak]
                    {
                        auto alive = alive_weak.lock();
                        if (alive && *alive)
                            surface_repaint_();
                    });
            });
    }

    // ── Repaint ──────────────────────────────────────────────────────────
    rv->set_repaint_requester(
        [this]
        {
            surface_repaint_();
        });

    // ── Per-room notification mode ────────────────────────────────────────
    rv->on_fetch_notification_mode = [this, rv](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_([this, rv, sess, room_id = std::move(room_id),
                    alive = alive_]() mutable {
            if (!sess || !sess->client) return;
            auto mode = sess->client->get_room_notification_mode(room_id);
            post_to_ui_([rv, alive = std::move(alive),
                         mode = std::move(mode)]() mutable {
                if (!*alive) return;
                rv->room_info_panel()->set_notification_mode(std::move(mode));
            });
        });
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
        run_async_([this, rv, sess, room_id = std::move(room_id),
                    alive = alive_]() mutable {
            if (!sess || !sess->client) return;
            auto members = sess->client->get_room_members(room_id);
            post_to_ui_([this, rv, alive = std::move(alive),
                         room_id, members = std::move(members)]() mutable {
                if (!*alive) return;
                cached_room_members_ = members;
                cached_members_room_ = room_id;
                rv->set_room_members(std::move(members));
            });
        });
    };
    rv->on_save_topic = [this](std::string room_id, std::string topic) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_([sess, room_id = std::move(room_id),
                        topic = std::move(topic)]() mutable {
            if (!sess || !sess->client) return;
            sess->client->set_room_topic(room_id, topic);
        });
    };
    rv->on_leave_room = [this](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_([this, sess, room_id = std::move(room_id),
                        alive = alive_]() mutable {
            if (!sess || !sess->client) return;
            auto res = sess->client->leave_room(room_id);
            post_to_ui_([this, alive = std::move(alive), ok = res.ok]() mutable {
                if (!*alive || !ok) return;
                // Leaving from a room's own pop-out closes that pop-out.
                schedule_self_close_();
            });
        });
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
        shell_->seed_room_media_section_(room_id);
        shell_->fetch_room_security_state_(room_id);
        shell_->seed_image_pack_tab_(room_id, v);
    };
    rv->on_room_settings_avatar_upload_requested =
        [this, rv](std::string room_id) {
        shell_->stage_room_settings_avatar_upload_(room_id, rv->room_settings_view());
    };
    rv->room_settings_view()->on_accept =
        [this, rv](std::string room_id, views::RoomSettingsChanges changes) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_mut_(
            [this, rv, sess, room_id = std::move(room_id),
             changes = std::move(changes), alive = alive_]() mutable {
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
                post_to_ui_(
                    [this, rv, alive = std::move(alive), outcome, room_id,
                     media_override = changes.media_override]() mutable {
                        if (!*alive) return;
                        if (auto* v = rv->room_settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            shell_->commit_room_media_preview_override_(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
            });
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
        [this](const std::string& event_id, const std::string& new_body)
    {
        if (new_body.empty())
        {
            return;
        }
        send_edit_(event_id, new_body);
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
    // back down re-attaches the pop-out's timeline to the live edge, matching
    // the main window. Both forward to friend-accessible ShellBase methods.
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
    // begin_focused_subscription_/subscribe_room_at pattern already used by
    // apply_popout_thread_transition_ below for thread-root jumps.
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
    // half, scoped to this pop-out's own room/messages.
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

    // Answering the incoming-call banner (or starting a call) from this
    // pop-out — mirrors ShellBase::wire_main_app_widget_'s main-window
    // wiring. start_call is a singleton (one call process-wide), so this is
    // safe to wire identically on every window; ShellBase resolves the
    // banner/dismiss target per-room via room_view_for_room_.
    rv->on_start_call = [this](const std::string& room_id,
                               const std::string& slot_id, bool audio_only)
    {
        shell_->start_call(room_id, slot_id, audio_only);
    };

    // Forward picker: stable providers wired once so open() always has
    // rooms — mirrors ShellBase::wire_main_app_widget_'s main-window wiring,
    // scoped to this pop-out's own room_id_/picker.
    if (auto* fp = forward_picker_())
    {
        fp->set_rooms_provider(
            [this]() -> std::vector<tesseract::RoomInfo> { return shell_->rooms_; });
        fp->set_avatar_provider(
            [this](const std::string& mxc) { return shell_avatar_(mxc); });
        fp->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { shell_->ensure_room_avatar_(r); };
        fp->on_close = [this] { hide_forward_picker_field_(); request_relayout(); };
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
        request_relayout();
    };

    // Room media gallery: opening/closing/pagination need no per-shell
    // platform specifics (unlike on_image_clicked/on_video_clicked below,
    // which restore native keyboard focus per shell), so they live here.
    // See open_room_media_view_()/close_room_media_view_().
    rv->on_media_view_requested = [this](std::string /*room_id*/)
    {
        open_room_media_view_();
    };
    if (auto* rmv = room_media_view_())
    {
        rmv->on_close = [this] { close_room_media_view_(); };
        rmv->on_load_older_media = [this](std::string /*room_id*/)
        {
            request_pagination_back_();
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
    // image normalises/compresses via the surface-bound encode_for_send_().
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
            auto enc = encode_for_send_(bytes.data(), bytes.size(), compress);
            if (enc.bytes.empty())
                return;
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
        shell_->client_->send_image_async(0, room_id_, send_bytes, send_mime,
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
        shell_->client_->send_video_async(
            0, room_id_, bytes, mime, filename, caption,
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
        shell_->client_->send_audio_async(0, room_id_, bytes, mime, filename,
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
        shell_->client_->send_file_async(0, room_id_, bytes, mime, filename,
                                         caption, reply_event_id);
    };

    // Pop-out windows share the shell's singleton capture_ but don't wire
    // voice recording — the main window owns that interaction. Hide the mic
    // button so pop-outs present a clean compose bar.
    rv->compose_bar()->set_mic_available(false);

    // ── Local image / video overlays ─────────────────────────────────────
    // img_viewer_ / vid_viewer_ are set by the subclass before this call
    // (via PopoutRoomWidget) when the subclass wants local media playback.
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
                surface_repaint_();
            });
        // Do NOT call close() here — close() fires on_close(), causing recursion.
        // The overlay has already done its close work before calling on_close.
        img_viewer_->on_close = [this]
        {
            if (img_viewer_)
            {
                img_viewer_->set_visible(false);
            }
            request_relayout();
            if (auto* ta = compose_text_area_())
            {
                ta->set_focused(true);
            }
        };
        // Copy-to-clipboard: fetch the original encoded bytes (shared) and hand
        // them to the surface's host via the per-shell put_image_on_clipboard_.
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
            request_relayout();
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
            request_relayout();
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
                surface_repaint_();
            });
        // Do NOT call close() here — close() fires on_close(), causing recursion.
        vid_viewer_->on_close = [this]
        {
            if (vid_viewer_)
            {
                vid_viewer_->set_visible(false);
            }
            request_relayout();
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
            request_relayout();
            std::string src = src_tok;
            std::weak_ptr<bool> alive_weak = alive_;
            if (shell_->client_)
            {
                auto req_id = shell_->begin_media_req_(0,
                    [this, alive_weak](std::vector<std::uint8_t> bytes) mutable
                    {
                        auto alive = alive_weak.lock();
                        if (!alive || !*alive)
                            return;
                        if (vid_viewer_)
                            vid_viewer_->load_bytes(bytes.data(), bytes.size());
                        request_relayout();
                    });
                shell_->client_->fetch_source_bytes_async(req_id, src);
            }
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
    // Mirror the main window's search wiring (ShellBase::wire_main_app_),
    // routing through the same ShellBase methods but with this popout's rv
    // and room_id set as the active search target first. The native text
    // field overlay is created per-platform in each subclass constructor.
    rv->on_room_search_query =
        [this, rv](const std::string& q)
    {
        shell_->in_room_search_active_rv_  = rv;
        shell_->in_room_search_active_win_ = this;
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
        request_relayout(); // hides the native search text field via layout cb
    };

    // ── Thread panel ──────────────────────────────────────────────────────
    rv->on_threads_button_clicked = [this]()
    {
        auto t = ShellBase::compute_thread_transition_(
            popout_thread_panel_, popout_thread_panel_prev_,
            popout_thread_root_, ThreadTrigger::ToggleList, {});
        apply_popout_thread_transition_(t);
    };
    rv->on_thread_open_requested = [this](const std::string& root_event_id)
    {
        const auto trigger = (popout_thread_panel_ == ThreadPanel::List)
                                 ? ThreadTrigger::OpenFromList
                                 : ThreadTrigger::OpenFromMain;
        auto t = ShellBase::compute_thread_transition_(
            popout_thread_panel_, popout_thread_panel_prev_,
            popout_thread_root_, trigger, root_event_id);
        apply_popout_thread_transition_(t);
    };
    rv->on_thread_close_requested = [this]()
    {
        auto t = ShellBase::compute_thread_transition_(
            popout_thread_panel_, popout_thread_panel_prev_,
            popout_thread_root_, ThreadTrigger::CloseThread, {});
        apply_popout_thread_transition_(t);
    };
    rv->on_thread_send = [this, rv](const std::string& body,
                                    const std::string& /*formatted*/)
    {
        if (!shell_->client_ || room_id_.empty() || popout_thread_root_.empty())
            return;
        // RoomView passes an always-empty `formatted` (it has no access to the
        // native text area's draft) — rebuild it here the same way on_send
        // does, so thread sends keep mentions and MSC2545 custom emoji.
        auto msg = draft_outgoing_message_(body);
        auto sess = shell_->active_account_;
        auto rid  = room_id_;
        auto root = popout_thread_root_;
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
        if (!shell_->client_ || room_id_.empty() || popout_thread_root_.empty() ||
            reply_id.empty())
            return;
        auto msg = draft_outgoing_message_(body);
        auto sess = shell_->active_account_;
        auto rid  = room_id_;
        auto root = popout_thread_root_;
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
    // Over-limit and empty payloads are dropped silently except for the
    // TooLarge status message — pop-outs have no status bar of their own, so
    // this reuses the main window's shell_->show_status_message_.
    rv->media_upload_limit_provider = [this]() -> std::uint64_t
    {
        if (auto* c = shell_client_())
            return c->media_upload_limit();
        return 0;
    };
    rv->media_info_extractor =
        [this, rv](std::uint32_t gen, std::vector<std::uint8_t> b, std::string m)
    {
        // RoomWindowBase is a friend of ShellBase, so it can reach the
        // protected per-shell probe and retarget it to this pop-out's
        // compose bar (guarded by alive_ against late posts after close).
        shell_->extract_drop_media_(gen, std::move(b), std::move(m),
                                    rv->compose_bar(), alive_);
    };
    rv->on_file_drop_outcome = [this](views::FileDropOutcome outcome)
    {
        if (outcome == views::FileDropOutcome::TooLarge)
            shell_->show_status_message_("File exceeds the upload limit");
    };
}

void RoomWindowBase::apply_popout_thread_transition_(
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

    popout_thread_panel_      = t.new_state;
    popout_thread_panel_prev_ = t.new_prev;
    popout_thread_root_       = t.new_root;

    if (room_view_)
    {
        using S = views::RoomView::ThreadPanelState;
        const S vs = (t.new_state == ThreadPanel::Closed) ? S::Closed
                   : (t.new_state == ThreadPanel::List)   ? S::List
                                                          : S::Open;
        room_view_->set_thread_panel(vs, t.new_root);

        if (auto* tlv = room_view_->thread_list_view())
            tlv->on_near_top = [this] { paginate_popout_threads_(); };

        // When opening a thread, scroll the main message list to the root so
        // the user can see which thread they opened.
        if (t.new_state == ThreadPanel::Open && !t.new_root.empty())
            if (auto* ml = room_view_->message_list())
                ml->set_pending_scroll_event_id(t.new_root);

        request_relayout();
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
        popout_thread_ctl_.rearm_backfill();
        paginate_popout_threads_();
    }
}

void RoomWindowBase::paginate_popout_threads_()
{
    auto* c = shell_->client_;
    auto sess = shell_->active_account_;
    std::weak_ptr<bool> alive_weak = alive_;
    popout_thread_ctl_.set_run_paginate(
        [this, c, sess, room_id = room_id_, alive_weak]
        {
            run_async_mut_([this, c, sess, room_id, alive_weak]
            {
                if (!sess || !sess->client) return;
                auto r = sess->client->paginate_room_threads(room_id);
                post_to_ui_([this, c, room_id, reached = r.reached_start,
                              alive_weak]
                {
                    auto alive = alive_weak.lock();
                    if (!alive || !*alive) return;
                    if (shell_->client_ != c) return;
                    const bool want_more =
                        (popout_thread_panel_ == ThreadPanel::List);
                    if (popout_thread_ctl_.on_paginate_result(reached, want_more))
                        paginate_popout_threads_();
                    if (room_view_ && room_view_->thread_list_view() &&
                        shell_->client_)
                    {
                        auto threads =
                            shell_->client_->list_room_threads(room_id);
                        room_view_->thread_list_view()->set_threads(
                            std::move(threads));
                        request_relayout();
                    }
                });
            });
        });
    popout_thread_ctl_.begin_paginate(popout_thread_panel_ == ThreadPanel::List);
}

void RoomWindowBase::apply_thread_reset_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->set_messages(std::move(rows), /*room_switch=*/true);
    request_relayout();
}

void RoomWindowBase::apply_thread_prepend_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl || rows.empty()) return;
    tl->prepend_messages(std::move(rows));
    request_relayout();
}

void RoomWindowBase::apply_thread_append_(std::vector<views::MessageRowData> rows)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl || rows.empty()) return;
    tl->append_messages(std::move(rows));
    request_relayout();
}

void RoomWindowBase::apply_thread_insert_(std::size_t index,
                                          views::MessageRowData row)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->insert_message(index, std::move(row));
    request_relayout();
}

void RoomWindowBase::apply_thread_update_(std::size_t index,
                                          views::MessageRowData row)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->update_message(index, std::move(row));
    request_relayout();
}

void RoomWindowBase::apply_thread_remove_(std::size_t index)
{
    if (!room_view_) return;
    auto* tl = room_view_->thread_view();
    if (!tl) return;
    tl->remove_message(index);
    request_relayout();
}

void RoomWindowBase::on_room_info_updated(const RoomInfo& r)
{
    if (room_view_)
    {
        room_view_->set_room(r);
        // set_room() clears the pinned-messages banner, so re-seed it from
        // the fresh room info (mirrors finish_init_'s initial seed).
        room_view_->set_pinned(r.pinned_events);
        room_view_->set_can_pin(
            shell_->client_ && shell_->client_->can_pin_in_room(room_id_));
    }
    update_window_title_(r.name);
    request_relayout();
}

void RoomWindowBase::on_timeline_reset(std::vector<views::MessageRowData> rows)
{
    const bool room_switch = !displayed_once_;
    displayed_once_ = true;
    if (room_view_)
    {
        room_view_->set_messages(std::move(rows), room_switch);
    }
    request_relayout();
}

void RoomWindowBase::on_message_inserted(std::size_t idx,
                                         views::MessageRowData row)
{
    if (auto* rmv = room_media_view_();
        rmv && rmv->is_open() &&
        (row.kind == views::MessageRowData::Kind::Image ||
         row.kind == views::MessageRowData::Kind::Video))
    {
        // idx == 0 is a backward-pagination insert (oldest-first within a
        // batch, but delivered one at a time here); anything else is a new
        // live message appended at the end.
        if (idx == 0)
        {
            std::vector<views::MessageRowData> batch{row};
            rmv->prepend_media(std::move(batch));
        }
        else
        {
            rmv->append_live_media(row);
        }
    }
    if (room_view_)
    {
        room_view_->insert_message(idx, std::move(row));
    }
    request_relayout();
}

void RoomWindowBase::on_message_updated(std::size_t idx,
                                        views::MessageRowData row)
{
    if (room_view_)
    {
        room_view_->update_message(idx, std::move(row));
    }
    request_relayout();
}

void RoomWindowBase::on_message_removed(std::size_t idx)
{
    if (room_view_)
    {
        room_view_->remove_message(idx);
    }
    request_relayout();
}

void RoomWindowBase::repaint_anim_frame()
{
    // Default: repaint the room surface so inline animated media advances.
    // Subclasses override to also repaint visible emoji/sticker pickers.
    surface_repaint_();
}

void RoomWindowBase::on_typing_changed(const std::string& text, bool visible)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
    request_relayout();
}

void RoomWindowBase::schedule_self_close_()
{
    if (!shell_)
    {
        return;
    }
    shell_->post_to_ui_(
        [shell = shell_, w = this]
        {
            shell->release_owned_window_(w);
        });
}

Settings::WindowGeometry RoomWindowBase::get_saved_popout_geometry_(
    int default_w, int default_h) const
{
    const auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it == pops.end())
        return {};
    return ShellBase::clamp_to_screens_(it->geometry, default_w, default_h,
                                        shell_->get_screen_work_areas_());
}

Settings::WindowGeometry RoomWindowBase::get_saved_popout_geometry_(
    int default_w, int default_h, int target_dpi) const
{
    const auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it == pops.end())
        return {};
    Settings::WindowGeometry g = it->geometry;
    if (g.valid && g.dpi > 0 && target_dpi > 0 && target_dpi != g.dpi)
    {
        const double s = double(target_dpi) / double(g.dpi);
        g.w = static_cast<int>(std::lround(g.w * s));
        g.h = static_cast<int>(std::lround(g.h * s));
    }
    return ShellBase::clamp_to_screens_(g, default_w, default_h,
                                        shell_->get_screen_work_areas_());
}

void RoomWindowBase::save_popout_geometry_(int x, int y, int w, int h, int dpi)
{
    auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    Settings::WindowGeometry geom;
    geom.x = x; geom.y = y; geom.w = w; geom.h = h;
    geom.dpi = dpi;
    geom.valid = (w > 0 && h > 0);
    if (it != pops.end())
        it->geometry = geom;
    else
    {
        Settings::PopoutEntry e;
        e.room_id  = room_id_;
        e.geometry = geom;
        pops.push_back(std::move(e));
    }
    if (shell_)
        shell_->save_settings_debounced_();
}

void RoomWindowBase::remove_popout_from_settings_()
{
    auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it != pops.end())
    {
        pops.erase(it);
        if (shell_)
            shell_->save_settings_debounced_();
    }
}

void RoomWindowBase::send_message_(const std::string& body)
{
    if (body.empty())
        return;
    send_message_(body, "");
}

tesseract::MarkdownResult
RoomWindowBase::draft_outgoing_message_(const std::string& fallback_body)
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

tesseract::Client* RoomWindowBase::shell_client_() const
{
    return shell_->client_;
}

void RoomWindowBase::send_message_(const std::string& body,
                                   const std::string& formatted_body)
{
    // Slash-command ladder + normal send are centralized in ShellBase; the
    // on_send caller clears the compose bar after we return.
    shell_->dispatch_room_send_(room_id_, body, formatted_body);
}

void RoomWindowBase::send_current_location_()
{
    shell_->send_current_location_(room_id_);
}

void RoomWindowBase::send_reply_(const std::string& reply_event_id,
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
    run_async_mut_([sess, rid, reply_id, body_copy]() mutable {
        if (!sess || !sess->client) return;
        sess->client->send_reply(rid, reply_id, body_copy);
    });
}

void RoomWindowBase::send_sticker_(const std::string& body,
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

void RoomWindowBase::send_edit_(const std::string& event_id,
                                const std::string& new_body)
{
    if (new_body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto eid = event_id;
    auto body_copy = new_body;
    run_async_mut_([sess, rid, eid, body_copy]() mutable {
        if (!sess || !sess->client) return;
        sess->client->send_edit(rid, eid, body_copy);
    });
}

void RoomWindowBase::delete_event_(const std::string& event_id)
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

void RoomWindowBase::copy_event_source_to_clipboard_(std::string event_id)
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
    set_clipboard_text_(json);
    show_toast_(tk::tr("Copied to clipboard"));
}

void RoomWindowBase::toggle_reaction_(const std::string& event_id,
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

void RoomWindowBase::send_receipt_(const std::string& event_id)
{
    shell_->maybe_send_read_receipt_(room_id_, event_id);
}

void RoomWindowBase::send_typing_notice_(bool typing)
{
    if (room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->send_typing_notice(room_id_, typing);
}

void RoomWindowBase::retry_send_(const std::string& /*txn_id*/)
{
    if (room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->retry_send(room_id_);
}

void RoomWindowBase::abort_send_(const std::string& txn_id)
{
    if (txn_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->abort_send(room_id_, txn_id);
}

void RoomWindowBase::pin_event_(const std::string& event_id)
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

void RoomWindowBase::unpin_event_(const std::string& event_id)
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

void RoomWindowBase::open_dm_(std::string user_id)
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
        request_relayout();
    }

    auto sess = shell_->active_account();
    std::weak_ptr<bool> alive_weak = alive_;
    run_async_mut_([this, sess, user_id, alive_weak]() mutable {
        if (!sess || !sess->client)
        {
            return;
        }
        auto dm_id = sess->client->get_or_create_dm(user_id);
        shell_->post_to_ui_(
            [this, user_id, dm_id = std::move(dm_id), alive_weak]() mutable
            {
                shell_->dm_in_flight_user_ids_.erase(user_id);
                auto alive = alive_weak.lock();
                if (!alive || !*alive)
                {
                    return;
                }
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
                    request_relayout();
                }
            });
    });
}

void RoomWindowBase::open_room_media_view_()
{
    auto* rmv = room_media_view_();
    if (!rmv || !room_view_ || room_id_.empty())
    {
        return;
    }
    // Distinct salt from media_group_for_room_(room_id_) (the room's normal
    // inline-media group) so closing the gallery never cancels unrelated
    // fetches — mirrors ShellBase::open_room_media_view_'s own salt.
    media_view_group_ = shell_->media_group_for_room_(room_id_) ^
                        0x9E3779B97F4A7C15ull;

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
    // item_count(), not content_fills_viewport(): rmv->open() above just made
    // the widget visible for the first time this session, so it hasn't had
    // its own arrange() pass yet and its bounds_ is still {0,0,0,0} — see
    // ShellBase::open_room_media_view_'s identical comment for why a
    // viewport-fill check would trivially skip this kickoff. estimated_capacity()
    // is likewise 0 here for the same reason, so this falls back to
    // kMediaViewMinTotal, mirroring ShellBase::open_room_media_view_.
    const std::uint64_t kickoff_target = std::max<std::uint64_t>(
        rmv->estimated_capacity(), ShellBase::kMediaViewMinTotal);
    if (!reached_start && rmv->item_count() < kickoff_target)
    {
        request_pagination_back_();
    }
    request_relayout();
}

void RoomWindowBase::close_room_media_view_()
{
    // Called from rmv->on_close (fired by the widget's own close button /
    // backdrop click), so this must NOT call rmv->close() itself — that
    // would re-fire on_close and recurse. Just clean up fetch state.
    if (media_view_group_ != 0)
    {
        shell_->cancel_media_group_(media_view_group_);
        media_view_group_ = 0;
    }
    request_relayout();
}

bool RoomWindowBase::handle_forward_done_(std::uint64_t request_id)
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

bool RoomWindowBase::handle_forward_failed_(std::uint64_t request_id,
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

const tk::Image* RoomWindowBase::shell_avatar_(const std::string& mxc) const
{
    return shell_->account_manager_.thumbnail_cache().peek(mxc);
}

std::vector<tesseract::ImagePackImage>
RoomWindowBase::shell_emoticons_() const
{
    return shell_->emoticons_for_room_(room_id_);
}

std::vector<std::string> RoomWindowBase::shell_parent_spaces_for_room_() const
{
    return shell_ ? shell_->parent_spaces_for_room_(room_id_) : std::vector<std::string>{};
}

void RoomWindowBase::shell_ensure_media_image_(const std::string& url, int w,
                                               int h)
{
    shell_->ensure_media_image_(url, w, h);
}

const tk::Image*
RoomWindowBase::shell_gif_strip_image_(const GifResult& result,
                                       const std::function<void()>& repaint)
{
    return shell_->gif_strip_image_(result, repaint);
}

std::vector<std::uint8_t>
RoomWindowBase::shell_cached_gif_bytes_(const std::string& url)
{
    return shell_->cached_gif_source_bytes_(url);
}

void RoomWindowBase::shell_show_status_message_(std::string msg,
                                                int auto_clear_ms)
{
    shell_->show_status_message_(std::move(msg), auto_clear_ms);
}

void RoomWindowBase::wire_mention_shell_hooks_(
    views::MentionPopup* popup, views::MentionController::Hooks& hooks)
{
    if (popup)
    {
        popup->set_image_provider(
            [this](const std::string& mxc) { return shell_avatar_(mxc); });
    }
    // Live client getter (robust against account switches while the pop-out is
    // open) and avatar prefetch into the shared cache. RoomWindowBase is a
    // friend of ShellBase, so ensure_user_avatar_ is reachable here.
    hooks.client = [this] { return shell_client_(); };
    hooks.fetch_avatar =
        [this](const std::string& mxc) { shell_->ensure_user_avatar_(mxc); };
}

const tk::Image* RoomWindowBase::shell_image_(const std::string& mxc) const
{
    // Full-resolution lightbox cache first, then anim → image → thumbnail.
    // Shared with the main-window viewers. viewer_image_lookup_ is non-const
    // (a hit may restart the anim tick), but shell_ is a mutable pointer so the
    // const-ness of this accessor is preserved — only *shell_ is mutated.
    return shell_->viewer_image_lookup_(mxc);
}

const views::UrlPreviewData*
RoomWindowBase::preview_lookup_(const std::string& url)
{
    auto it = shell_->url_preview_data_.find(url);
    return it == shell_->url_preview_data_.end() ? nullptr : &it->second;
}

void RoomWindowBase::request_pagination_back_()
{
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
    shell_->run_async_(
        [shell = shell_, sess = shell_->active_account(), room_id = room_id_]
        {
            if (!sess || !sess->client) return;
            auto pr = sess->client->paginate_back_with_status(
                room_id, ShellBase::kPaginationBatch);
            shell->post_to_ui_(
                [shell, room_id, pr]
                {
                    shell->push_paginate_result_(room_id, pr.reached_start);
                });
        });
}

void RoomWindowBase::run_async_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->run_async_(std::move(fn));
    }
}

void RoomWindowBase::run_async_mut_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->run_async_mut_(std::move(fn));
    }
}

void RoomWindowBase::post_to_ui_(std::function<void()> fn)
{
    if (shell_)
    {
        shell_->post_to_ui_(std::move(fn));
    }
}

void RoomWindowBase::save_source_to_file_(std::string source_json,
                                           std::string dest_path)
{
    if (!shell_->client_) return;
    std::weak_ptr<bool> alive_weak = alive_;
    auto req_id = shell_->begin_media_req_(0,
        [alive_weak = std::move(alive_weak),
         dest = std::move(dest_path)](std::vector<std::uint8_t> bytes) mutable
        {
            auto alive = alive_weak.lock();
            if (!alive || !*alive) return;
            if (!bytes.empty())
            {
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            }
        });
    shell_->client_->fetch_source_bytes_async(req_id, source_json);
}

void RoomWindowBase::fetch_source_bytes_(
    const std::string& src, std::function<void(std::vector<std::uint8_t>)> on_ready)
{
    if (!shell_->client_)
    {
        return;
    }
    std::weak_ptr<bool> alive_weak = alive_;
    auto req_id = shell_->begin_media_req_(0,
        [alive_weak = std::move(alive_weak), on_ready = std::move(on_ready)](
            std::vector<std::uint8_t> bytes) mutable
        {
            auto alive = alive_weak.lock();
            if (!alive || !*alive) return;
            on_ready(std::move(bytes));
        });
    shell_->client_->fetch_source_bytes_async(req_id, src);
}

void RoomWindowBase::copy_source_to_clipboard_(std::string source_json)
{
    if (!shell_->client_) return;
    std::weak_ptr<bool> alive_weak = alive_;
    auto req_id = shell_->begin_media_req_(0,
        [this, alive_weak = std::move(alive_weak)](
            std::vector<std::uint8_t> bytes) mutable
        {
            auto alive = alive_weak.lock();
            if (!alive || !*alive) return;
            if (!bytes.empty() && put_image_on_clipboard_(bytes))
            {
                show_toast_(tk::tr("Copied to clipboard"));
            }
        });
    shell_->client_->fetch_source_bytes_async(req_id, source_json);
}

void RoomWindowBase::ensure_viewer_image_(const std::string& url)
{
    if (shell_ && !url.empty())
    {
        shell_->ensure_viewer_fullres_(url);
    }
}

std::function<const tk::Image*(const std::string&, const std::string&)>
RoomWindowBase::picker_image_provider_(bool is_sticker)
{
    return shell_->make_picker_image_provider_(is_sticker);
}

} // namespace tesseract
