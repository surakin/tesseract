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
#include "ConfirmDialog.h"
#include "MessageListView.h"
#include "PinnedBanner.h"
#include "PopupMenu.h"
#include "RoomHeader.h"
#include "RoomInfoPanel.h"
#include "RoomSearchBar.h"
#include "RoomSettingsView.h"
#include "ThreadListView.h"
#include "ThreadView.h"
#include "UserProfilePanel.h"
#include "media_drop.h"
#ifdef TESSERACT_CALLS_ENABLED
#include "CallOverlayWidget.h"
#include "IncomingCallBanner.h"
#endif

#include "tk/audio.h"
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
    void set_image_acquirer(MessageListView::ImageAcquirer a);
    void set_shortcode_provider(MessageListView::ShortcodeProvider p);
    void set_preview_provider(MessageListView::PreviewProvider p);
    void set_audio_player(std::unique_ptr<tk::AudioPlayer> player);
    void set_voice_bytes_provider(MessageListView::VoiceBytesProvider p);
    void set_repaint_requester(std::function<void()> f);
    void set_post_delayed(std::function<void(int, std::function<void()>)> f);
    void set_video_player_factory(MessageListView::VideoPlayerFactory f);
    void set_video_fetch_provider(MessageListView::VideoFetchProvider f);

    // Closure that opens a confirmation dialog with the given options. Set by
    // MainAppWidget to route through its shared ConfirmDialog overlay; if
    // unset, destructive callbacks fire directly without confirmation.
    using ConfirmProvider = std::function<void(ConfirmDialog::Options,
                                                std::function<void()>)>;
    void set_confirm_provider(ConfirmProvider p);

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
    void prepend_messages(std::vector<MessageRowData> rows);
    void append_messages(std::vector<MessageRowData> rows);

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

    // Surface-space rect of the full compose bar (used to anchor the full-width
    // GIF strip just above it). Valid after the first arrange() pass.
    tk::Rect compose_bar_rect() const;

    // ── Historical-mode helpers (forwarded to MessageListView) ──────────

    // Toggle historical display mode (pill stays visible; clicking it
    // fires on_return_to_live instead of scrolling to the bottom).
    void set_historical_mode(bool historical);

    // Show/hide the back-pagination spinner (delegates to MessageListView).
    void set_paginating(bool paginating);

    // Suppress the main timeline's expensive relayout work while it's
    // fully covered by another overlay (the room media gallery) — see
    // MessageListView::set_relayout_suppressed(). Data mutations (new
    // messages, pagination) still apply normally underneath.
    void set_message_list_relayout_suppressed(bool suppressed);

    // Pause/resume inline autoplay video on a main-window visibility edge —
    // see MessageListView::set_video_playback_suspended().
    void set_message_list_video_suspended(bool suspended);

    // Scroll to the row matching event_id. Returns true when found.
    bool scroll_to_event_id(const std::string& id);

    // Open the user-profile panel for user_id without a room-member context
    // (display_name and avatar_url may be empty; the panel fills them in).
    void open_user_profile(const std::string& user_id);

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
    RoomInfoPanel* room_info_panel() const
    {
        return room_info_panel_;
    }
    RoomSettingsView* room_settings_view() const
    {
        return room_settings_view_;
    }
    UserProfilePanel* user_profile_panel() const
    {
        return user_profile_panel_;
    }
    ThreadView* thread_view() const
    {
        return thread_view_;
    }
    PopupMenu* overflow_menu() const
    {
        return overflow_menu_;
    }
    ThreadListView* thread_list_view() const
    {
        return thread_list_view_;
    }
    PinnedBanner* pinned_banner() const
    {
        return pinned_banner_;
    }

    // ── In-room search bar ("find in conversation") ──────────────────────

    // Open the docked search strip (also reveals the header search button).
    void open_room_search();
    // Close the search strip, clear match highlights and focused event.
    void close_room_search();

    bool      has_room()                 const { return has_room_; }
    bool      room_search_open()         const;
    tk::Rect  room_search_field_rect()   const;
    bool      room_search_field_visible() const;

    RoomSearchBar* room_search_bar() const;

    // Forwarded to the shell. Shell debounces and calls Client::search_messages
    // with the current room id.
    std::function<void(const std::string& query)> on_room_search_query;
    // delta: -1 = older/UP, +1 = newer/DOWN.
    std::function<void(int delta)> on_room_search_navigate;
    std::function<void(bool enabled)> on_room_search_paginate_toggled;
    // Fired by close_room_search() so ShellBase can stop the paginate loop.
    std::function<void()> on_room_search_closed;

    // ── Pinned events ────────────────────────────────────────────────────

    // Drive the pinned-events banner + the per-message Pin/Unpin button
    // state. set_pinned() lazily creates the banner widget the first time a
    // non-empty pin list arrives; the banner shrinks to zero-height when
    // empty so the message list reclaims the row. set_can_pin() gates the
    // hover Pin/Unpin button in the message list based on the user's PL.
    void set_pinned(std::vector<tesseract::PinnedEvent> pins);
    void set_can_pin(bool can_pin);

    // Forwarded by RoomView → shell. Fired when the user clicks the hover
    // Pin / Unpin button on a message row in the main timeline.
    std::function<void(const std::string& event_id)> on_pin_requested;
    std::function<void(const std::string& event_id)> on_unpin_requested;

    // Fixed width of the floating right-side thread overlay (panel paints
    // on top of the message list rather than reshaping it). Clamped to
    // `bounds.w` so narrow windows still get a sensible overlay.
    static constexpr float kThreadPanelWidth = 420.0f;

    // Mirror enum (avoids include cycle with ShellBase::ThreadPanel).
    enum class ThreadPanelState
    {
        Closed,
        List,
        Open,
    };
    void set_thread_panel(ThreadPanelState state,
                          const std::string& root_event_id);
    ThreadPanelState thread_panel_state() const
    {
        return thread_panel_state_;
    }

    // Show or hide the threads button in the room header. Driven by the shell
    // from the latest SDK thread-list snapshot — hidden when empty.
    void set_show_threads_button(bool show);

    // Forwarded by RoomView → shell.
    std::function<void()> on_threads_button_clicked;
    std::function<void(const std::string& root_event_id)>
        on_thread_open_requested;
    std::function<void()> on_thread_close_requested;
    std::function<void(const std::string& body,
                       const std::string& formatted_body)>
        on_thread_send;
    // Reply send routed through the active thread instead of the room. Fires
    // when compose_bar_->on_send_reply triggers and thread_panel_state_ ==
    // Open; the shell calls send_thread_reply on the SDK.
    std::function<void(const std::string& reply_to_event_id,
                       const std::string& body,
                       const std::string& formatted_body)>
        on_thread_send_reply;

    // "Press Up in an empty composer to edit your last message." Wired by
    // the shell to the NativeTextArea's set_on_edit_last hook. No-op (and
    // returns false so Up keeps default caret behaviour) while already
    // editing or composing a reply, or when there is no editable own
    // message. Returns true when an edit was started (consume the key).
    bool edit_last_own();

    void set_room_members(std::vector<tesseract::RoomMember> members);
    tk::Rect    topic_edit_rect() const;
    bool        topic_edit_visible() const;
    void        set_topic_edit_text(std::string t);
    std::string topic_edit_initial_text() const;

    // True when any room-view-owned modal (room-info or user-profile panel)
    // is currently up. MainAppWidget combines this with its own ConfirmDialog
    // state to hide the compose-textarea + room-search NativeTextField while
    // overlays cover the canvas.
    bool is_overlay_open() const;

    // Forward DM button state to the user profile panel.
    void set_dm_button_state(UserProfilePanel::DmButtonState state);

    // Close the user profile panel if open and trigger a repaint.
    void close_user_profile();

#ifdef TESSERACT_CALLS_ENABLED
    // Show the incoming-call banner for the given room+slot. Caller name is
    // displayed in the banner; Answer fires on_start_call; Decline + the
    // auto-dismiss timer both call dismiss_call_banner(). lifetime_ms controls
    // the auto-dismiss timeout (0 → falls back to 30 s).
    void show_call_banner(const std::string& room_id,
                          const std::string& slot_id,
                          const std::string& caller_display_name,
                          const std::string& call_intent,
                          std::uint64_t      lifetime_ms);
    void dismiss_call_banner();
    // Returns true while the incoming-call banner is visible.
    bool call_banner_visible() const;

    // Fired when the user answers (banner or header button). The shell calls
    // ShellBase::start_call(room_id, slot_id, audio_only).
    // audio_only=true skips video capture; audio_only=false starts with video.
    std::function<void(const std::string& room_id,
                       const std::string& slot_id,
                       bool               audio_only)> on_start_call;

    // ── Docked call panel ────────────────────────────────────────────────
    // post_delayed signature matches the post_delayed_ provider used by
    // CallOverlayWidget's elapsed-time ticker.
    using PostDelayedFn = std::function<void(int, std::function<void()>)>;

    // Creates and mounts a CallOverlayWidget child in Docked or DockedExpanded
    // mode. Returns a borrowed pointer (ownership stays in the widget tree).
    views::CallOverlayWidget* mount_call_panel(
        views::CallOverlayWidget::Mode        initial_mode,
        PostDelayedFn                         post_delayed,
        std::function<void()>                 repaint_requester,
        std::function<const tk::Image*(const std::string&)> avatar_provider,
        std::function<std::string(const std::string&)>      display_name_provider);

    // Removes and destroys the CallOverlayWidget child.
    void unmount_call_panel();

    // Returns the active docked call panel, or nullptr if not mounted.
    views::CallOverlayWidget* call_panel() const { return call_panel_; }
#endif

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
    std::function<void(std::string event_id)> on_forward_requested;
    // `source_mxc` is the mxc:// URI for MSC4027 custom-image reactions,
    // empty for plain Unicode. Hosts route empty → `Client::send_reaction`,
    // non-empty → `Client::send_reaction_custom`.
    std::function<void(std::string event_id, std::string key,
                       std::string source_mxc)>
        on_reaction_toggled;
    std::function<void(std::string event_id, tk::Rect anchor)>
        on_add_reaction_requested;
    std::function<void(std::string url)> on_link_clicked;
    std::function<void(std::string url)> on_link_hovered;
    std::function<void(std::string event_id)> on_receipt_needed;
    // Forwarded from MessageListView (both the main list and the thread panel):
    // the visible rows' media tokens changed; the host re-prioritizes them.
    std::function<void(const std::vector<std::string>&)> on_visible_range_changed;
    // Forwarded from MessageListView: the visible rows' avatar mxc URLs changed;
    // the host calls ensure_user_avatar_ for each so only visible avatars are fetched.
    std::function<void(const std::vector<std::string>&)> on_visible_avatars_changed;
    std::function<void(MessageListView::ImageHit)> on_image_clicked;
    std::function<void(MessageListView::VideoHit)> on_video_clicked;
    std::function<void(MessageListView::FileHit)> on_file_clicked;
    std::function<void()> on_near_top;
    std::function<void()> on_near_bottom;
    std::function<void()> on_return_to_live;
    std::function<void(std::uint64_t ts_ms)> on_date_jump;
    std::function<void(std::string original_event_id)> on_scroll_to_original;

    // Clipboard write — forward to the platform host. Wire to
    // Host::set_clipboard_text in the shell.
    std::function<void(std::string_view)> on_set_clipboard;

    // Fired from set_room() so the shell can ensure the room's avatar thumbnail
    // is fetched and cached before the header paints.
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    std::function<void(std::string room_id)>                on_fetch_notification_mode;
    std::function<void(std::string room_id, std::string)>   on_notification_mode_changed;
    std::function<void(std::string room_id, bool)>          on_favourite_changed;
    std::function<void(std::string room_id, bool)>          on_low_priority_changed;
    std::function<void(std::string room_id)>                on_fetch_room_members;
    std::function<void(std::string room_id, std::string t)> on_save_topic;
    std::function<void(std::string room_id)>                on_leave_room;
    // Fired when the user clicks the "Media (N)" row in RoomInfoPanel. The
    // shell opens the MainAppWidget-level RoomMediaView overlay for room_id.
    std::function<void(std::string room_id)>                on_media_view_requested;
    // Fired when RoomSettingsView opens, so the shell can push per-field
    // permissions (can_set_room_name/topic/avatar).
    std::function<void(std::string room_id)>                on_room_settings_opened;
    // Fired when the user clicks the room-settings avatar disc to pick a
    // new image. The shell uploads it (Client::upload_media) and calls
    // room_settings_view()->set_staged_avatar() — never commits directly.
    std::function<void(std::string room_id)>
        on_room_settings_avatar_upload_requested;
    std::function<void(std::string user_id)>                on_open_dm;
    // Predicate: return true when a DM with user_id already exists.
    // Set by the shell (ShellBase wires this to find_existing_dm_).
    std::function<bool(const std::string& user_id)>         on_has_dm;
    std::function<void(std::string user_id)>                on_ignore_user;
    std::function<void(std::string avatar_url, std::string display_name)>
                                                            on_avatar_clicked;

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

    // Fired when the user clicks in the compose card area (not a button).
    // Shell should focus the native text area overlay.
    std::function<void()> on_focus_input;

    // ── Drop-into-compose-bar wiring ─────────────────────────────────────
    // Set once by the shell (alongside RoomView's other providers), not
    // per-drop. Used by on_file_drop, the catch-all leaf handler reached
    // when a dropped file doesn't land on anything more specific (e.g. the
    // room/space image-pack grid, which claims it first via
    // RoomSettingsView being the active overlay panel).
    std::function<std::uint64_t()> media_upload_limit_provider;
    MediaInfoExtractor              media_info_extractor;
    std::function<void(FileDropOutcome)> on_file_drop_outcome;

    // ── tk::Widget overrides ─────────────────────────────────────────────

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    // Pointer/hit-test routing. The overlay panels (RoomInfoPanel /
    // UserProfilePanel) paint last (on top) but are created before the
    // lazily-added pinned banner and thread panels, so the default
    // child-vector dispatch order would let those underlying widgets steal
    // clicks the panel covers. While a panel is open we route all input to
    // it so it intercepts events first, matching its paint order.
    tk::Widget* hit_test(tk::Point world) override;
    tk::Widget* dispatch_pointer_down(tk::Point world) override;
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty) override;
    tk::Widget* dispatch_right_click(tk::Point world) override;
    bool        dispatch_wheel(tk::Point world, float dx, float dy) override;
    // Same overlay-first routing as above, for a dropped file — e.g. while
    // RoomSettingsView is open, a drop anywhere in the room reaches its
    // image-pack grid (or is discarded if it misses the dialog) rather than
    // falling through to the compose bar behind it.
    tk::Widget* dispatch_file_drop(tk::Point world, tk::FileDropPayload& payload) override;
    // Catch-all leaf: reached only when dispatch_file_drop's base-class walk
    // finds no more specific claimant. Routes into compose_bar_ by MIME via
    // views::route_file_drop_to_compose_bar.
    bool on_file_drop(tk::Point local, tk::FileDropPayload& payload) override;
    // Same overlay-first routing, for drag-hover feedback.
    tk::Widget* dispatch_drag_hover(tk::Point world) override;
    // Claims whenever on_file_drop would (mirrors its compose_bar_->enabled()
    // gate) and shows a highlight scoped to compose_bar_'s own bounds — not
    // RoomView's whole content pane — since that's where the drop will land.
    bool on_drag_hover(tk::Point local) override;
    void on_drag_leave() override;

private:
    // The open overlay panel that should receive input ahead of all other
    // children, or nullptr when none is open.
    tk::Widget* active_overlay_panel_() const;

    // Transparent overlay placed on top of the main MessageListView while the
    // thread panel is open. It eats hover events (so the timeline doesn't
    // surface hover affordances behind the focused thread) and treats any
    // click as a "close the thread panel" request. Defined in RoomView.cpp.
    class MessageBlocker;

    void wire_internal_callbacks();
    // Wire every MessageListView callback that is identical between the main
    // timeline and the thread-panel timeline. Called from
    // wire_internal_callbacks() for message_list_ and from set_thread_panel()
    // for thread_view_->message_list() so action buttons (reply / edit /
    // redact, reactions, media clicks, etc.) work in both contexts.
    void wire_message_list_callbacks_(MessageListView* ml);
    // Recompute the room-info panel's "Media (N)" count from
    // message_list_->messages() and push it. Gated on the panel being open
    // (except from show_room_info(), which always seeds it) so mutations in
    // rooms nobody is looking at don't pay the O(n) rescan.
    void refresh_media_count_();
    void show_room_info();
    void show_room_settings();
    void show_user_profile(std::string user_id, std::string display_name,
                           std::string avatar_url);

    bool has_room_ = false; // true after the first set_room() call
    bool drag_hover_ = false; // true while claiming on_drag_hover

    std::function<void()> repaint_requester_;
    std::function<void(int, std::function<void()>)> post_delayed_;
    bool anim_repaint_pending_ = false;

    ConfirmProvider confirm_provider_;

    // Cached so show_room_info() can open the panel with the current room data.
    tesseract::RoomInfo current_room_info_;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    BrandView* brand_view_ = nullptr;
    RoomHeader* header_ = nullptr;
    MessageListView* message_list_ = nullptr;
    ComposeBar* compose_bar_ = nullptr;
    RoomInfoPanel*    room_info_panel_    = nullptr;
    RoomSettingsView* room_settings_view_ = nullptr;
    UserProfilePanel* user_profile_panel_ = nullptr;
    PopupMenu*        overflow_menu_      = nullptr;
    PopupMenu*        call_popup_         = nullptr;
    // Stored so they can be forwarded to the lazily-created thread view.
    MessageListView::ImageProvider stored_avatar_provider_;
    MessageListView::ImageProvider stored_image_provider_;
    MessageListView::ImageAcquirer stored_image_acquirer_;
    // Lazily created when the thread panel first opens. Owned by the tk
    // child list (add_child); we keep a borrowed pointer for access.
    ThreadView*     thread_view_      = nullptr;
    ThreadListView* thread_list_view_ = nullptr;
    // Lazily created the first time set_pinned() is called with a non-empty
    // list. Owned by the tk child list (add_child); we keep a borrowed
    // pointer for access. Stays in the child list (with set_visible(false))
    // after the room's pins drop back to empty so subsequent set_pinned()
    // calls don't re-add it.
    PinnedBanner*   pinned_banner_    = nullptr;
    // Click/hover blocker over message_list_, visible only while the
    // thread panel is open. See the nested class doc above.
    MessageBlocker*  message_blocker_   = nullptr;
#ifdef TESSERACT_CALLS_ENABLED
    // Incoming-call banner — created in constructor (hidden), shown/dismissed
    // via show_call_banner() / dismiss_call_banner(). Stores pending room+slot
    // so the answer callback can pass them to on_start_call.
    IncomingCallBanner* call_banner_ = nullptr;
    std::string         call_banner_room_id_;
    std::string         call_banner_slot_id_;
    std::uint64_t       call_banner_dismiss_gen_ = 0;
    // Docked call panel — lazily created by mount_call_panel(), removed by
    // unmount_call_panel(). nullptr when no call is active.
    views::CallOverlayWidget* call_panel_ = nullptr;
#endif
    // Docked search strip under the header; nullptr until first open.
    RoomSearchBar*   room_search_bar_   = nullptr;
    ThreadPanelState thread_panel_state_ = ThreadPanelState::Closed;
    std::string thread_panel_root_;
    // Current room's members (mirrors what was last passed to
    // set_room_members) — used to resolve a clicked mention's display name
    // and avatar for the profile panel.
    std::vector<tesseract::RoomMember> room_members_;
};

} // namespace tesseract::views
