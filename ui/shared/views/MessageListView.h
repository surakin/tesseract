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
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/video.h"
#include "views/LinkLayoutCache.h"
#include "views/LocationMapPanner.h"
#include "views/MembershipGroupExpander.h"
#include "views/ReadReceiptTracker.h"
#include "views/RoomSwitchGateKeeper.h"
#include "views/SpoilerRevealer.h"
#include "views/TimelineMediaController.h"
#include "views/UrlPreviewCardDisplay.h"
#include "views/TimelineVideoPlaylist.h"
#include "views/map_tiles.h"

#include <tesseract/types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{

struct ScrollPillWidget;

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
        Utd,
        Notice,
        Emote,
        Unhandled,
        DaySeparator,
        ReadMarker,
        TimelineStart,
        PinnedEvent,        // m.room.pinned_events state-event row
        CallNotification,  // org.matrix.msc4075.rtc.notification
        Location,
        Membership,         // m.room.member state-event row
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

    // Whole-room pinning: the ImageRef this row currently displays (image /
    // sticker / video-thumbnail / URL-preview image). Holding it keeps the
    // image un-evictable from the shell's image cache while the row stays
    // materialized; `owned_image_key` is the cache key it was acquired for.
    // Both are populated lazily and are NOT produced by make_row_data().
    tk::ImageRef owned_image;
    std::string owned_image_key;
    // Sender-avatar pin: prevents thumbnail cache eviction during idle periods.
    // Same lifecycle as owned_image — populated by try_acquire_image_(), NOT make_row_data().
    tk::ImageRef owned_avatar;
    std::string  owned_avatar_key;

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
    // Non-null when the replied-to message is an m.image event in the local cache.
    tesseract::MediaSourceRef in_reply_to_image_source;
    bool has_reply() const { return !in_reply_to_id.empty(); }
    bool has_reply_image() const
    {
        return has_reply() && bool(in_reply_to_image_source);
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

    // MSC3440 threads. Mirror of tesseract::Event's thread fields. Used by
    // MessageListView to filter in-thread replies out of the main list and
    // to render the "N replies" preview chip under thread-root rows.
    std::string thread_root_id;  // non-empty iff this row is an in-thread reply
    bool is_thread_root = false; // true when this row roots a thread
    std::uint64_t thread_reply_count = 0;
    std::string thread_latest_sender_name;
    std::string thread_latest_body;
    std::uint64_t thread_latest_ts = 0;

    // Membership change (Kind::Membership only). `membership_target_*`
    // identifies whose membership changed (the target), which may differ
    // from `sender`/`sender_name` (who performed the action, e.g. an admin
    // kicking/banning/inviting a different user).
    tesseract::MembershipAction membership_action = tesseract::MembershipAction::Joined;
    std::string membership_target_user_id;
    std::string membership_target_name;
    std::string membership_target_avatar_url; // mxc
};

// Convert a raw SDK Event into the flat MessageRowData the shared view
// consumes. `my_user_id` is used to set `is_own` on the returned row.
MessageRowData make_row_data(const tesseract::Event& ev,
                             const std::string& my_user_id);

// Membership-group boundary helpers, factored out as pure functions (over
// `msgs`/an index) so they're unit-testable without a live MessageListView.
// MessageListView::Adapter's row-height/paint/click logic delegates to
// these. A "group" is a maximal run of consecutive Kind::Membership rows
// sharing the same membership_action; any other kind (including a virtual
// day-separator row) or a different action starts a new group. No
// time-based splitting — arbitrarily distant same-action rows still group
// as long as nothing else is interleaved.
bool is_membership_group_start(const std::vector<MessageRowData>& msgs,
                               std::size_t index);
// Exclusive end index of the group starting at `start` (which must satisfy
// is_membership_group_start(msgs, start)).
std::size_t membership_group_end(const std::vector<MessageRowData>& msgs,
                                 std::size_t start);
// Walk backward from any Membership row to the start of its group.
std::size_t membership_group_start_of(const std::vector<MessageRowData>& msgs,
                                      std::size_t index);

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
    // Returns a pinning handle to the cached image for `mxc_or_url`, or null
    // when it is not (yet) decoded. Rows hold the handle so the image they
    // display is not evicted while the room is open.
    using ImageAcquirer =
        std::function<tk::ImageRef(const std::string& mxc_or_url)>;
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

    // MSC4278: predicate deciding whether a row's media preview is suppressed
    // (rendered as a click-to-load placeholder instead of the image). Set by
    // the shell from the media-preview config + per-message reveal state.
    // When unset, no media is ever hidden.
    using MediaHiddenPredicate =
        std::function<bool(const std::string& event_id, bool is_own)>;
    void set_media_hidden_predicate(MediaHiddenPredicate p)
    {
        media_hidden_ = std::move(p);
    }

    // Fired when the user clicks a suppressed-media placeholder to load it.
    // The shell records the reveal and kicks the media fetch.
    std::function<void(const std::string& event_id)> on_reveal_media;

    // Pinning handle source for whole-room image retention. When set, rows
    // acquire and hold the ImageRef for the image/sticker/video-thumbnail/
    // URL-preview they display, so it survives cache eviction while open.
    void set_image_acquirer(ImageAcquirer a);

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

    // Pause every inline autoplay video when the OS-level window becomes
    // hidden/minimized (suspended = true); resume only the ones whose row is
    // still within the current viewport when it becomes visible again
    // (suspended = false). Wired from ShellBase on a main-window visibility
    // edge — see ShellBase::update_video_playback_suspension_().
    void set_video_playback_suspended(bool suspended);

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

    // Fires when the user clicks an inline mention pill (a matrix.to user
    // link). The shell should open that user's profile panel rather than a
    // browser. `user_id` is the parsed Matrix id (e.g. "@alice:example.org").
    std::function<void(const std::string& user_id)> on_mention_clicked;

    // Resolves a mentioned user's avatar image by Matrix user id, for drawing
    // a small avatar inside the mention pill. Returns nullptr when unknown
    // (the pill then renders without an avatar). May trigger an async fetch;
    // the view repaints when bytes arrive.
    using MentionAvatarProvider =
        std::function<const tk::Image*(const std::string& user_id)>;
    void set_mention_avatar_provider(MentionAvatarProvider p)
    {
        mention_avatar_provider_ = std::move(p);
    }

    // Fires when the pointer enters or leaves an inline hyperlink. url is
    // non-empty while hovering, empty when the pointer leaves. Used by the
    // shell to switch the cursor to/from a pointing-hand cursor.
    std::function<void(const std::string& url)> on_link_hovered;

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

    // Overflow-menu affordance — fires when the user clicks the "⋯" more
    // button. `anchor` is the button rect in world coordinates. `can_delete`
    // is true for own non-redacted messages; `can_pin` when the room allows
    // pinning; `is_pinned` when this event is already pinned; `can_forward`
    // is true for any non-redacted, non-pending message. The host should
    // open a PopupMenu and call the appropriate SDK methods on selection.
    std::function<void(const std::string& event_id, tk::Rect anchor,
                       bool can_delete, bool can_pin, bool is_pinned,
                       bool can_forward)>
        on_more_requested;

    // Pin / Unpin affordance — kept for backward-compat wiring; now fired
    // indirectly via the overflow popup rather than a direct pill button.
    std::function<void(const std::string& event_id)> on_pin_requested;
    std::function<void(const std::string& event_id)> on_unpin_requested;

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

    // Fired with the media fetch tokens of the currently-visible rows whenever
    // that set changes (scroll, room enter, async data update). The host raises
    // the download priority of the still-pending fetches for these tokens so the
    // media the user is looking at downloads ahead of the off-screen backlog.
    std::function<void(const std::vector<std::string>&)> on_visible_range_changed;

    // Fired with the deduplicated avatar mxc URLs (sender + read-receipt users)
    // of the currently-visible rows whenever that set changes. The host calls
    // ensure_user_avatar_ for each URL so avatars are fetched lazily rather than
    // for the entire room history on room switch.
    std::function<void(const std::vector<std::string>&)> on_visible_avatars_changed;

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

    // Widget overrides
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // Own pointer-move/down/up so we can hit-test reaction chips before the
    // ListView base sees the event.
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    Widget* dispatch_pointer_move(tk::Point world, bool* dirty = nullptr) override;
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
        tk::Rect react_button{};  // 0-area when not painted
        tk::Rect reply_button{};  // 0-area when not painted
        tk::Rect thread_button{}; // 0-area when not painted
        tk::Rect edit_button{};   // 0-area when not painted
        tk::Rect more_button{};   // 0-area when not painted
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

    // Store `event_id` as a deferred scroll target. On each arrange() pass,
    // after row_offsets_ are rebuilt and anchor adjustment has run, the
    // pending scroll is applied. Cleared once the event is found and scrolled,
    // or when set_messages() resets the list.
    void set_pending_scroll_event_id(const std::string& event_id);

    // Suppress the expensive part of arrange() (the base ListView's
    // rebuild_heights/rebuild_dirty_, which can rebuild many rows' text
    // layouts) while this view is fully covered and invisible — e.g.
    // behind the room media gallery overlay. Mutations still update
    // messages_ and mark rows dirty normally while suppressed; that
    // deferred dirty state is simply picked up and processed in one pass
    // by the next real arrange() once suppression lifts, instead of once
    // per mutation while nobody can see the result.
    void set_relayout_suppressed(bool suppressed)
    {
        relayout_suppressed_ = suppressed;
    }

    // Show/hide historical mode. The pill stays visible regardless of scroll
    // position, and clicking it fires on_return_to_live instead of
    // scroll_to_bottom(). Calls invalidate_data(); the caller must also schedule
    // a surface repaint for the change to take effect immediately.
    void set_historical_mode(bool historical);

    /// Bulk-insert events at the front of the timeline (oldest-first in `rows`).
    void prepend_messages(std::vector<MessageRowData> rows);

    /// Bulk-insert events at the end of the timeline (oldest-first in `rows`).
    void append_messages(std::vector<MessageRowData> rows);

    // Show/hide the back-pagination spinner. Pass true when a back-paginate
    // request is in flight, false when it completes.
    void set_paginating(bool paginating);
    bool paginating() const { return paginating_; }

    // Enter the room-switch "loading" state: clear the previous room's rows at
    // once (so they never show under the new room's header) and hold a clean
    // loading view until the new room's populated snapshot arrives via
    // set_messages(..., room_switch=true), which cancels this state. A centered
    // spinner is drawn only if the load runs longer than kSwitchSpinnerDelayMs,
    // so fast / warm switches show nothing transient. Each call supersedes any
    // prior loading (epoch), neutralising an outstanding delayed-spinner timer.
    void begin_switch_loading();
    // Leave the room-switch loading state without a populated snapshot — used
    // when the subscription FAILS (no reset will ever arrive), so the list stops
    // showing the spinner and settles on an empty room instead of hanging.
    void end_switch_loading();
    // True while the room-switch loading state is active (rows held empty,
    // awaiting the new room's snapshot).
    bool is_switch_loading() const { return switch_loading_; }
    // Test seam: whether the delayed spinner is currently due to paint.
    bool switch_spinner_visible_for_test() const
    {
        return switch_loading_ && switch_spinner_due_;
    }

    void begin_nav_loading();
    void end_nav_loading();
    bool is_nav_loading() const { return nav_loading_; }
    bool nav_spinner_visible_for_test() const { return nav_loading_ && nav_spinner_due_; }

    // Fired when the user clicks the scroll-to-bottom pill while in
    // historical mode.
    std::function<void()> on_return_to_live;

    // Fired when the user clicks the "N replies" preview chip drawn under
    // a thread-root row. `thread_root_id` is the root event id (i.e. the
    // event_id of the row the chip belongs to).
    std::function<void(const std::string& thread_root_id)>
        on_thread_preview_clicked;

    // Paint a semi-transparent dark overlay over the list bounds. The
    // highlight outline (see below) is painted after this overlay so it
    // remains visible above the dim.
    void set_dimmed(bool dimmed);
    bool dimmed() const { return dimmed_; }

    void set_thread_button_visible(bool v) { thread_button_visible_ = v; }

    // When true, on_pointer_leave() is suppressed so the hover-action pill
    // stays visible while an associated popup is open over this widget.
    void set_hover_locked(bool v) { hover_locked_ = v; }

    // Paint a coloured 2px outline around the row whose event_id matches.
    // Pass the empty string to clear the highlight.
    void set_highlighted_event(const std::string& event_id);
    const std::string& highlighted_event() const
    {
        return highlighted_event_id_;
    }

    /// Replace the full set of search-match event ids.  Every loaded row
    /// whose event_id is in the set receives a subtle accent tint; the
    /// focused match (highlighted_event_id_) additionally gets a 2px
    /// accent outline.  Survives set_messages() calls — the shell clears
    /// when the search bar closes or the query changes.
    void set_search_matches(std::unordered_set<std::string> ids);
    void clear_search_matches();
    bool has_search_matches() const { return !search_match_ids_.empty(); }
    const std::unordered_set<std::string>& search_match_ids() const
    {
        return search_match_ids_;
    }

    /// Replace the cached set of pinned event ids for this room.
    /// MessageListView consults the set when drawing the hover-action row
    /// to pick Pin vs Unpin and the corresponding callback. Cleared on
    /// set_messages(.., room_switch=true).
    void set_pinned_event_ids(std::unordered_set<std::string> ids);
    const std::unordered_set<std::string>& pinned_event_ids() const
    {
        return pinned_event_ids_;
    }

    /// Toggle visibility of the Pin/Unpin button in the hover-action row.
    /// When false, the button is hidden entirely (not greyed out). Driven
    /// by ShellBase from Client::can_pin_in_room. Cleared on room switch.
    void set_can_pin(bool can_pin);
    bool can_pin() const { return can_pin_; }

    // Per-chip geometry recorded by the most recent paint pass. Each entry
    // is the world-coords hit rect of a thread-preview chip and the
    // root event id its click should fire. Public for tests.
    struct ChipHit
    {
        std::string root_event_id;
        tk::Rect rect{};
    };
    const std::vector<ChipHit>& chip_hit_rects_for_test() const
    {
        return chip_hit_rects_;
    }

    // Number of live body-text layouts retained by the shared layout cache.
    // Exposed so tests can assert the cache stays memory-bounded.
    std::size_t body_layout_cache_size_for_test() const
    {
        return link_cache_.size();
    }

private:
    class Adapter;
    friend class Adapter;

    // ListView hook: fired after an anchored relayout repositions the
    // viewport. Re-resolves the hover highlight + chip target from the last
    // pointer position so they track the same message across the height
    // change / index shift.
    void on_anchored_relayout_() override;

    // Clear the cached chip/receipt geometry for the hovered row so the next
    // paint_row rebuilds it. Shared by on_pointer_move (row changed) and
    // on_anchored_relayout_ (layout changed under a stationary pointer).
    void reset_hovered_row_geom_();

    // Drop all paint-recorded hit-test geometry after scroll_y_ changes. The
    // next paint rebuilds it at the new world coordinates.
    void clear_scroll_hit_geometry_();

    // Suppress the "start thread" hover button (set by ThreadView on its
    // embedded list — replies inside a thread don't need to open sub-threads).
    bool thread_button_visible_ = true;

    // Thread overlay state (see set_dimmed / set_highlighted_event).
    bool dimmed_ = false;
    std::string highlighted_event_id_;

    // Search-match state (see set_search_matches / clear_search_matches).
    // All loaded rows whose event_id is in this set receive a subtle accent
    // tint; the focused match (highlighted_event_id_) gets a 2px outline.
    // NOT cleared in set_messages() — the shell owns clearing.
    std::unordered_set<std::string> search_match_ids_;

    // Pinned-events state (see set_pinned_event_ids / set_can_pin). The
    // set determines whether the hover-action row shows Pin or Unpin for
    // a given row; `can_pin_` hides the button entirely when false.
    std::unordered_set<std::string> pinned_event_ids_;
    bool can_pin_ = false;
    // Per-paint chip hit rects (world coords). Cleared at the top of each
    // paint pass; populated when paint_row draws a thread-root chip.
    mutable std::vector<ChipHit> chip_hit_rects_;

    std::vector<MessageRowData> messages_;
    // True while waiting for the SDK to relocate the read marker after a
    // new real message was appended. The adapter returns height 0 and skips
    // painting the ReadMarker row; cleared when update_message receives a
    // ReadMarker row (SDK confirmed the new position).
    bool suppress_read_marker_ = false;

    // Back-pagination spinner state. Set while a back-paginate is in flight;
    // a rotating-dots indicator is drawn at the top of the viewport.
    bool paginating_ = false;
    std::chrono::steady_clock::time_point paginate_start_;
    void draw_pagination_spinner_(tk::PaintCtx& ctx);

    // Room-switch loading state (see begin_switch_loading). While active the
    // list is held empty (clean background) and, once switch_spinner_due_ flips
    // true at the delay, a centered spinner is drawn. switch_epoch_ neutralises
    // a superseded delayed-spinner timer (rapid re-switch / arriving snapshot).
    //
    // The delay must sit ABOVE a normal cold-room load (first visit: the SDK
    // builds the room's timeline from scratch, ~hundreds of ms) so that path
    // shows only the clean background, never a one-frame spinner flicker. Warm
    // re-visits (reused timeline) return well under this and also show nothing.
    // Only a genuinely slow load — uncached history, slow/offline network —
    // outlasts it and surfaces the spinner, which is exactly when it's wanted.
    static constexpr int kSwitchSpinnerDelayMs = 500;
    bool          switch_loading_     = false;
    bool          switch_spinner_due_ = false;
    std::uint64_t switch_epoch_       = 0;
    std::chrono::steady_clock::time_point switch_spinner_start_;
    void draw_switch_spinner_(tk::PaintCtx& ctx);
    // Historical-event nav loading. Active from the moment subscribe_room_at is
    // dispatched until set_messages() arrives with the rebuilt timeline.
    // Scroll/pointer input is blocked immediately; the scrim + spinner paint only
    // after kNavSpinnerDelayMs so fast resolutions show nothing transient.
    static constexpr int kNavSpinnerDelayMs = 400;
    bool          nav_loading_     = false;
    bool          nav_spinner_due_ = false;
    std::uint64_t nav_epoch_       = 0;
    std::chrono::steady_clock::time_point nav_spinner_start_;
    void draw_nav_spinner_(tk::PaintCtx& ctx);
    // Shared 8-dot rotating indicator (back-pagination + room-switch loading).
    void draw_spinner_dots_(tk::PaintCtx& ctx, float cx, float cy,
                            std::chrono::steady_clock::time_point start,
                            float radius, float dot_r);

    // Room-switch display gate. Armed by set_messages(.., room_switch=true);
    // evaluated on the first paint (heights are guaranteed measured there);
    // while blocking the message rows are not painted (only the list
    // background) so the room appears once, already correct. The state machine
    // (pending-deps set, per-Kind satisfied check, focused/jump-to-event
    // reveal, 400ms timeout) lives in RoomSwitchGateKeeper; this view drives
    // evaluate()/try_reveal() from paint() and forwards its public
    // begin_focused_gate / notify_*_ready to the keeper.
    RoomSwitchGateKeeper room_switch_gate_;

    // Whole-room pinning: derive `m`'s display key (image/sticker/video-thumb
    // source token, or URL-preview image_mxc) and, when `m` does not yet hold
    // a matching ImageRef, acquire and store one via `image_acquirer_`. A null
    // result (image not yet decoded) is fine — paint falls back to the
    // provider. No-op for kinds without a pinnable image or when no acquirer
    // is wired.
    void try_acquire_image_(MessageRowData& m);
    // Display key used both for pinning and for the gate dependency check.
    std::string row_image_key_(const MessageRowData& m) const;
    // True while a room-switch gate still holds the list invisible. Pointer
    // and wheel input is swallowed in that window so the user can't click /
    // scroll rows that aren't painted yet.
    bool gate_blocks_input_() const { return room_switch_gate_.active(); }

    // Non-empty → render a synthetic trailing typing row (see Adapter).
    std::string typing_text_;
    ImageProvider avatar_provider_;
    ImageProvider image_provider_;
    ImageAcquirer image_acquirer_;
    MediaHiddenPredicate media_hidden_;

    // True when row `m` is media whose preview is currently suppressed
    // (media_hidden_ set and returns true for it). Consulted by the inline
    // media / video paint paths and by the click handler to choose reveal
    // over open-viewer.
    bool media_is_hidden_(const MessageRowData& m) const;
    // Same check from a click handler that only has the event id: resolves the
    // row's `is_own` from the message list before consulting the predicate.
    bool media_is_hidden_by_eid_(const std::string& event_id) const;
    MentionAvatarProvider mention_avatar_provider_;
    ShortcodeProvider shortcode_provider_;
    std::unique_ptr<Adapter> adapter_;
    std::string pending_scroll_event_id_;
    // See set_relayout_suppressed().
    bool relayout_suppressed_ = false;

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

    enum class ActionTooltip { None, React, Reply, Thread, Edit, More };
    ActionTooltip action_tooltip_ = ActionTooltip::None;

    // True while an inline custom-emoji shortcode tooltip (hovering an
    // MSC2545 <img data-mx-emoticon> span in a message body) is showing.
    bool hover_emoji_tooltip_ = false;

    // Cached from paint() so on_pointer_move/on_pointer_leave (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;

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

    // React button press state (action-pill cell that opens the emoji picker).
    bool press_react_btn_ = false;
    std::string press_react_event_id_;

    // Reply button press state. Tracks a clean down-up on the hover reply btn.
    bool press_reply_btn_ = false;
    std::string press_reply_event_id_;

    // Edit button press state (own text messages only).
    bool press_edit_btn_ = false;
    std::string press_edit_event_id_;

    // More (⋯) button press state. Context captured at press time so the
    // popup can be opened with the correct items on release.
    bool press_more_btn_         = false;
    std::string press_more_event_id_;
    bool press_more_can_delete_  = false;
    bool press_more_can_pin_     = false;
    bool press_more_is_pinned_   = false;
    bool press_more_can_forward_ = false;

    bool hover_locked_ = false;

    // Thread button press state.
    bool press_thread_btn_ = false;
    std::string press_thread_event_id_;

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

    // URL preview cards: provider, per-paint card geometry, and the card paint
    // live in `previews_`. The view forwards set_preview_provider() here and
    // keeps only the pointer-press FSM (press_preview_ / press_preview_url_)
    // and the notify→invalidate→scroll-anchor glue (notify_url_preview_ready).
    UrlPreviewCardDisplay previews_;
    bool press_preview_ = false;
    std::string press_preview_url_;

    // Shared per-message body-text cache, keyed by event_id. A single shaped
    // layout is built once (by measure_body_text or paint_body_text, whichever
    // runs first) and reused for the row's height, its painting, and inline
    // hyperlink/selection hit-testing — so the body is not re-shaped on every
    // repaint. `keyed` entries carry a validity key (width / theme / spoiler /
    // content hash); a mismatch rebuilds. The cache is memory-bounded by LRU
    // (see LinkLayoutCache) and cleared when messages_ is replaced. Emote rows
    // write here too (keyed=false) purely for hit-testing. The LinkLayout type,
    // the LRU clock, the validity-key check and eviction all live in the
    // collaborator; the span-build pipeline stays on this view and is injected
    // via the get_or_build builder callback.
    mutable LinkLayoutCache link_cache_;
    std::string press_link_url_;
    std::string hover_link_url_;

    // MSC2010 spoiler reveal state. The revealed set lives in SpoilerRevealer;
    // the press-FSM fields stay here (part of the pointer state machine).
    SpoilerRevealer spoilers_;
    bool press_spoiler_ = false;
    std::string press_spoiler_eid_;

    // Membership-group (join/leave/etc.) expand/collapse state. The expanded
    // set lives in MembershipGroupExpander, keyed by the event_id of the
    // first row in the collapsed run; the press-FSM fields stay here.
    MembershipGroupExpander membership_groups_;
    bool press_membership_group_ = false;
    std::string press_membership_group_key_;

    // Scroll-to-bottom pill. Geometry is recomputed in paint() (after
    // ListView::paint has updated scroll state), so the rect + visible
    // flag are mutable — same trick as hovered_row_geom_ above.
    mutable tk::Rect pill_rect_{}; // world coords
    mutable bool pill_visible_ = false;
    ScrollPillWidget* scroll_pill_ = nullptr; // borrowed from child
    bool historical_mode_ = false;

    bool should_show_pill() const;

    // Read receipt viewport tracking. Fires on_receipt_needed at most once
    // per distinct event_id; the ReadReceiptTracker guards against re-firing
    // on every paint when the scroll position is unchanged. The
    // newest-visible-id computation stays here (it needs visible_range() +
    // messages_); the tracker owns only the de-dup state.
    mutable ReadReceiptTracker receipt_tracker_;
    std::string newest_visible_real_event_id() const;
    void maybe_notify_receipt_() const;

    // Visible-media prioritization. Collects the media fetch tokens (the same
    // key the image provider looks up) for the currently-visible rows and fires
    // on_visible_range_changed when that set changes, so the host can move those
    // rows' downloads to the front of the queue. De-duped against the last
    // notified set so an unchanged scroll position does not re-fire each paint.
    std::vector<std::string> collect_visible_media_keys_() const;
    // Collects deduplicated avatar mxc URLs (sender + read-receipt users) for
    // the currently-visible rows; fired via on_visible_avatars_changed.
    std::vector<std::string> collect_visible_avatar_urls_() const;
    void maybe_notify_visible_range_() const;
    void clear_hit_geometry_();
    // True when `event_id` maps to a row currently within the visible range.
    bool is_event_visible_(const std::string& event_id) const;
    mutable std::vector<std::string> last_visible_media_keys_;
    mutable std::vector<std::string> last_visible_avatar_urls_;

    // Voice + audio message playback. The view owns a single AudioPlayer
    // (at most one clip plays at a time); ownership, the byte-cache provider,
    // the play/pause/scrub/speed state, the per-paint hit-test geometry, and
    // the card paint all live in `media_`. The host hands ownership in via
    // `set_audio_player` after construction; until then click-to-play is a
    // no-op. The press FSM below stays on the view because it is part of the
    // pointer-down/up/drag pipeline; it only names the controller's geometry.
    TimelineMediaController media_;

    // Auto-advance lookup handed to `media_` (see set_next_voice_lookup):
    // scans messages_ forward from the just-finished voice row for the next
    // Kind::Voice row from the same sender.
    const MessageRowData*
    find_next_voice_from_same_sender_(const std::string& finished_event_id) const;

    // View-wide repaint requester. Wired by `set_repaint_requester` and used
    // throughout the view (gate reveal, selection drag, async hops, …); also
    // handed to `media_` so the playback subsystem can drive its own repaints.
    std::function<void()> request_repaint_;

    // Delayed-callback scheduler. Wired by `set_post_delayed`; the room-switch
    // gate uses it to arm its 400ms timeout fallback.
    std::function<void(int, std::function<void()>)> post_delayed_;

    // Liveness sentinel. Async media-fetch / player callbacks capture a
    // weak_ptr to this and bail if it has expired — the view is destroyed
    // on every room switch while a fetch may still be in flight, so a raw
    // `this` capture would be a use-after-free.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    enum class VoicePressKind
    {
        None,
        PlayButton,
        Waveform,
        SpeedPill
    };
    VoicePressKind press_voice_kind_ = VoicePressKind::None;
    std::string press_voice_event_id_;

    enum class AudioPressKind
    {
        None,
        PlayButton,
        ProgressTrack,
    };
    AudioPressKind press_audio_kind_ = AudioPressKind::None;
    std::string press_audio_event_id_;

    // Inline auto-play video subsystem (player pool, async byte fetch with its
    // own liveness sentinel, kMaxInlinePlayers eviction, live-frame draw). The
    // fullscreen video click/hit-test (video_geom_ / on_video_clicked /
    // video_hit_at) is a separate concern and stays in this view.
    TimelineVideoPlaylist video_playlist_;
    // Build the playlist source descriptor + visibility guard for one row,
    // then ensure_playing it. Mirrors the historical start_inline_video gate
    // (skips media_is_hidden_ rows). No-op when the row has no source.
    void start_inline_video(const MessageRowData& m);

    // Interactive pan/zoom/tooltip + hit-test geometry for Kind::Location
    // rows. Owns the pan FSM state and the per-paint map rect geometry; the
    // dispatch handlers below resolve the active row from messages_ and hand
    // the row's MapViewport to the panner's math.
    LocationMapPanner map_panner_;

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
