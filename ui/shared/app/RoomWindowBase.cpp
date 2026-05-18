#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "views/text_util.h"
#include <tesseract/client.h>

namespace tesseract
{

RoomWindowBase::RoomWindowBase(ShellBase* shell, std::string room_id)
    : shell_(shell), room_id_(std::move(room_id))
{
}

RoomWindowBase::~RoomWindowBase()
{
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
        [this](const std::string& event_id, const std::string& key)
    {
        toggle_reaction_(event_id, key);
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
                                      const std::string& key)
{
    if (event_id.empty() || room_id_.empty() || !shell_->client_)
    {
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

} // namespace tesseract
