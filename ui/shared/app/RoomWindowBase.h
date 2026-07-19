#pragma once
#include "app/ThreadPanelController.h"
#include "views/MentionController.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "tk/host.h"
#include "tk/theme.h"
#include <tesseract/image_pack.h>
#include <tesseract/mentions.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{
class ImageViewerOverlay;
class VideoViewerOverlay;
class ForwardRoomPicker;
class RoomMediaView;
} // namespace tesseract::views

namespace tesseract
{

class ShellBase;
class Client;

// Base class for secondary (pop-out) room windows. Each instance owns the
// per-window state for one room: the borrowed RoomView pointer, compose
// typing state, and the link back to ShellBase for dispatch.
//
// Platform subclasses create a native window + surface, set room_view_, then
// call finish_init_() to register with ShellBase and start the event feed.
// The destructor unregisters and releases the subscription automatically.
class RoomWindowBase
{
public:
    RoomWindowBase(ShellBase* shell, std::string room_id);
    virtual ~RoomWindowBase();

    const std::string& room_id() const
    {
        return room_id_;
    }
    views::RoomView* room_view() const
    {
        return room_view_;
    }

    // Called by ShellBase on the UI thread when SDK events arrive for this room.
    void on_room_info_updated(const RoomInfo& r);
    void on_timeline_reset(std::vector<views::MessageRowData> rows);
    void on_message_inserted(std::size_t idx, views::MessageRowData row);
    void on_message_updated(std::size_t idx, views::MessageRowData row);
    void on_message_removed(std::size_t idx);
    void on_typing_changed(const std::string& text, bool visible);

    // Thread view delivery — called by ShellBase when SDK events arrive for
    // the thread this pop-out has open (popout_thread_root_ matches). Each
    // method mirrors the corresponding handle_thread_*_ui_ path used by the
    // main window but targets this window's room_view_->thread_view().
    const std::string& popout_thread_root() const { return popout_thread_root_; }
    void apply_thread_reset_(std::vector<views::MessageRowData> rows);
    void apply_thread_prepend_(std::vector<views::MessageRowData> rows);
    void apply_thread_append_(std::vector<views::MessageRowData> rows);
    void apply_thread_insert_(std::size_t index, views::MessageRowData row);
    void apply_thread_update_(std::size_t index, views::MessageRowData row);
    void apply_thread_remove_(std::size_t index);

    // Fan-in for async GIF search results. ShellBase forwards every result to
    // every open pop-out (the GIF search request_id is process-global, so only
    // the controller that issued it matches; the rest drop it). Default no-op
    // for subclasses without a GIF strip; the Qt/GTK/Win32 pop-outs override to
    // forward into their GifController.
    virtual void on_gif_results(std::uint64_t /*request_id*/,
                                std::vector<GifResult> /*results*/)
    {
    }
    virtual void on_gif_search_failed(std::uint64_t /*request_id*/,
                                      const std::string& /*message*/)
    {
    }

    // Fan-in for async message-forward completions, mirroring on_gif_results
    // above: ShellBase::forward_event's request_id is process-global, so
    // ShellBase checks its own main-window pending_forwards_ first, then
    // calls this on every open pop-out until one recognizes the id. Returns
    // true if this window issued request_id (and updated its own picker
    // accordingly), so the caller can stop looking.
    bool handle_forward_done_(std::uint64_t request_id);
    bool handle_forward_failed_(std::uint64_t request_id,
                                const std::string& message);

    // Called by ShellBase::tick_anim_ on every animation frame so this window's
    // animated images (inline media, and any open pickers) advance even when
    // the pointer is still. The base repaints the room surface; subclasses
    // override to also repaint visible emoji/sticker pickers.
    virtual void repaint_anim_frame();

    // Returns true if this secondary window is currently on-screen (not
    // minimized, not hidden). Default: true — conservative, so platforms
    // that do not override never accidentally starve the animation timer.
    virtual bool is_visible() const { return true; }

    // Platform overrides.
    virtual void bring_to_front() = 0;
    virtual void close_window() = 0;
    virtual void request_relayout() = 0;
    virtual void update_window_title_(const std::string& /*name*/)
    {
    }
    // Re-theme this pop-out window's surface (and any native chrome).
    // Called by the shell's apply_theme_ui_() so secondary windows track
    // the theme setting just like the main window.
    virtual void apply_theme(const tk::Theme& t) = 0;

protected:
    // Call at the end of the subclass constructor, after surface + room_view_
    // are ready. Registers with ShellBase, acquires subscription, and
    // populates the view with the room's current state.
    void finish_init_();

    // Install the shared RoomView providers + compose callbacks on `rv`
    // (which the subclass has just created and parented to its surface).
    // Call after creating room_view_ and before finish_init_(). Uses the
    // shared SDK helpers + shell_ caches, routing the three per-shell
    // primitives through surface_repaint_(), compose_text_area_(), and
    // preview_lookup_(). Surface-bound providers that need the subclass's
    // own surface_ (set_audio_player / set_post_delayed / on_layout_changed)
    // stay in the subclass ctor.
    //
    // If img_viewer_ and vid_viewer_ are non-null (set before this call),
    // on_image_clicked and on_video_clicked are wired to open them locally.
    // Also wires RoomView::media_upload_limit_provider/media_info_extractor/
    // on_file_drop_outcome — the drop-into-compose-bar catch-all reached via
    // RoomView::on_file_drop, now driven purely by tree dispatch (see
    // tk::Widget::on_file_drop) rather than a per-surface flat callback. A
    // subclass's surface-level native drop entry point needs no wiring of
    // its own beyond calling its own Host::dispatch_file_drop.
    void wire_room_view_(views::RoomView* rv);

    // The single per-shell repaint primitive (surface->update() /
    // gtk_widget_queue_draw / InvalidateRect / surface->relayout()).
    virtual void surface_repaint_() = 0;

    // Compose text widget to clear after a successful send / prefill on edit
    // / focus on reply. Self-owned by room_view_'s ComposeBar (see
    // ComposeBar::text_area()) — no subclass override needed; kept virtual
    // only so a future shell without a real Host could still opt out.
    virtual tk::TextArea* compose_text_area_()
    {
        return room_view_ ? room_view_->compose_bar()->text_area() : nullptr;
    }

    // This pop-out's forward-message picker overlay, or nullptr if the
    // subclass hasn't added one to its PopoutRoomWidget yet.
    virtual views::ForwardRoomPicker* forward_picker_()
    {
        return nullptr;
    }
    // Focus/hide this pop-out's own native forward-picker search field
    // (mirrors ShellBase::focus_forward_picker_field_/hide_forward_picker_field_
    // for the main window, but scoped per-window since each pop-out needs its
    // own NativeTextField overlay). Default no-op until a platform adds one.
    virtual void focus_forward_picker_field_() {}
    virtual void hide_forward_picker_field_() {}

    // This pop-out's room-media gallery overlay, or nullptr if the subclass
    // hasn't added one to its PopoutRoomWidget yet.
    virtual views::RoomMediaView* room_media_view_()
    {
        return nullptr;
    }

    // Build the outgoing (body, formatted_body) pair from the live compose
    // text area's mention/emoticon draft (matrix.to links + m.mentions for
    // pills, <img data-mx-emoticon> for custom emoji) via
    // tesseract::build_mention_message. Falls back to {fallback_body, ""}
    // when there is no text area or its draft is empty (plain text, no
    // pills). Shared by on_send / on_thread_send / on_thread_send_reply so
    // thread sends carry the same rich formatting as room sends instead of
    // silently dropping it.
    tesseract::MarkdownResult draft_outgoing_message_(
        const std::string& fallback_body);

    // Encode raw image bytes for sending via the subclass's surface host
    // (tk::Host::encode_for_send). Surface-bound, so it can't live in the base;
    // wire_room_view_'s on_send_image uses it to compress/normalise before
    // upload. `compress` follows the user's image-quality setting.
    virtual tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                              std::size_t size,
                                              bool compress) = 0;

    // Look up a cached URL-preview card for `url`, or nullptr. Reads from
    // ShellBase::url_preview_data_; every shell shares the same cache.
    const views::UrlPreviewData* preview_lookup_(const std::string& url);

    // Post a deferred call to ShellBase::release_owned_window_(this) on the UI
    // thread. Call from WM_DESTROY (Win32) or the platform destroy callback
    // so the C++ object is deleted safely outside its own message handler.
    void schedule_self_close_();

    // Look up the saved geometry for this room's popout from Settings,
    // validated against available screens. Returns {valid=false} when there
    // is no saved entry — callers should fall back to their platform default.
    // Intended for use in the subclass constructor, after shell_ is set.
    Settings::WindowGeometry get_saved_popout_geometry_(int default_w,
                                                        int default_h) const;

    // Write the current native window position/size into Settings and
    // schedule a debounced save. Call from resize/move callbacks.
    // Pass dpi=GetDpiForWindow(hwnd) on Win32 so that geometry can be
    // rescaled correctly when restored on a monitor at a different DPI.
    void save_popout_geometry_(int x, int y, int w, int h, int dpi = 0);

    // Like get_saved_popout_geometry_ but rescales w/h from the saved DPI
    // to target_dpi before clamping. Pass target_dpi=0 to skip rescaling
    // (equivalent to the no-target_dpi overload). Win32 callers should pass
    // GetDpiForMonitor(...) for the monitor at the saved position.
    Settings::WindowGeometry get_saved_popout_geometry_(int default_w,
                                                        int default_h,
                                                        int target_dpi) const;

    // Remove this room's entry from Settings::popout_windows and save.
    // Called automatically by the destructor; also callable on explicit close.
    void remove_popout_from_settings_();

    // SDK operation helpers — forward to shell_->client_ (accessible via
    // friend class ShellBase). All must be called on the UI thread.
    // The shell's Client (for member fetches in the mention controller etc.).
    tesseract::Client* shell_client_() const;

    void send_message_(const std::string& body);
    void send_message_(const std::string& body,
                       const std::string& formatted_body);
    void send_reply_(const std::string& reply_event_id,
                     const std::string& body);
    void send_sticker_(const std::string& body, const std::string& image_url,
                       const std::string& info_json);
    // Fetch the device's current location and send it to this window's room.
    // Forwards to ShellBase::send_current_location_(room_id_).
    void send_current_location_();
    void send_edit_(const std::string& event_id, const std::string& new_body,
                    bool is_caption = false);
    void delete_event_(const std::string& event_id);
    void toggle_reaction_(const std::string& event_id, const std::string& key,
                          const std::string& source_mxc);
    void send_receipt_(const std::string& event_id);
    void send_typing_notice_(bool typing);
    void retry_send_(const std::string& txn_id);
    void abort_send_(const std::string& txn_id);
    void pin_event_(const std::string& event_id);
    void unpin_event_(const std::string& event_id);
    // Resolve/create a DM with user_id, then open (or focus) a SEPARATE
    // pop-out for it — unlike ShellBase::handle_open_dm_, which navigates
    // the main window itself, this window's own room_id_ must stay put.
    void open_dm_(std::string user_id);
    // Open/close this pop-out's room-media gallery for its own room_id_.
    // Simpler than ShellBase's main-window version: reuses this window's
    // existing request_pagination_back_() (one batch per scroll-to-top, no
    // retry/accumulate-until-enough-media loop) rather than duplicating
    // ShellBase's more elaborate per-gesture retry budget.
    void open_room_media_view_();
    void close_room_media_view_();
    void request_pagination_back_();

    // Image cache accessors — friend access to ShellBase protected members
    // so platform subclasses don't need their own friend declarations.
    const tk::Image* shell_avatar_(const std::string& mxc) const;
    const tk::Image* shell_image_(const std::string& mxc) const;
    void shell_show_status_message_(std::string msg, int auto_clear_ms = 4000);

    // This pop-out's own shortcode-popup suggestion source (personal +
    // this window's own room_id_ + subscribed rooms) — a fresh filtered
    // list per call, via ShellBase::emoticons_for_room_(room_id_), NOT the
    // main window's current room.
    std::vector<tesseract::ImagePackImage> shell_emoticons_() const;
    // Every Space (direct and ancestor) that this pop-out's room_id_ is in
    // — forwards to ShellBase::parent_spaces_for_room_(room_id_). Used to
    // feed the pop-out's own emoji/sticker pickers so they can surface
    // those spaces' packs alongside the room's own.
    std::vector<std::string> shell_parent_spaces_for_room_() const;
    // Trigger an async fetch+decode of a media image (e.g. a custom emoticon
    // thumbnail) into the shell cache so shell_image_() resolves it on a later
    // repaint. Idempotent. Forwards to ShellBase::ensure_media_image_.
    void shell_ensure_media_image_(const std::string& url, int w, int h);

    // Render one GIF strip cell via the shell's backend-specific provider; the
    // pop-out passes a `repaint` that refreshes its own GIF popup surface when
    // async media lands. Forwards to ShellBase::gif_strip_image_.
    const tk::Image* shell_gif_strip_image_(const GifResult& result,
                                            const std::function<void()>& repaint);
    // Source bytes the GIF strip cached on fetch — reused so a selected GIF
    // sends without a second download. Forwards to ShellBase.
    std::vector<std::uint8_t> shell_cached_gif_bytes_(const std::string& url);

    // Wire the shell-backed mention-popup hooks shared by every pop-out window:
    // a live client getter (so the controller never holds a stale snapshot), an
    // avatar prefetch into the shell cache, and the popup's avatar image
    // provider (peeks that same cache). Call after creating the popup widget
    // and before constructing the MentionController.
    void wire_mention_shell_hooks_(views::MentionPopup* popup,
                                   views::MentionController::Hooks& hooks);
    // Kick off an async task on the shell's worker-thread pool. Safe to call
    // from the UI thread; fn runs on a background thread with no UI access.
    void run_async_(std::function<void()> fn);

    // Like run_async_, but routed to the shell's mutation pool so write
    // operations (set topic, leave, ignore) stay ordered relative to sends.
    void run_async_mut_(std::function<void()> fn);

    // Post a callback to the UI thread (from any thread).
    void post_to_ui_(std::function<void()> fn);

    // Fetch source_json bytes on a background thread and write them to
    // dest_path on the UI thread. No-op if bytes are empty (fetch failed).
    void save_source_to_file_(std::string source_json,
                               std::string dest_path);

    // Fetch source_json bytes and hand them to on_ready — the friend-access
    // equivalent of ShellBase::begin_media_req_/client_->fetch_source_bytes_async
    // for platform subclasses, which aren't themselves ShellBase friends.
    // Used by RoomView::set_video_fetch_provider for inline timeline
    // autoplay video/GIF.
    void fetch_source_bytes_(
        const std::string& src,
        std::function<void(std::vector<std::uint8_t>)> on_ready);

    // Fetch source_json bytes and place the decoded image on the system
    // clipboard via put_image_on_clipboard_. Shared body for the image
    // lightbox's copy button; no-op if bytes are empty.
    void copy_source_to_clipboard_(std::string source_json);

    // Put a decoded image (from `bytes`, a self-describing encoded blob) on the
    // clipboard via this window's surface host. Surface-bound, so it can't live
    // in the base (mirrors encode_for_send_); each shell forwards to
    // surface_->host().set_clipboard_image(bytes).
    virtual bool
    put_image_on_clipboard_(std::span<const std::uint8_t> bytes) = 0;

    // Put `text` on the system clipboard via this window's surface host.
    // Surface-bound (like put_image_on_clipboard_); each shell forwards to
    // surface_->host().set_clipboard_text(text). Used by
    // copy_event_source_to_clipboard_.
    virtual void set_clipboard_text_(std::string_view text) = 0;

    // Show a brief "toast" confirmation pill (e.g. "Copied to clipboard")
    // via this window's surface host. Surface-bound (like the others
    // above); each shell forwards to surface_->host().show_toast(message).
    virtual void show_toast_(std::string message) = 0;

    // Look up event_id's raw JSON via Client::get_event_source() and place it
    // on the clipboard via set_clipboard_text_. Shared body for the "Copy
    // event source" overflow-menu item (Settings::developer_mode-gated); no-op
    // if the room isn't known, there's no active client, or the event isn't
    // in the room's currently-loaded timeline.
    void copy_event_source_to_clipboard_(std::string event_id);

    // Trigger fetch of a full-res image into the shared tk_images_ cache.
    // Idempotent: no-op if already cached or in-flight.
    void ensure_viewer_image_(const std::string& url);

    // Image provider for an emoji/sticker picker, shared with the main window:
    // synchronous lookup against the shell's animated + static caches, with an
    // async decode/fetch on miss. The (cache_key, source_token) signature
    // matches the shared EmojiPicker/StickerPicker ImageProvider alias.
    std::function<const tk::Image*(const std::string&, const std::string&)>
    picker_image_provider_(bool is_sticker);

    // Apply the side-effects of a thread-panel transition for this pop-out:
    // subscribe/unsubscribe on the client, update popout_thread_panel_ state,
    // drive room_view_->set_thread_panel(), and refresh the thread-list
    // snapshot when entering List mode.
    void apply_popout_thread_transition_(
        const ThreadPanelController::ThreadTransition& t);
    // Kick a thread-list pagination pass for this pop-out's room, using the
    // per-popout controller (popout_thread_ctl_) rather than ShellBase's.
    void paginate_popout_threads_();

    ShellBase* shell_;
    std::string room_id_;
    views::RoomView* room_view_ =
        nullptr; // borrowed; owned by surface widget tree
    // Room-switch member-list cache backing the received-mention-pill avatar
    // provider (set_mention_avatar_provider) — holds names + avatar_urls only;
    // no avatar bytes are fetched until a pill actually paints.
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;
    // Media overlays — set by the subclass ctor before wire_room_view_().
    // When non-null, image/video click callbacks are wired to open them.
    views::ImageViewerOverlay* img_viewer_ = nullptr; // borrowed
    views::VideoViewerOverlay* vid_viewer_ = nullptr; // borrowed
    // Shared liveness token: set to false in destructor so background-thread
    // lambdas can detect that this object is gone before posting to UI.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool compose_typing_active_ = false;
    // First timeline reset = initial fill of this pop-out (gate the display
    // like a room switch); later resets are reconnect/gappy refreshes of the
    // room already shown (refresh in place, no blank).
    bool displayed_once_ = false;
    // Dedup for on_visible_range_changed's lazy media fetch — mirrors
    // ShellBase::media_prepped_event_ids_, scoped to this pop-out window.
    std::unordered_set<std::string> visible_media_prepped_;
    // In-flight message forwards started from THIS window's picker — mirrors
    // ShellBase::pending_forwards_, keyed by the same process-global
    // request_id space (ShellBase::next_request_id_).
    std::unordered_map<std::uint64_t, std::string> pending_forwards_;
    // Media-fetch group id for this pop-out's gallery, distinct from
    // media_group_for_room_(room_id_) (used by the room's normal inline
    // media) so cancel_media_group_ on gallery close never touches unrelated
    // fetches. Set on open_room_media_view_(), same salt ShellBase uses.
    std::uint64_t media_view_group_ = 0;

    // Per-popout thread-panel state (separate from ShellBase's main-window
    // state so each window tracks its own panel independently).
    using ThreadPanel    = ThreadPanelController::ThreadPanel;
    using ThreadTrigger  = ThreadPanelController::ThreadTrigger;
    ThreadPanel popout_thread_panel_      = ThreadPanel::Closed;
    ThreadPanel popout_thread_panel_prev_ = ThreadPanel::Closed;
    std::string popout_thread_root_;
    ThreadPanelController popout_thread_ctl_;
};

} // namespace tesseract
