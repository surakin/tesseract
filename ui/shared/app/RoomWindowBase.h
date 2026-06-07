#pragma once
#include "views/MentionController.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "tk/host.h"
#include "tk/theme.h"
#include <tesseract/image_pack.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{
class ImageViewerOverlay;
class VideoViewerOverlay;
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

    // Called by ShellBase::tick_anim_ on every animation frame so this window's
    // animated images (inline media, and any open pickers) advance even when
    // the pointer is still. The base repaints the room surface; subclasses
    // override to also repaint visible emoji/sticker pickers.
    virtual void repaint_anim_frame();

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
    void wire_room_view_(views::RoomView* rv);

    // Shared drag-and-drop ingest for pop-out windows. A subclass wires its
    // surface's set_on_file_drop to forward here. Routes the payload via
    // views::dispatch_file_drop into this window's compose bar (honouring the
    // shell's media upload limit), running the shell's per-platform media probe
    // (shell_->extract_drop_media_) retargeted to this window's compose bar and
    // alive_ token. Over-limit and empty payloads are dropped silently
    // (pop-outs have no status bar).
    void handle_file_drop_(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename);

    // The single per-shell repaint primitive (surface->update() /
    // gtk_widget_queue_draw / InvalidateRect / surface->relayout()).
    virtual void surface_repaint_() = 0;

    // Compose text widget to clear after a successful send / prefill on edit
    // / focus on reply, or nullptr if the shell has no native text area and
    // drives compose state through room_view_ instead. All four pop-out
    // subclasses currently return their own native text area.
    virtual tk::NativeTextArea* compose_text_area_()
    {
        return nullptr;
    }

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
    void save_popout_geometry_(int x, int y, int w, int h);

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
    void send_edit_(const std::string& event_id, const std::string& new_body);
    void delete_event_(const std::string& event_id);
    void toggle_reaction_(const std::string& event_id, const std::string& key,
                          const std::string& source_mxc);
    void send_receipt_(const std::string& event_id);
    void send_typing_notice_(bool typing);
    void retry_send_(const std::string& txn_id);
    void abort_send_(const std::string& txn_id);
    void request_pagination_back_();

    // Image cache accessors — friend access to ShellBase protected members
    // so platform subclasses don't need their own friend declarations.
    const tk::Image* shell_avatar_(const std::string& mxc) const;
    const tk::Image* shell_image_(const std::string& mxc) const;
    void shell_show_status_message_(std::string msg, int auto_clear_ms = 4000);

    // The shell's MSC2545 emoticon flat list (ShellBase::cached_emoticons_) —
    // the shortcode popup's suggestion source, shared across every composer.
    const std::vector<tesseract::ImagePackImage>& shell_emoticons_() const;
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
    std::vector<std::uint8_t>
    fetch_source_bytes_(const std::string& source_json);

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

    // Trigger fetch of a full-res image into the shared tk_images_ cache.
    // Idempotent: no-op if already cached or in-flight.
    void ensure_viewer_image_(const std::string& url);

    // Image provider for an emoji/sticker picker, shared with the main window:
    // synchronous lookup against the shell's animated + static caches, with an
    // async decode/fetch on miss. The (cache_key, source_token) signature
    // matches the shared EmojiPicker/StickerPicker ImageProvider alias.
    std::function<const tk::Image*(const std::string&, const std::string&)>
    picker_image_provider_(bool is_sticker);

    ShellBase* shell_;
    std::string room_id_;
    views::RoomView* room_view_ =
        nullptr; // borrowed; owned by surface widget tree
    // Media overlays — set by the subclass ctor before wire_room_view_().
    // When non-null, image/video click callbacks are wired to open them.
    views::ImageViewerOverlay* img_viewer_ = nullptr; // borrowed
    views::VideoViewerOverlay* vid_viewer_ = nullptr; // borrowed
    // Shared liveness token: set to false in destructor so background-thread
    // lambdas can detect that this object is gone before posting to UI.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool compose_typing_active_ = false;
    bool typing_bar_visible_ = false;
    // First timeline reset = initial fill of this pop-out (gate the display
    // like a room switch); later resets are reconnect/gappy refreshes of the
    // room already shown (refresh in place, no blank).
    bool displayed_once_ = false;
};

} // namespace tesseract
