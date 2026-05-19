#include "RoomView.h"

#include "tk/theme.h"

#include <algorithm>
#include <memory>

namespace tesseract::views
{

RoomView::RoomView()
{
    auto brand = std::make_unique<BrandView>();
    brand_view_ = add_child(std::move(brand));

    auto header = std::make_unique<RoomHeader>();
    header_ = add_child(std::move(header));

    auto msg = std::make_unique<MessageListView>();
    message_list_ = add_child(std::move(msg));

    auto compose = std::make_unique<ComposeBar>();
    compose_bar_ = add_child(std::move(compose));
    compose_bar_->set_enabled(false);

    wire_internal_callbacks();
}

void RoomView::wire_internal_callbacks()
{
    // Reply flow: hover ↩ → compose enters reply mode + focus.
    message_list_->on_reply_requested = [this](const std::string& event_id,
                                               const std::string& sender_name,
                                               const std::string& body_preview)
    {
        compose_bar_->set_reply_to(event_id, sender_name, body_preview);
        if (on_reply_focus)
        {
            on_reply_focus();
        }
    };

    // Edit flow: hover ✏ → compose enters edit mode, shell prefills textarea.
    message_list_->on_edit_requested =
        [this](const std::string& event_id, const std::string& current_body)
    {
        compose_bar_->set_editing(event_id);
        compose_bar_->set_current_text(current_body);
        if (on_edit_prefill)
        {
            on_edit_prefill(current_body);
        }
    };

    // Forward compose callbacks that reach the shell.
    compose_bar_->on_size_changed = [this]
    {
        if (on_layout_changed)
        {
            on_layout_changed();
        }
    };
    compose_bar_->on_send = [this](const std::string& body)
    {
        if (on_send)
        {
            on_send(body);
        }
    };
    compose_bar_->on_send_reply =
        [this](const std::string& reply_id, const std::string& body)
    {
        if (on_send_reply)
        {
            on_send_reply(reply_id, body);
        }
    };
    compose_bar_->on_send_edit =
        [this](const std::string& event_id, const std::string& new_body)
    {
        if (on_send_edit)
        {
            on_send_edit(event_id, new_body);
        }
    };
    compose_bar_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                         std::string mime, std::string filename,
                                         std::string caption, int w, int h,
                                         std::string reply_id)
    {
        if (on_send_image)
        {
            on_send_image(std::move(bytes), std::move(mime),
                          std::move(filename), std::move(caption), w, h,
                          std::move(reply_id));
        }
    };
    compose_bar_->on_send_file =
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename, std::string caption, std::string reply_id)
    {
        if (on_send_file)
        {
            on_send_file(std::move(bytes), std::move(mime), std::move(filename),
                         std::move(caption), std::move(reply_id));
        }
    };
    compose_bar_->on_mic_clicked = [this]
    {
        if (on_mic_clicked)
            on_mic_clicked();
    };
    compose_bar_->on_cancel_voice = [this]
    {
        if (on_cancel_voice)
            on_cancel_voice();
    };
    compose_bar_->on_edit_cancelled = [this]
    {
        if (on_edit_cancelled)
        {
            on_edit_cancelled();
        }
    };
    compose_bar_->on_emoji = [this](tk::Rect r)
    {
        if (on_emoji)
        {
            on_emoji(r);
        }
    };
    compose_bar_->on_sticker = [this](tk::Rect r)
    {
        if (on_sticker)
        {
            on_sticker(r);
        }
    };

    // Forward message-list callbacks that reach the shell.
    message_list_->on_delete_requested = [this](const std::string& event_id)
    {
        if (on_delete_requested)
        {
            on_delete_requested(event_id);
        }
    };
    message_list_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& emoji)
    {
        if (on_reaction_toggled)
        {
            on_reaction_toggled(event_id, emoji);
        }
    };
    message_list_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor)
    {
        if (on_add_reaction_requested)
        {
            on_add_reaction_requested(event_id, anchor);
        }
    };
    message_list_->on_link_clicked = [this](const std::string& url)
    {
        if (on_link_clicked)
        {
            on_link_clicked(url);
        }
    };
    message_list_->on_link_hovered = [this](const std::string& url)
    {
        if (on_link_hovered)
        {
            on_link_hovered(url);
        }
    };
    message_list_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip)
        {
            on_show_tooltip(std::move(text), anchor);
        }
    };
    message_list_->on_hide_tooltip = [this]()
    {
        if (on_hide_tooltip)
        {
            on_hide_tooltip();
        }
    };
    header_->on_link_clicked = [this](const std::string& url)
    {
        if (on_link_clicked)
        {
            on_link_clicked(url);
        }
    };
    message_list_->on_receipt_needed = [this](const std::string& event_id)
    {
        if (on_receipt_needed)
        {
            on_receipt_needed(event_id);
        }
    };
    message_list_->on_image_clicked =
        [this](const MessageListView::ImageHit& hit)
    {
        if (on_image_clicked)
        {
            on_image_clicked(hit);
        }
    };
    message_list_->on_video_clicked =
        [this](const MessageListView::VideoHit& hit)
    {
        if (on_video_clicked)
        {
            on_video_clicked(hit);
        }
    };
    message_list_->on_near_top = [this]
    {
        if (on_near_top)
        {
            on_near_top();
        }
    };
    message_list_->on_near_bottom = [this]
    {
        if (on_near_bottom)
        {
            on_near_bottom();
        }
    };
    message_list_->on_return_to_live = [this]
    {
        if (on_return_to_live)
        {
            on_return_to_live();
        }
    };
    header_->on_jump_to_date_requested = [this]
    {
        if (on_jump_to_date_requested)
        {
            on_jump_to_date_requested();
        }
    };
    header_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip)
        {
            on_show_tooltip(std::move(text), anchor);
        }
    };
    header_->on_hide_tooltip = [this]
    {
        if (on_hide_tooltip)
        {
            on_hide_tooltip();
        }
    };
    message_list_->on_scroll_to_original =
        [this](const std::string& original_event_id)
    {
        if (on_scroll_to_original)
        {
            on_scroll_to_original(original_event_id);
        }
    };
}

// ── Providers ─────────────────────────────────────────────────────────────

void RoomView::set_avatar_provider(MessageListView::ImageProvider p)
{
    // The same provider goes to both the header (for the room avatar) and
    // the message list (for per-sender avatars).
    if (header_)
    {
        header_->set_avatar_provider(p);
    }
    if (message_list_)
    {
        message_list_->set_avatar_provider(std::move(p));
    }
}

void RoomView::set_image_provider(MessageListView::ImageProvider p)
{
    if (message_list_)
    {
        message_list_->set_image_provider(std::move(p));
    }
}

void RoomView::set_preview_provider(MessageListView::PreviewProvider p)
{
    if (message_list_)
    {
        message_list_->set_preview_provider(std::move(p));
    }
}

void RoomView::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    if (message_list_)
    {
        message_list_->set_audio_player(std::move(player));
    }
}

void RoomView::set_voice_bytes_provider(MessageListView::VoiceBytesProvider p)
{
    if (message_list_)
    {
        message_list_->set_voice_bytes_provider(std::move(p));
    }
}

void RoomView::set_repaint_requester(std::function<void()> f)
{
    if (message_list_)
    {
        message_list_->set_repaint_requester(std::move(f));
    }
}

void RoomView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    if (message_list_)
    {
        message_list_->set_post_delayed(std::move(f));
    }
}

void RoomView::set_video_player_factory(MessageListView::VideoPlayerFactory f)
{
    if (message_list_)
    {
        message_list_->set_video_player_factory(std::move(f));
    }
}

void RoomView::set_video_fetch_provider(MessageListView::VideoFetchProvider f)
{
    if (message_list_)
    {
        message_list_->set_video_fetch_provider(std::move(f));
    }
}

// ── Historical-mode helpers ────────────────────────────────────────────────

void RoomView::set_historical_mode(bool historical)
{
    if (message_list_)
    {
        message_list_->set_historical_mode(historical);
    }
}

bool RoomView::scroll_to_event_id(const std::string& id)
{
    return message_list_ && message_list_->scroll_to_event_id(id);
}

// ── Room / message state ───────────────────────────────────────────────────

void RoomView::set_room(const tesseract::RoomInfo& info)
{
    has_room_ = true;
    if (header_)
    {
        header_->set_room(info);
    }
    if (compose_bar_)
    {
        compose_bar_->set_enabled(true);
    }
}

void RoomView::clear_room()
{
    has_room_ = false;
    if (compose_bar_)
    {
        compose_bar_->set_enabled(false);
    }
}

void RoomView::set_messages(std::vector<MessageRowData> msgs, bool room_switch)
{
    if (message_list_)
    {
        message_list_->set_messages(std::move(msgs), room_switch);
    }
}

void RoomView::insert_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->insert_message(index, std::move(msg));
    }
}

void RoomView::update_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->update_message(index, std::move(msg));
    }
}

void RoomView::remove_message(std::size_t index)
{
    if (message_list_)
    {
        message_list_->remove_message(index);
    }
}

void RoomView::append_message(MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->append_message(std::move(msg));
    }
}

void RoomView::notify_image_ready(const std::string& url)
{
    if (message_list_)
    {
        message_list_->notify_image_ready(url);
    }
}

void RoomView::notify_url_preview_ready(const std::string& url)
{
    if (message_list_)
    {
        message_list_->notify_url_preview_ready(url);
    }
}

void RoomView::set_typing_text(std::string text)
{
    // The typing indicator is now a synthetic trailing row inside the
    // message list, so it scrolls with the timeline and is hidden when
    // the user scrolls up.
    if (message_list_)
    {
        message_list_->set_typing_text(std::move(text));
    }
}

// ── Compose integration ────────────────────────────────────────────────────

void RoomView::set_text_area_natural_height(float h)
{
    if (compose_bar_)
    {
        compose_bar_->set_text_area_natural_height(h);
    }
}

void RoomView::set_current_text(std::string text)
{
    if (compose_bar_)
    {
        compose_bar_->set_current_text(std::move(text));
    }
}

void RoomView::clear_compose_text()
{
    if (compose_bar_)
    {
        compose_bar_->set_current_text({});
    }
}

bool RoomView::edit_last_own()
{
    if (!compose_bar_ || !message_list_)
    {
        return false;
    }
    // Don't hijack Up while the user is already mid-edit or composing a
    // reply — that would silently discard their in-progress context.
    if (compose_bar_->has_editing() || compose_bar_->has_reply())
    {
        return false;
    }
    return message_list_->edit_last_own();
}

tk::Rect RoomView::compose_text_area_rect() const
{
    // text_area_rect() inside ComposeBar is computed from bounds_ (world
    // coords) so the rect is already in surface space — no offset needed.
    return compose_bar_ ? compose_bar_->text_area_rect() : tk::Rect{};
}

// ── tk::Widget overrides ───────────────────────────────────────────────────

tk::Size RoomView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // RoomView fills the entire surface it's mounted on.
    return constraints;
}

void RoomView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    if (!has_room_)
    {
        if (brand_view_)
        {
            brand_view_->arrange(ctx, bounds);
        }
        return;
    }

    const float compose_h =
        compose_bar_ ? compose_bar_->natural_height() : ComposeBar::kMinHeight;

    const float header_h = header_
                               ? header_->measure(ctx, {bounds.w, bounds.h}).h
                               : RoomHeader::kHeight;
    const float header_bottom = bounds.y + header_h;
    const float compose_top = bounds.y + bounds.h - compose_h;
    // The message list spans header → composer; the typing indicator is a
    // synthetic trailing row inside it, so no strip space is reserved here.
    const float msg_h = std::max(0.0f, compose_top - header_bottom);

    if (header_)
    {
        header_->arrange(ctx, {bounds.x, bounds.y, bounds.w, header_h});
    }

    if (message_list_)
    {
        message_list_->arrange(ctx, {bounds.x, header_bottom, bounds.w, msg_h});
    }

    if (compose_bar_)
    {
        compose_bar_->arrange(ctx,
                              {bounds.x, compose_top, bounds.w, compose_h});
    }
}

void RoomView::paint(tk::PaintCtx& ctx)
{
    if (!has_room_)
    {
        if (brand_view_)
        {
            brand_view_->paint(ctx);
        }
        return;
    }

    // Each child paints its own background; the typing indicator now lives
    // inside the message list (synthetic trailing row), so RoomView paints
    // no strip of its own.
    if (header_)
    {
        header_->paint(ctx);
    }
    if (message_list_)
    {
        message_list_->paint(ctx);
    }

    if (compose_bar_)
    {
        compose_bar_->paint(ctx);
    }
}

} // namespace tesseract::views
