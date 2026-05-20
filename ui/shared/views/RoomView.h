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

#include "BrandView.h"
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

namespace tesseract::views
{

class RoomView : public tk::Widget
{
public:
    RoomView();
    ~RoomView() override = default;

    // ── Providers (forwarded to MessageListView / RoomHeader) ────────────

    void set_avatar_provider(MessageListView::ImageProvider p);
    void set_image_provider(MessageListView::ImageProvider p);
    void set_preview_provider(MessageListView::PreviewProvider p);
    void set_audio_player(std::unique_ptr<tk::AudioPlayer> player);
    void set_voice_bytes_provider(MessageListView::VoiceBytesProvider p);
    void set_repaint_requester(std::function<void()> f);
    void set_post_delayed(std::function<void(int, std::function<void()>)> f);
    void set_video_player_factory(MessageListView::VideoPlayerFactory f);
    void set_video_fetch_provider(MessageListView::VideoFetchProvider f);

    // ── Room / message state ─────────────────────────────────────────────

    // Update room header and enable the compose bar. Must be called when the
    // user navigates to a room; RoomView is deliberately inert before this.
    void set_room(const tesseract::RoomInfo& info);

    // Reset to the brand-view state (no active room). Call when switching
    // accounts or logging out so the splash reappears.
    void clear_room();

    void set_messages(std::vector<MessageRowData> msgs,
                      bool room_switch = false);
    void insert_message(std::size_t index, MessageRowData msg);
    void update_message(std::size_t index, MessageRowData msg);
    void remove_message(std::size_t index);
    void append_message(MessageRowData msg);

    void notify_image_ready(const std::string& url);
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
    RoomHeader* header() const
    {
        return header_;
    }
    ComposeBar* compose_bar() const
    {
        return compose_bar_;
    }
    MessageListView* message_list() const
    {
        return message_list_;
    }

    // "Press Up in an empty composer to edit your last message." Wired by
    // the shell to the NativeTextArea's set_on_edit_last hook. No-op (and
    // returns false so Up keeps default caret behaviour) while already
    // editing or composing a reply, or when there is no editable own
    // message. Returns true when an edit was started (consume the key).
    bool edit_last_own();

    // ── External callbacks — wire to SDK ─────────────────────────────────

    // Plain text send.
    std::function<void(std::string body)> on_send;

    // Reply send.
    std::function<void(std::string reply_event_id, std::string body)>
        on_send_reply;

    // Edit send.
    std::function<void(std::string event_id, std::string new_body)>
        on_send_edit;

    // Image send. `is_animated` is true for animated GIF/WebP — the host
    // sends via the MSC4230 raw path instead of re-encoding.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption, int w, int h,
                       bool is_animated, std::string reply_event_id)>
        on_send_image;

    // Video send. `thumb_bytes` is a JPEG first-frame thumbnail (empty when
    // unavailable) with dimensions `thumb_w`/`thumb_h`; `duration_ms`
    // populates `info.duration`. Wired to real routing in a later task.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption, int w, int h,
                       std::vector<std::uint8_t> thumb_bytes, int thumb_w,
                       int thumb_h, std::uint64_t duration_ms,
                       std::string reply_event_id)>
        on_send_video;

    // Audio send (plain m.audio, not MSC3245 voice). `duration_ms`
    // populates `info.duration`. Wired to real routing in a later task.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::uint64_t duration_ms, std::string reply_event_id)>
        on_send_audio;

    // File send.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::string reply_event_id)>
        on_send_file;

    std::function<void()> on_mic_clicked;
    std::function<void()> on_cancel_voice;

    std::function<void(std::string event_id)> on_delete_requested;
    std::function<void(std::string event_id, std::string emoji)>
        on_reaction_toggled;
    std::function<void(std::string event_id, tk::Rect anchor)>
        on_add_reaction_requested;
    std::function<void(std::string url)> on_link_clicked;
    std::function<void(std::string url)> on_link_hovered;
    std::function<void(std::string event_id)> on_receipt_needed;
    std::function<void(MessageListView::ImageHit)> on_image_clicked;
    std::function<void(MessageListView::VideoHit)> on_video_clicked;
    std::function<void(MessageListView::FileHit)> on_file_clicked;
    std::function<void()> on_near_top;
    std::function<void()> on_near_bottom;
    std::function<void()> on_return_to_live;
    std::function<void()> on_jump_to_date_requested;
    std::function<void(std::string original_event_id)> on_scroll_to_original;

    // Fired when the pointer enters a topic text that was truncated (i.e. the
    // topic didn't fit and shows an ellipsis). Shell should show a tooltip with
    // the full text anchored to `anchor`. on_hide_tooltip fires when the
    // pointer leaves, so the shell can dismiss it.
    std::function<void(std::string text, tk::Rect anchor)> on_show_tooltip;
    std::function<void()> on_hide_tooltip;
    std::function<void(tk::Rect)> on_emoji;
    std::function<void(tk::Rect)> on_sticker;

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
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void wire_internal_callbacks();

    bool has_room_ = false; // true after the first set_room() call

    std::function<void()> repaint_requester_;
    std::function<void(int, std::function<void()>)> post_delayed_;
    bool anim_repaint_pending_ = false;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    BrandView* brand_view_ = nullptr;
    RoomHeader* header_ = nullptr;
    MessageListView* message_list_ = nullptr;
    ComposeBar* compose_bar_ = nullptr;
};

} // namespace tesseract::views
