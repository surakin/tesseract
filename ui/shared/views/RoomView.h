#pragma once

// Shared room-chat view. Owns and lays out three child widgets:
//   1. RoomHeader  — 60 px strip (avatar, name, topic)
//   2. MessageListView — flex middle area (scrollable message log)
//   3. ComposeBar  — dynamic-height bottom strip (text input + attachments)
//
// A 20 px typing-indicator strip sits between the message list and compose
// bar; it is painted directly (no child widget) and always reserves its
// space even when empty.
//
// The shell mounts this widget as the root of a single tk::Surface and
// overlays one NativeTextArea at compose_text_area_rect() in the surface's
// set_on_layout callback. The shell is responsible for all NativeTextArea
// state (set_text, insert_at_cursor, set_focused) and calls
// set_text_area_natural_height / set_current_text to feed changes back in.
//
// Internal wiring (reply → compose, edit → compose, compose sends) is done
// inside the constructor so the shell only wires SDK-touching callbacks.

#include "ComposeBar.h"
#include "MessageListView.h"
#include "RoomHeader.h"

#include "tk/audio.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views {

class RoomView : public tk::Widget {
public:
    RoomView();
    ~RoomView() override = default;

    // ── Providers (forwarded to MessageListView / RoomHeader) ────────────

    void set_avatar_provider(MessageListView::ImageProvider p);
    void set_image_provider (MessageListView::ImageProvider p);
    void set_preview_provider(MessageListView::PreviewProvider p);
    void set_audio_player   (std::unique_ptr<tk::AudioPlayer> player);
    void set_voice_bytes_provider(MessageListView::VoiceBytesProvider p);
    void set_repaint_requester   (std::function<void()> f);
    void set_video_player_factory(MessageListView::VideoPlayerFactory f);
    void set_video_fetch_provider(MessageListView::VideoFetchProvider f);

    // ── Room / message state ─────────────────────────────────────────────

    // Update room header and enable the compose bar. Must be called when the
    // user navigates to a room; RoomView is deliberately inert before this.
    void set_room(const tesseract::RoomInfo& info);

    void set_messages  (std::vector<MessageRowData> msgs);
    void insert_message(std::size_t index, MessageRowData msg);
    void update_message(std::size_t index, MessageRowData msg);
    void remove_message(std::size_t index);
    void append_message(MessageRowData msg);

    void notify_image_ready      (const std::string& url);
    void notify_url_preview_ready(const std::string& url);

    // Typing indicator text (e.g. "Alice is typing…"). Pass an empty string
    // to clear. Triggers on_layout_changed so the surface repaints.
    void set_typing_text(std::string text);

    // ── Compose integration (shell owns NativeTextArea) ──────────────────

    // Called by the shell from NativeTextArea on_height_changed.
    void set_text_area_natural_height(float h);

    // Called by the shell from NativeTextArea on_changed.
    void set_current_text(std::string text);

    // Clear compose bar text state (call alongside roomTextArea_->set_text("")).
    void clear_compose_text();

    // Surface-space rect the shell should use to position the NativeTextArea.
    // Valid after the first arrange() pass.
    tk::Rect compose_text_area_rect() const;

    // ── Historical-mode helpers (forwarded to MessageListView) ──────────

    // Toggle historical display mode (pill stays visible; clicking it
    // fires on_return_to_live instead of scrolling to the bottom).
    void set_historical_mode(bool historical);

    // Scroll to the row matching event_id. Returns true when found.
    bool scroll_to_event_id(const std::string& id);

    // ── Direct accessors for shell integration ───────────────────────────

    // Needed by the shell for: emoji/sticker picker anchor (popupAt),
    // pending attachment (set_pending_image / set_pending_file),
    // and cursor insert after emoji selection.
    ComposeBar*      compose_bar()  const { return compose_bar_;  }
    MessageListView* message_list() const { return message_list_; }

    // ── External callbacks — wire to SDK ─────────────────────────────────

    // Plain text send.
    std::function<void(std::string body)> on_send;

    // Reply send.
    std::function<void(std::string reply_event_id, std::string body)> on_send_reply;

    // Edit send.
    std::function<void(std::string event_id, std::string new_body)> on_send_edit;

    // Image send.
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string mime,
                       std::string filename,
                       std::string caption,
                       int w, int h,
                       std::string reply_event_id)> on_send_image;

    // File send.
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string mime,
                       std::string filename,
                       std::string caption,
                       std::string reply_event_id)> on_send_file;

    std::function<void(std::string event_id)>              on_delete_requested;
    std::function<void(std::string event_id,
                       std::string emoji)>                 on_reaction_toggled;
    std::function<void(std::string event_id,
                       tk::Rect anchor)>                   on_add_reaction_requested;
    std::function<void(std::string url)>                   on_link_clicked;
    std::function<void(std::string event_id)>              on_receipt_needed;
    std::function<void(MessageListView::ImageHit)>         on_image_clicked;
    std::function<void(MessageListView::VideoHit)>         on_video_clicked;
    std::function<void()>                                  on_near_top;
    std::function<void()>                                  on_near_bottom;
    std::function<void()>                                  on_return_to_live;
    std::function<void()>                                  on_jump_to_date_requested;
    std::function<void(std::string original_event_id)>     on_scroll_to_original;
    std::function<void()>                                  on_emoji;
    std::function<void()>                                  on_sticker;

    // Fired when the compose bar or typing indicator changes the internal
    // layout. Shell should call roomSurface_->relayout() in response.
    std::function<void()> on_layout_changed;

    // Fired when edit mode is entered (e.g. user clicked ✏ hover button).
    // The shell should call roomTextArea_->set_text(body) and set_focused.
    std::function<void(std::string body)> on_edit_prefill;

    // Fired when the user clicks ✕ on the edit banner. Shell should call
    // roomTextArea_->set_text("") and clear_compose_text().
    std::function<void()> on_edit_cancelled;

    // Fired when reply mode is entered so the shell can focus the textarea.
    std::function<void()> on_reply_focus;

    // ── tk::Widget overrides ─────────────────────────────────────────────

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    void wire_internal_callbacks();

    // Child widgets — owned via add_child, raw pointers borrowed back.
    RoomHeader*      header_       = nullptr;
    MessageListView* message_list_ = nullptr;
    ComposeBar*      compose_bar_  = nullptr;
};

} // namespace tesseract::views
