#pragma once

// Shared message list. Renders a `std::vector<MessageRowData>` as
// avatar + sender name + body (text or inline media or file card) +
// reactions + timestamp. Variable row heights — sizes are recomputed
// when the data set or list width changes.
//
// The data model is deliberately flat — integration code unpacks the
// SDK's polymorphic Event hierarchy into MessageRowData on the UI
// thread so the shared view doesn't see virtual Events.

#include "tk/audio.h"
#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/video.h"
#include "views/map_tiles.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{

struct MessageRowData
{
    enum class Kind
    {
        Text,
        Image,
        Sticker,
        File,
        Audio,
        Voice,
        Video,
        Redacted,
        Notice,
        Emote,
        Unhandled,
        DaySeparator,
        ReadMarker,
        TimelineStart,
        Location,
    };

    Kind kind = Kind::Text;
    std::string event_id;
    std::string sender; // canonical Matrix ID
    std::string sender_name;
    std::string sender_avatar_url; // mxc
    std::string body;
    std::string formatted_body; // HTML; empty when plain text only
    std::uint64_t timestamp_ms = 0;
    bool is_own = false;

    // Image / Sticker / Video — typed media sources.
    // `source`    carries the full-resolution (or video) media source.
    // `thumbnail` carries the thumbnail source; nullptr when the server omits one.
    // For video, the client-side first-frame generator fills `image_provider_`
    // under the key `"thumb::" + event_id` when thumbnail is nullptr.
    tesseract::MediaSourceRef source;
    tesseract::MediaSourceRef thumbnail;
    int media_w = 0;
    int media_h = 0;
    // MSC2530 caption — non-empty for `m.image` events whose sender
    // supplied a distinct `filename`, in which case `body` is a user
    // caption to render beneath the image.
    bool has_filename_caption = false;
    // MSC4230: sender flagged this image/sticker as animated.
    bool image_animated = false;
    // JSON-serialised ImageInfo from the sticker event (empty for Kind::Image).
    std::string sticker_info_json;

    // File card
    tesseract::MediaSourceRef file_source; // file attachment
    std::string file_name;
    std::uint64_t file_size = 0;

    // Voice (MSC3245) and Audio (plain m.audio).
    // `waveform` is the MSC1767 amplitude list (each 0..=1024); empty for
    // Kind::Audio (no waveform) and for voice messages that omitted it
    // (view paints flat placeholder bars in that case).
    tesseract::MediaSourceRef audio_source;
    std::string audio_mime;
    std::uint64_t duration_ms = 0;
    std::vector<std::uint16_t> waveform;

    // Video (m.video). Uses `source` / `thumbnail` from the image/sticker
    // slots above. `media_w`/`media_h`/`duration_ms` are shared.
    std::string video_mime; // e.g. "video/mp4"; empty when absent
    // fi.mau.* hints — each false by default.
    bool video_autoplay = false;
    bool video_loop = false;
    bool video_no_audio = false;
    bool video_hide_controls = false;
    bool video_gif = false;

    std::vector<tesseract::Reaction> reactions;
    /// Users (excluding the current user) whose latest read receipt landed
    /// on this event. The view renders up to a small cap of mini-avatars
    /// at the bottom-right of the row; the rest fall into a "+N" overflow.
    std::vector<tesseract::ReadReceipt> read_receipts;

    // Reply reference (m.in_reply_to). All three are empty when not a reply.
    std::string in_reply_to_id;
    std::string in_reply_to_sender_name;
    std::string in_reply_to_body;
    bool has_reply() const
    {
        return !in_reply_to_id.empty();
    }

    // Set when the message has been superseded by an m.replace edit.
    bool is_edited = false;

    // First http(s) URL found in the message body. Non-empty only for
    // Kind::Text / Kind::Unhandled. Used to fetch and display a preview card.
    std::string first_url;

    // MSC2448: xyz.amorgan.blurhash placeholder string; empty when absent.
    std::string blurhash;

    // Optimistic send state (own messages only).
    enum class PendingState
    {
        None,
        Sending,
        Failed
    };
    PendingState pending_state = PendingState::None;
    std::string pending_txn_id;
    std::string pending_error;
    bool pending_recoverable = false;
    // Set for ~2 s after a Sending → None transition to show ✓.
    bool just_sent = false;

    // Location (m.location / MSC3488)
    double location_lat = 0.0;
    double location_lon = 0.0;
    std::string location_description;
    tesseract::views::MapViewport map_viewport; // mutable: updated by pan/zoom
};

// Convert a raw SDK Event into the flat MessageRowData the shared view
// consumes. `my_user_id` is used to set `is_own` on the returned row.
MessageRowData make_row_data(const tesseract::Event& ev,
                             const std::string& my_user_id);

struct UrlPreviewData
{
    std::string title;
    std::string description;
    std::string image_mxc; // mxc:// URI, or empty
    int image_w = 0;
    int image_h = 0;
    bool has_content() const
    {
        return !title.empty() || !description.empty();
    }
};

class MessageListView : public tk::ListView
{
public:
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc_or_url)>;
    using PreviewProvider =
        std::function<const UrlPreviewData*(const std::string& url)>;
    // Resolves the bare shortcode (no surrounding colons) for an mxc://
    // URI from the user's MSC2545 image packs. Returns an empty string
    // when the mxc isn't in any of the user's emoticon packs. Used to
    // render `:shortcode:` instead of the raw mxc URI in the MSC4027
    // image-reaction chip tooltip.
    using ShortcodeProvider =
        std::function<std::string(const std::string& mxc)>;

    MessageListView();
    ~MessageListView() override; // out-of-line — Adapter is opaque here

    // Bulk-replace the messages. Re-measures + repaints. When
    // `room_switch` is true the list is held invisible (background only)
    // until the rows that will be visible have their height-affecting
    // content (images / stickers / video thumbnails / URL-preview cards)
    // loaded and final heights measured, or a short timeout elapses — so
    // a freshly-entered room appears once, already correct, instead of
    // flashing and reflowing as async media arrives.
    void set_messages(std::vector<MessageRowData> msgs,
                      bool room_switch = false);

    // Tell a live room-switch gate that the timeline opened in
    // jump-to-event (focused) mode so the gate evaluates around the
    // focused row and the post-gate reveal re-pins to it instead of the
    // bottom. No-op when no gate is active. Call right after
    // set_messages(.., true) + relayout, before set_historical_mode.
    void begin_focused_gate(const std::string& focus_event_id);
    const std::vector<MessageRowData>& messages() const
    {
        return messages_;
    }

    // Find the most recent own, editable (Kind::Text, fully sent) message
    // and fire `on_edit_requested` for it. Drives the "press Up in an empty
    // composer to edit your last message" shortcut. Returns true when a
    // message was found and the callback fired; false otherwise (caller
    // lets Up fall through to default caret handling).
    bool edit_last_own();

    // Insert `msg` at visible index `index` (0 = front, == size() = append).
    // The single entry point that mirrors `VectorDiff::Insert` /
    // `PushBack` / `PushFront`. Preserves the user's visual scroll
    // position when `index` lands above the viewport, and follows the
    // live tail when `index == size()` and the user was already pinned
    // to the bottom.
    void insert_message(std::size_t index, MessageRowData msg);

    // Replace the row at visible `index` with `msg`. Used for edits,
    // redactions, reaction changes, and sender-profile resolution.
    void update_message(std::size_t index, MessageRowData msg);

    // Remove the row at visible `index`. Preserves the user's scroll
    // position when the removed row was above the viewport.
    void remove_message(std::size_t index);

    // Append a single message (typical live-update path) and scroll to
    // the bottom if the user was already pinned there. Thin wrapper over
    // `insert_message(size(), msg)`.
    void append_message(MessageRowData msg);

    // Avatar bytes come from the host-side media cache. Returning null
    // falls back to an initials disc.
    void set_avatar_provider(ImageProvider p);

    // Inline image / sticker bytes come from the same kind of cache.
    void set_image_provider(ImageProvider p);

    // Wire the mxc → shortcode lookup used by the MSC4027 reaction tooltip.
    void set_shortcode_provider(ShortcodeProvider p);

    // URL preview cards. Returns UrlPreviewData* for the given URL or nullptr
    // when data is not yet available (returning nullptr is a signal to the shell
    // to trigger a fetch as a side-effect).
    void set_preview_provider(PreviewProvider p);

    // Called by the shell after storing new preview data in the provider cache.
    // Invalidates row heights and, if the affected row is entirely above the
    // current viewport, arms the scroll anchor so the user's visual position
    // is preserved (preview card grows upward, not into the visible region).
    void notify_url_preview_ready(const std::string& url);

    // Set/clear the "X is typing…" indicator. Rendered as a synthetic
    // trailing row at the end of the list (not part of the SDK timeline
    // model), so it scrolls with the tail and is hidden when the user
    // scrolls up. Empty string removes the row. Stays pinned to the
    // bottom when the user was already there.
    void set_typing_text(std::string text);

    // Called by the host shell when a media image finishes decoding and is
    // stored in the image_provider cache. Invalidates cached row heights so
    // Kind::Image rows that were measured against a zero-dimension placeholder
    // are remeasured with the actual pixel dimensions.
    void notify_image_ready(const std::string& url);

    // Update the waveform of the first voice row matching `event_id` and
    // repaint. Called after local waveform generation completes for a voice
    // message that arrived without MSC1767 waveform data.
    void update_voice_waveform(const std::string&              event_id,
                               std::vector<std::uint16_t>      waveform);

    // Voice-message playback (MSC3245). Shells wire all three after
    // construction; the view stays inert (clicks become no-ops) when any
    // of them is missing — Win32 currently lacks an audio backend, so
    // `make_audio_player()` returns nullptr there and the player handle
    // is never set.
    using VoiceBytesProvider = std::function<std::vector<std::uint8_t>(
        const std::string& source_json)>;
    void set_audio_player(std::unique_ptr<tk::AudioPlayer> player);
    void set_voice_bytes_provider(VoiceBytesProvider provider);
    void set_repaint_requester(std::function<void()> request_repaint);

    // Host-backed delayed-callback hook. Wired by every shell (and
    // RoomWindow) to Host::post_delayed. Drives the room-switch gate's
    // timeout fallback so the list never stays invisible forever on a
    // slow / offline network. May be left unset (tests / shells without
    // a timer) — the gate then relies solely on the notify_* re-eval.
    void set_post_delayed(std::function<void(int, std::function<void()>)> f);

    // Inline video playback for fi.mau.autoplay / fi.mau.gif rows.
    // Both must be set for inline playback to activate; if either is missing
    // the view falls back to the static thumbnail (clicks still open the overlay).
    using VideoPlayerFactory =
        std::function<std::unique_ptr<tk::VideoPlayer>()>;
    using VideoFetchProvider = std::function<void(
        const std::string& source_json,
        std::function<void(std::vector<std::uint8_t>)> on_ready)>;
    void set_video_player_factory(VideoPlayerFactory f);
    void set_video_fetch_provider(VideoFetchProvider f);

    // Called during paint when a tile is missing from the image cache.
    // Wire to ShellBase::ensure_tile_async() in RoomWindowBase::finish_init_().
    std::function<void(int z, int x, int y)> on_tile_needed;

    // Click hooks. on_message_clicked fires on row click.
    std::function<void(const std::string& event_id)> on_message_clicked;

    // Fired when the user clicks a message sender's avatar or display name.
    std::function<void(std::string user_id, std::string display_name,
                       std::string avatar_url)> on_sender_clicked;

    // Reaction-chip clicks. `key` is the emoji (or `:shortcode:`) the
    // user tapped. `source_mxc` is the mxc:// URI for MSC4027 custom-image
    // reactions, empty for plain Unicode. The host should call
    // `Client::send_reaction` when `source_mxc` is empty (Rust toggle
    // semantics handle add/remove); for non-empty `source_mxc`, route to
    // `Client::send_reaction_custom(room, ev, source_mxc, key)` so the
    // outgoing event carries the MSC4027 mxc:// key and shortcode.
    std::function<void(const std::string& event_id,
                       const std::string& key,
                       const std::string& source_mxc)>
        on_reaction_toggled;

    // Add-reaction button (the trailing "+" pseudo-chip that appears on
    // row hover). The host should open the emoji picker anchored near
    // `anchor`, then call `send_reaction` with the chosen glyph.
    std::function<void(const std::string& event_id, tk::Rect anchor)>
        on_add_reaction_requested;

    // Reply affordance — fires when the user clicks the "↩" reply button
    // that appears on hover. The host should call
    // `compose_bar_->set_reply_to(event_id, sender_name, body_preview)`.
    std::function<void(const std::string& event_id,
                       const std::string& sender_name,
                       const std::string& body_preview)>
        on_reply_requested;

    // Fires when the user clicks a URL preview card or an inline hyperlink.
    std::function<void(const std::string& url)> on_link_clicked;

    // Fires when the pointer enters or leaves an inline hyperlink. url is
    // non-empty while hovering, empty when the pointer leaves. Used by the
    // shell to switch the cursor to/from a pointing-hand cursor.
    std::function<void(const std::string& url)> on_link_hovered;

    // Fires to show/hide a native tooltip. `anchor` is in world coordinates.
    std::function<void(std::string text, tk::Rect anchor)> on_show_tooltip;
    std::function<void()> on_hide_tooltip;

    // Fires when the user clicks the quote block of a reply to scroll to
    // the original message. If the event is currently loaded the view scrolls
    // to it internally; this callback fires only when the original is not found
    // in the current message list (host should back-paginate + scroll).
    std::function<void(const std::string& original_event_id)>
        on_scroll_to_original;

    // Edit affordance — fires when the user clicks the "✏" edit button on a
    // hover row they own. Only fires for Kind::Text rows. The host should call
    // `compose_bar_->set_editing(event_id)`, then pre-fill the text area.
    std::function<void(const std::string& event_id,
                       const std::string& current_body)>
        on_edit_requested;

    // Delete affordance — fires when the user clicks the "🗑" delete button on
    // a hover row they own. Fires for any non-Redacted own message kind.
    // The host should call `Client::redact_event(room_id, event_id, "")`.
    std::function<void(const std::string& event_id)> on_delete_requested;

    // Fired when a local echo transitions Sending → None (message confirmed).
    std::function<void(const std::string& event_id)> on_just_sent;

    // Fired when the user clicks the Retry button on a failed own message.
    std::function<void(const std::string& txn_id)> on_retry_send;

    // Fired when the user clicks the ✕ (abort) button on a failed own message.
    std::function<void(const std::string& txn_id)> on_abort_send;

    // Clear the just_sent flag on the row matching `event_id` and repaint.
    void clear_just_sent(const std::string& event_id);

    // Sticker right-click hit. Native shells call this with the world-coord
    // pointer position from their secondary-click handler. Returns the
    // sticker event metadata when the click lands on a Kind::Sticker rect,
    // or `std::nullopt` otherwise. The shell decides whether to show a
    // context menu (e.g. skip when `Client::user_pack_has_sticker` is true).
    struct StickerHit
    {
        std::string event_id;
        tesseract::MediaSourceRef source;
        std::string body;
        std::string info_json; // JSON-serialised ImageInfo from the sticker event
        tk::Rect world_rect;
    };
    std::optional<StickerHit> sticker_hit_at(tk::Point world) const;

    // Image / sticker left-click hit — fires `on_image_clicked`.
    struct ImageHit
    {
        std::string event_id;
        tesseract::MediaSourceRef source;    // full-res
        tesseract::MediaSourceRef thumbnail; // nullptr when absent
        std::string body;  // MSC2530 caption, may be empty
        int natural_w = 0;
        int natural_h = 0;
        tk::Rect world_rect;
    };
    std::optional<ImageHit> image_hit_at(tk::Point world) const;

    /// Fires when the user left-clicks an image or sticker thumbnail.
    std::function<void(const MessageListView::ImageHit&)> on_image_clicked;

    // Video thumbnail left-click hit — fires `on_video_clicked`.
    struct VideoHit
    {
        std::string event_id;
        tesseract::MediaSourceRef source;    // video source
        tesseract::MediaSourceRef thumbnail; // nullptr when absent
        std::string mime_type;
        int natural_w = 0;
        int natural_h = 0;
        std::uint64_t duration_ms = 0;
        // fi.mau.* hints forwarded to VideoViewerOverlay::open().
        bool autoplay = false;
        bool loop = false;
        bool no_audio = false;
        bool hide_controls = false;
        bool gif = false;
        tk::Rect world_rect;
    };
    std::optional<VideoHit> video_hit_at(tk::Point world) const;

    /// Fires when the user left-clicks a video thumbnail card.
    std::function<void(const MessageListView::VideoHit&)> on_video_clicked;

    // File card left-click hit — fires `on_file_clicked`.
    struct FileHit
    {
        std::string event_id;
        tesseract::MediaSourceRef source; // file source
        std::string file_name; // suggested save filename
        uint64_t    file_size = 0;
        tk::Rect    world_rect;
    };
    std::optional<FileHit> file_hit_at(tk::Point world) const;

    /// Fires when the user left-clicks a file card.
    std::function<void(const MessageListView::FileHit&)> on_file_clicked;

    // Fired with the event_id of the newest real (non-virtual) message
    // visible in the viewport when that id changes. The host should call
    // Client::send_read_receipt for the active room.
    std::function<void(const std::string& event_id)> on_receipt_needed;

    // Clipboard write. Wire to Host::set_clipboard_text via RoomView.
    std::function<void(std::string_view)> on_set_clipboard;

    // Called by on_right_click when there is an active selection. The shell
    // should show a native context menu with a "Copy" item and call
    // copy_selection() if chosen. Falls back to copy_selection() if unset.
    std::function<void()> on_show_copy_menu;

    // True when the user has dragged out a non-empty selection.
    bool has_selection() const;

    // Copy the current selection to the clipboard via on_set_clipboard.
    // No-op when has_selection() is false or on_set_clipboard is unset.
    void copy_selection();

    // Widget overrides — own pointer-move/down/up so we can hit-test
    // reaction chips before the ListView base sees the event.
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_drag(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy) override;
    bool on_right_click(tk::Point local) override;
    void paint(tk::PaintCtx&) override;

    // Per-chip geometry for the currently hovered row. Populated by
    // `Adapter::paint_row` during the row's paint pass (geometry is
    // recomputed there because chip width depends on text measurement);
    // consumed by `on_pointer_move` / `on_pointer_down` on subsequent
    // events. Stored in world coordinates. Public for tests.
    struct RowChipGeom
    {
        std::size_t row_index = static_cast<std::size_t>(-1);
        std::vector<tk::Rect> chips; // one per Reaction in row
        std::vector<tk::Rect>
            receipt_discs;     // one per visible read-receipt disc
        tk::Rect add_button{}; // 0-area when not painted
        bool add_visible = false;
        tk::Rect reply_button{};  // 0-area when not painted
        tk::Rect edit_button{};   // 0-area when not painted
        tk::Rect delete_button{}; // 0-area when not painted
        tk::Rect retry_button{};  // 0-area when not painted
        tk::Rect abort_button{};  // 0-area when not painted
        tk::Rect row_bounds{};
    };

    enum class HoverTarget
    {
        None,
        Chip,
        AddButton,
        Receipt,
        RetryButton,
        AbortButton
    };

    // Test introspection: the chip geometry recorded by the most
    // recent paint of the hovered row, and the resolved hover target.
    const RowChipGeom& hovered_row_geom() const
    {
        return hovered_row_geom_;
    }
    HoverTarget hover_target() const
    {
        return hover_target_;
    }
    int hover_chip_index() const
    {
        return hover_chip_idx_;
    }

    // Scroll-to-bottom pill — visible only when the user has scrolled
    // away from the live tail. World-coord rect, recomputed each paint.
    bool pill_visible() const
    {
        return pill_visible_;
    }
    tk::Rect pill_bounds() const
    {
        return pill_rect_;
    }

    // Scroll to the row matching `event_id`. Returns true if found and
    // scrolled; false if event_id is not currently loaded.
    bool scroll_to_event_id(const std::string& event_id);

    // Show/hide historical mode. The pill stays visible regardless of scroll
    // position, and clicking it fires on_return_to_live instead of
    // scroll_to_bottom(). Calls invalidate_data(); the caller must also schedule
    // a surface repaint for the change to take effect immediately.
    void set_historical_mode(bool historical);

    // Fired when the user clicks the scroll-to-bottom pill while in
    // historical mode.
    std::function<void()> on_return_to_live;

private:
    class Adapter;
    friend class Adapter;

    std::vector<MessageRowData> messages_;
    // True while waiting for the SDK to relocate the read marker after a
    // new real message was appended. The adapter returns height 0 and skips
    // painting the ReadMarker row; cleared when update_message receives a
    // ReadMarker row (SDK confirmed the new position).
    bool suppress_read_marker_ = false;

    // Room-switch display gate. Armed by set_messages(.., room_switch=true);
    // evaluated on the first paint (heights are guaranteed measured there);
    // while `pending` is non-empty the message rows are not painted (only
    // the list background) so the room appears once, already correct.
    // `epoch` supersedes a stale gate on a rapid A→B→C re-switch.
    struct RoomSwitchGate
    {
        std::uint64_t epoch = 0;
        bool evaluated = false;                  // visible band scanned
        std::unordered_set<std::string> pending; // unmet media/url keys
        bool focused = false;                    // jump-to-event mode
        std::string focus_event_id;
    };
    std::optional<RoomSwitchGate> room_switch_gate_;
    std::uint64_t room_switch_epoch_ = 0;
    std::function<void(int, std::function<void()>)> post_delayed_;

    // Has every height-affecting dependency of row `m` already resolved?
    bool gate_dep_satisfied_(const MessageRowData& m) const;
    // First-paint scan: fill the gate's `pending` set from the visible band.
    void collect_gate_deps_();
    // Clear the gate + re-pin scroll (bottom, or focus row) so the first
    // visible frame is already correct.
    void reveal_room_switch_gate_();
    // Hook called from notify_image_ready / notify_url_preview_ready: drop
    // a resolved key and repaint to reveal once the pending set empties.
    void on_gate_notify_(const std::string& key);
    // True while a room-switch gate still holds the list invisible. Pointer
    // and wheel input is swallowed in that window so the user can't click /
    // scroll rows that aren't painted yet.
    bool gate_blocks_input_() const
    {
        return room_switch_gate_.has_value();
    }

    // Non-empty → render a synthetic trailing typing row (see Adapter).
    std::string typing_text_;
    ImageProvider avatar_provider_;
    ImageProvider image_provider_;
    ShortcodeProvider shortcode_provider_;
    std::unique_ptr<Adapter> adapter_;

    // Per-frame chip geometry for the hovered row. Mutable so paint_row
    // can write into it from a const-ish paint pass.
    mutable RowChipGeom hovered_row_geom_;

    // Per-frame inline-sticker geometry, keyed by event_id. Populated by
    // `Adapter::paint_row` when it paints a Kind::Sticker row; consumed by
    // `sticker_hit_at` on demand. Cleared at the top of each paint pass so
    // entries scrolled offscreen don't linger.
    mutable std::unordered_map<std::string, StickerHit> sticker_geom_;

    // Which chip (if any) the pointer is currently over within the
    // hovered row. -1 means "no chip"; HoverTarget chooses between an
    // existing reaction chip and the trailing add-button.
    HoverTarget hover_target_ = HoverTarget::None;
    int hover_chip_idx_ = -1;

    // Press-state — remember which chip the user pressed so we only
    // fire the callback on a clean down-up on the same chip.
    HoverTarget press_target_ = HoverTarget::None;
    int press_chip_idx_ = -1;
    std::string press_event_id_;

    // Sender avatar / name press state.
    bool press_sender_ = false;
    std::string press_sender_user_id_;
    std::string press_sender_display_name_;
    std::string press_sender_avatar_url_;

    // Reply button press state. Tracks a clean down-up on the hover reply btn.
    bool press_reply_btn_ = false;
    std::string press_reply_event_id_;

    // Edit button press state (own text messages only).
    bool press_edit_btn_ = false;
    std::string press_edit_event_id_;

    // Delete button press state (own non-redacted messages).
    bool press_delete_btn_ = false;
    std::string press_delete_event_id_;

    // Retry / abort pending-send button press state (own failed messages).
    bool press_retry_btn_ = false;
    bool press_abort_btn_ = false;
    std::string press_pending_txn_id_;

    // Image / sticker click-to-view press state.
    mutable std::unordered_map<std::string, ImageHit> image_geom_;
    bool press_image_ = false;
    std::string press_image_eid_;

    // Video thumbnail click-to-view press state (parallel to image).
    mutable std::unordered_map<std::string, VideoHit> video_geom_;
    bool press_video_ = false;
    std::string press_video_eid_;

    // File card click-to-download press state.
    mutable std::unordered_map<std::string, FileHit> file_geom_;
    bool press_file_ = false;
    std::string press_file_eid_;

    // Quote-block press state. Fires on_scroll_to_original (or internal
    // scroll_to_index) when the user clicks the quote block of a reply.
    bool press_quote_ = false;
    std::string press_quote_event_id_;

    // Per-frame quote-block rects in world coordinates, keyed by event_id.
    // Cleared at the top of each paint pass (same pattern as voice_card_geom_).
    mutable std::unordered_map<std::string, tk::Rect> quote_block_geom_;
    mutable std::unordered_map<std::string, tk::Rect> map_rect_geom_;

    // URL preview card provider + press state.
    PreviewProvider preview_provider_;
    struct PreviewCardHit
    {
        std::string url;
        tk::Rect rect;
    };
    mutable std::unordered_map<std::string, PreviewCardHit> preview_card_geom_;
    bool press_preview_ = false;
    std::string press_preview_url_;

    // Inline hyperlink hit-testing. Layouts built during paint_body_text
    // are moved here so link_at() can be called on pointer events without
    // a repaint. Cleared whenever messages_ is replaced.
    struct LinkLayout
    {
        std::unique_ptr<tk::TextLayout> layout;
        tk::Point origin{}; // world-space draw origin
        std::string plain;  // concatenated span text for clipboard
    };
    mutable std::unordered_map<std::string, LinkLayout> link_layout_cache_;
    std::string press_link_url_;
    std::string hover_link_url_;

    // MSC2010 spoiler reveal state.
    std::unordered_set<std::string> revealed_spoilers_;
    bool press_spoiler_ = false;
    std::string press_spoiler_eid_;

    // Scroll-to-bottom pill. Geometry is recomputed in paint() (after
    // ListView::paint has updated scroll state), so the rect + visible
    // flag are mutable — same trick as hovered_row_geom_ above.
    mutable tk::Rect pill_rect_{}; // world coords
    mutable bool pill_visible_ = false;
    bool press_pill_ = false;
    bool historical_mode_ = false;

    bool should_show_pill() const;

    // Read receipt viewport tracking. Fires on_receipt_needed at most once
    // per distinct event_id; last_receipt_event_id_ guards against re-firing
    // on every paint when the scroll position is unchanged.
    mutable std::string last_receipt_event_id_;
    std::string newest_visible_real_event_id() const;
    void maybe_notify_receipt_() const;

    // Voice playback. The view owns a single AudioPlayer — at most one
    // voice clip plays at a time. The host hands ownership in via
    // `set_audio_player` after construction; until then click-to-play is
    // a no-op. `voice_card_geom_` is rebuilt every paint pass (world
    // coords, keyed by event_id) so `on_pointer_down` can hit-test the
    // play button, waveform strip, and speed pill without touching the
    // painter again.
    struct VoiceCardGeom
    {
        std::string event_id;
        tk::Rect play_button{};    // play/pause hit rect
        tk::Rect waveform_strip{}; // scrub hit rect
        tk::Rect speed_pill{};     // playback-rate cycle hit rect
        tk::Rect card_bounds{};
    };
    mutable std::unordered_map<std::string, VoiceCardGeom> voice_card_geom_;

    struct AudioCardGeom
    {
        std::string event_id;
        tk::Rect play_button{};    // play/pause hit rect
        tk::Rect progress_track{}; // linear scrub hit rect
        tk::Rect card_bounds{};
    };
    mutable std::unordered_map<std::string, AudioCardGeom> audio_card_geom_;

    std::unique_ptr<tk::AudioPlayer> audio_player_;
    VoiceBytesProvider voice_bytes_provider_;
    std::function<void()> request_repaint_;

    // Liveness sentinel. Async media-fetch / player callbacks capture a
    // weak_ptr to this and bail if it has expired — the view is destroyed
    // on every room switch while a fetch may still be in flight, so a raw
    // `this` capture would be a use-after-free.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // The event_id of the currently-loaded clip in `audio_player_`. Empty
    // when nothing is loaded.
    std::string playing_event_id_;
    // Mirror of `audio_player_->position_ms()` — refreshed from the
    // AudioPlayer's on_progress callback so paint doesn't have to call
    // back into the player.
    std::uint64_t playing_position_ms_ = 0;
    bool playing_is_active_ = false;
    // Set to true once on_audio_progress() first observes is_playing()==true
    // for the current clip.  Guards against clearing playing_event_id_ during
    // the async-load window when Qt reports is_playing()==false at position 0.
    bool playing_ever_active_ = false;
    // Global playback rate. Cycles 1.0 → 1.5 → 2.0 → 1.0 via the per-row
    // speed pill. Applied to every play()/resume()/seek().
    float playback_rate_ = 1.0f;

    enum class VoicePressKind
    {
        None,
        PlayButton,
        Waveform,
        SpeedPill
    };
    VoicePressKind press_voice_kind_ = VoicePressKind::None;
    std::string press_voice_event_id_;

    void handle_voice_play_click(const MessageRowData& row);
    void handle_voice_scrub_at(const MessageRowData& row, float local_x);
    void handle_voice_speed_click();
    void on_audio_progress();

    enum class AudioPressKind
    {
        None,
        PlayButton,
        ProgressTrack,
    };
    AudioPressKind press_audio_kind_ = AudioPressKind::None;
    std::string press_audio_event_id_;

    void handle_audio_play_click(const MessageRowData& row);
    void handle_audio_scrub_at(const MessageRowData& row, float world_x);

    // Inline video playback — at most kMaxInlinePlayers active simultaneously.
    static constexpr int kMaxInlinePlayers = 10;
    struct InlinePlayer
    {
        std::unique_ptr<tk::VideoPlayer> player;
    };
    std::unordered_map<std::string, InlinePlayer> inline_players_;
    VideoPlayerFactory video_player_factory_;
    VideoFetchProvider video_fetch_provider_;
    void start_inline_video(const MessageRowData& m);

    // Pan state for Kind::Location rows.
    static constexpr std::size_t kNoMapRow =
        std::numeric_limits<std::size_t>::max();
    std::size_t map_active_row_ = kNoMapRow;
    tk::Point map_drag_start_pt_{};
    tk::Point map_drag_start_vp_px_{}; // world-pixel viewport at drag start
    float map_zoom_accum_ = 0.0f;      // fractional wheel accumulator
    bool map_tooltip_showing_ = false;

    // Drag-select state.
    struct Selection
    {
        std::string anchor_event_id;
        int         anchor_byte = 0; // UTF-8 byte offset at pointer_down
        std::string head_event_id;
        int         head_byte   = 0; // UTF-8 byte offset during drag
    };
    std::optional<Selection> sel_;
    bool sel_is_dragging_ = false; // true once head has moved from anchor
    bool press_sel_ = false;       // this pointer-down started a selection

    struct OrderedSel { int lo_idx = 0, lo_byte = 0, hi_idx = 0, hi_byte = 0; };
    int message_index_of(const std::string& event_id) const;
    std::optional<OrderedSel> selection_ordered() const;

    // Last pointer-move position in widget-local coordinates. Used to
    // re-evaluate chip_hit_at after hovered_row_geom_ is repopulated by paint.
    tk::Point last_pointer_local_{};

    // Multi-click tracking: double = word, triple = line.
    int       click_count_      = 0;
    tk::Point last_down_pt_     = {};
    int64_t   last_down_time_ms_ = 0; // ms from steady_clock
};

} // namespace tesseract::views
