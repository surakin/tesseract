#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/text_util.h"
#include <tesseract/client.h>
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
}

void RoomWindowBase::wire_room_view_(views::RoomView* rv)
{
    // ── RoomView providers ────────────────────────────────────────────────
    rv->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_avatar_(mxc);
        });
    rv->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_image_(mxc);
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
    rv->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t>
        {
            return fetch_source_bytes_(source_json);
        });

    // ── Repaint ──────────────────────────────────────────────────────────
    rv->set_repaint_requester(
        [this]
        {
            surface_repaint_();
        });

    // ── Compose callbacks ────────────────────────────────────────────────
    rv->on_send = [this](const std::string& body)
    {
        std::string trimmed = tesseract::text::trim(body);
        if (trimmed.empty())
        {
            return;
        }
        send_message_(trimmed);
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
    rv->on_link_clicked = [](const std::string& url)
    {
        tesseract::Client::open_in_browser(url);
    };
    rv->on_near_top = [this]
    {
        request_pagination_back_();
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
                              hit.natural_h, hit.autoplay, hit.loop,
                              hit.no_audio, hit.hide_controls);
            vid_viewer_->set_visible(true);
            request_relayout();
            std::string src = src_tok;
            std::weak_ptr<bool> alive_weak = alive_;
            run_async_(
                [this, src = std::move(src),
                 alive_weak = std::move(alive_weak)]()
                {
                    auto bytes = fetch_source_bytes_(src);
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

void RoomWindowBase::on_typing_changed(const std::string& text, bool visible)
{
    typing_bar_visible_ = visible;
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

void RoomWindowBase::send_message_(const std::string& body)
{
    if (body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->send_message(room_id_, body);
}

void RoomWindowBase::send_reply_(const std::string& reply_event_id,
                                 const std::string& body)
{
    if (body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->send_reply(room_id_, reply_event_id, body);
}

void RoomWindowBase::send_edit_(const std::string& event_id,
                                const std::string& new_body)
{
    if (new_body.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->send_edit(room_id_, event_id, new_body);
}

void RoomWindowBase::delete_event_(const std::string& event_id)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    shell_->client_->redact_event(room_id_, event_id);
}

void RoomWindowBase::toggle_reaction_(const std::string& event_id,
                                      const std::string& key,
                                      const std::string& source_mxc)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
        return;
    }
    if (!source_mxc.empty())
    {
        // For MSC4027 chips matrix-sdk aggregates by the mxc:// key (so the
        // incoming `key` IS the mxc URI). Look up the shortcode locally so
        // the outgoing event carries `:shortcode:` rather than the URI; if
        // unknown, send an empty shortcode (MSC4027 allows omitting it).
        std::string sc = shell_->shortcode_for_mxc_(source_mxc);
        std::string shortcode =
            sc.empty() ? std::string() : ":" + sc + ":";
        shell_->client_->send_reaction_custom(room_id_, event_id, source_mxc,
                                              shortcode);
        return;
    }
    shell_->client_->send_reaction(room_id_, event_id, key);
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
    auto it = shell_->tk_avatars_.find(mxc);
    return it == shell_->tk_avatars_.end() ? nullptr : it->second.get();
}

const tk::Image* RoomWindowBase::shell_image_(const std::string& mxc) const
{
    if (auto* f = shell_->anim_cache_.current_frame(mxc))
    {
        return f;
    }
    auto it = shell_->tk_images_.find(mxc);
    return it == shell_->tk_images_.end() ? nullptr : it->second.get();
}

std::vector<std::uint8_t>
RoomWindowBase::fetch_source_bytes_(const std::string& source_json)
{
    if (!shell_->client_)
    {
        return {};
    }
    return shell_->client_->fetch_source_bytes(source_json);
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
        [shell = shell_, room_id = room_id_]
        {
            auto pr = shell->client_->paginate_back_with_status(
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
    run_async_(
        [this, src = std::move(source_json), dest = std::move(dest_path),
         alive_weak = std::move(alive_weak)]()
        {
            auto bytes = fetch_source_bytes_(src);
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
        shell_->ensure_media_image_(url, visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight);
    }
}

} // namespace tesseract
