#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "app/SlashCommands.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/media_drop.h"
#include "views/text_util.h"
#include <tesseract/client.h>
#include <tesseract/mentions.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <fstream>

namespace tesseract
{

RoomWindowBase::RoomWindowBase(ShellBase* shell, std::string room_id)
    : shell_(shell), room_id_(std::move(room_id))
{
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
            }
            // Set the real title immediately from the cached room info; the
            // subclass created the window with the room_id as a placeholder,
            // and the first on_room_info_updated may be seconds away.
            update_window_title_(r.name);
            break;
        }
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
    // The room info panel fetches its member list lazily through this
    // callback; pre-cache each avatar into the shared cache before handing
    // the members to the view so rows paint with pictures.
    rv->on_fetch_room_members = [this, rv](std::string room_id) {
        if (!shell_->client_) return;
        auto sess = shell_->active_account();
        run_async_([this, rv, sess, room_id = std::move(room_id),
                    alive = alive_]() mutable {
            if (!sess || !sess->client) return;
            auto members = sess->client->get_room_members(room_id);
            post_to_ui_([this, rv, alive = std::move(alive),
                         members = std::move(members)]() mutable {
                if (!*alive) return;
                for (const auto& m : members)
                    shell_->ensure_user_avatar_(m.avatar_url);
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
        auto sess = shell_->active_account();
        run_async_mut_([sess, user_id = std::move(user_id)]() mutable {
            if (!sess || !sess->client) return;
            sess->client->ignore_user(user_id);
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
            auto draft = ta->mention_draft();
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
    rv->on_reply_focus = [this]
    {
        if (auto* ta = compose_text_area_())
        {
            ta->set_focused(true);
        }
    };
    rv->on_focus_input = [this]
    {
        if (auto* ta = compose_text_area_())
        {
            ta->set_focused(true);
        }
    };
    rv->on_delete_requested = [this](const std::string& event_id)
    {
        delete_event_(event_id);
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
            auto sess = shell_->active_account();
            run_async_(
                [this, src = std::move(src),
                 alive_weak = std::move(alive_weak),
                 sess = std::move(sess)]()
                {
                    if (!sess || !sess->client) return;
                    auto bytes = fetch_source_bytes_(sess->client.get(), src);
                    shell_->post_to_ui_(
                        [this, alive_weak, bytes = std::move(bytes)]() mutable
                        {
                            auto alive = alive_weak.lock();
                            if (!alive || !*alive)
                                return;
                            if (vid_viewer_)
                            {
                                vid_viewer_->load_bytes(bytes.data(),
                                                        bytes.size());
                            }
                            request_relayout();
                        });
                });
        };
    }
}

void RoomWindowBase::handle_file_drop_(std::vector<std::uint8_t> bytes,
                                       std::string mime, std::string filename)
{
    if (!room_view_)
        return;
    std::uint64_t limit = 0;
    if (auto* c = shell_client_())
        limit = c->media_upload_limit();
    auto* cb = room_view_->compose_bar();
    auto outcome = views::dispatch_file_drop(
        *cb, std::move(bytes), std::move(mime), std::move(filename), limit,
        [this, cb](std::uint32_t gen, std::vector<std::uint8_t> b,
                   std::string m)
        {
            // RoomWindowBase is a friend of ShellBase, so it can reach the
            // protected per-shell probe and retarget it to this pop-out's
            // compose bar (guarded by alive_ against late posts after close).
            shell_->extract_drop_media_(gen, std::move(b), std::move(m), cb,
                                        alive_);
        });
    if (outcome == views::FileDropOutcome::TooLarge)
        shell_->show_status_message_("File exceeds the upload limit");
}

void RoomWindowBase::on_room_info_updated(const RoomInfo& r)
{
    if (room_view_)
    {
        room_view_->set_room(r);
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

void RoomWindowBase::save_popout_geometry_(int x, int y, int w, int h)
{
    auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    Settings::WindowGeometry geom;
    geom.x = x; geom.y = y; geom.w = w; geom.h = h;
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
    if (body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    if (tesseract::is_slash_command_no_arg(body, "myroomavatar"))
    {
        shell_->pick_and_set_room_avatar_(room_id_);
        return;
    }
    if (tesseract::is_slash_command_no_arg(body, "leave"))
    {
        shell_->leave_room_command_(room_id_);
        return;
    }
    if (auto target = tesseract::parse_slash_arg(body, "join"))
    {
        shell_->join_room_command_(*target);
        return;
    }
    if (auto user = tesseract::parse_slash_arg(body, "invite"))
    {
        shell_->invite_user_command_(room_id_, *user);
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto body_copy = body;
    run_async_mut_([sess, rid, body_copy]() mutable {
        if (!sess || !sess->client) return;
        tesseract::dispatch_compose_send(*sess->client, rid, body_copy, "");
    });
}

tesseract::Client* RoomWindowBase::shell_client_() const
{
    return shell_->client_;
}

void RoomWindowBase::send_message_(const std::string& body,
                                   const std::string& formatted_body)
{
    if (room_id_.empty() || !shell_->client_)
        return;
    if (tesseract::is_slash_command_no_arg(body, "myroomavatar"))
    {
        shell_->pick_and_set_room_avatar_(room_id_);
        return; // on_send caller clears the compose bar after we return
    }
    if (tesseract::is_slash_command_no_arg(body, "leave"))
    {
        shell_->leave_room_command_(room_id_);
        return;
    }
    if (auto target = tesseract::parse_slash_arg(body, "join"))
    {
        shell_->join_room_command_(*target);
        return;
    }
    if (auto user = tesseract::parse_slash_arg(body, "invite"))
    {
        shell_->invite_user_command_(room_id_, *user);
        return;
    }
    auto sess = shell_->active_account();
    auto rid = room_id_;
    auto body_copy = body;
    auto fmt_copy = formatted_body;
    run_async_mut_([sess, rid, body_copy, fmt_copy]() mutable {
        if (!sess || !sess->client) return;
        tesseract::dispatch_compose_send(*sess->client, rid, body_copy, fmt_copy);
    });
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

const tk::Image* RoomWindowBase::shell_avatar_(const std::string& mxc) const
{
    return shell_->account_manager_.thumbnail_cache().peek(mxc);
}

const std::vector<tesseract::ImagePackImage>&
RoomWindowBase::shell_emoticons_() const
{
    return shell_->cached_emoticons_;
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

std::vector<std::uint8_t>
RoomWindowBase::fetch_source_bytes_(tesseract::Client* client,
                                    const std::string& source_json)
{
    // Blocking — callers (video-viewer load, save-to-file) already run this on
    // a worker thread. The UI-thread voice-playback path uses the non-blocking
    // voice_bytes_or_fetch_ via set_voice_bytes_provider instead.
    // client is the AccountSession-captured Client so it remains valid even if
    // the account is logged out/switched while the task is in-flight.
    if (!client)
    {
        return {};
    }
    return client->fetch_source_bytes(source_json);
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
    std::weak_ptr<bool> alive_weak = alive_;
    auto sess = shell_->active_account();
    run_async_(
        [this, src = std::move(source_json), dest = std::move(dest_path),
         alive_weak = std::move(alive_weak),
         sess = std::move(sess)]()
        {
            if (!sess || !sess->client) return;
            auto bytes = fetch_source_bytes_(sess->client.get(), src);
            // Inner lambda: file write only — no this capture needed.
            shell_->post_to_ui_(
                [alive_weak, dest, bytes = std::move(bytes)]() mutable
                {
                    auto alive = alive_weak.lock();
                    if (!alive || !*alive)
                        return;
                    if (!bytes.empty())
                    {
                        std::ofstream f(dest, std::ios::binary);
                        f.write(
                            reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                    }
                });
        });
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
