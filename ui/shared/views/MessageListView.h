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

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesseract::views {

struct MessageRowData {
    enum class Kind { Text, Image, Sticker, File, Voice, Video, Redacted, Unhandled };

    Kind        kind          = Kind::Text;
    std::string event_id;
    std::string sender;            // canonical Matrix ID
    std::string sender_name;
    std::string sender_avatar_url; // mxc
    std::string body;
    std::uint64_t timestamp_ms   = 0;
    bool          is_own         = false;

    // Image / Sticker
    std::string media_url;         // mxc
    int         media_w           = 0;
    int         media_h           = 0;
    // MSC2530 caption — non-empty for `m.image` events whose sender
    // supplied a distinct `filename`, in which case `body` is a user
    // caption to render beneath the image.
    bool        has_filename_caption = false;

    // File card
    std::string file_name;
    std::uint64_t file_size = 0;

    // Voice (MSC3245). `audio_source` is the JSON-serialised MediaSource
    // passed straight to `Client::fetch_source_bytes`. `waveform` is the
    // MSC1767 amplitude list (each 0..=1024); when empty the view paints
    // flat placeholder bars.
    std::string                audio_source;
    std::string                audio_mime;
    std::uint64_t              duration_ms = 0;
    std::vector<std::uint16_t> waveform;

    // Video (m.video). `media_url` carries the video MediaSource JSON for
    // `fetch_source_bytes`; `media_w`/`media_h`/`duration_ms` are reused
    // from the shared slots above. `video_thumb_url` is the thumbnail
    // MediaSource JSON (pass to `image_provider_`); empty when the server
    // omits a thumbnail and the client-side first-frame generator fills it
    // in with the key `"thumb::" + event_id`.
    std::string video_thumb_url;
    std::string video_mime;            // e.g. "video/mp4"; empty when absent
    // fi.mau.* hints — each false by default.
    bool video_autoplay      = false;
    bool video_loop          = false;
    bool video_no_audio      = false;
    bool video_hide_controls = false;
    bool video_gif           = false;

    std::vector<tesseract::Reaction> reactions;
    /// Users (excluding the current user) whose latest read receipt landed
    /// on this event. The view renders up to a small cap of mini-avatars
    /// at the bottom-right of the row; the rest fall into a "+N" overflow.
    std::vector<tesseract::ReadReceipt> read_receipts;

    // Reply reference (m.in_reply_to). All three are empty when not a reply.
    std::string in_reply_to_id;
    std::string in_reply_to_sender_name;
    std::string in_reply_to_body;
    bool has_reply() const { return !in_reply_to_id.empty(); }

    // Set when the message has been superseded by an m.replace edit.
    bool is_edited = false;
};

// Convert a raw SDK Event into the flat MessageRowData the shared view
// consumes. `my_user_id` is used to set `is_own` on the returned row.
MessageRowData make_row_data(const tesseract::Event& ev,
                             const std::string&      my_user_id);

class MessageListView : public tk::ListView {
public:
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc_or_url)>;

    MessageListView();
    ~MessageListView() override;   // out-of-line — Adapter is opaque here

    // Bulk-replace the messages. Re-measures + repaints.
    void set_messages(std::vector<MessageRowData> msgs);
    const std::vector<MessageRowData>& messages() const { return messages_; }

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

    // Voice-message playback (MSC3245). Shells wire all three after
    // construction; the view stays inert (clicks become no-ops) when any
    // of them is missing — Win32 currently lacks an audio backend, so
    // `make_audio_player()` returns nullptr there and the player handle
    // is never set.
    using VoiceBytesProvider =
        std::function<std::vector<std::uint8_t>(const std::string& source_json)>;
    void set_audio_player        (std::unique_ptr<tk::AudioPlayer> player);
    void set_voice_bytes_provider(VoiceBytesProvider provider);
    void set_repaint_requester   (std::function<void()> request_repaint);

    // Inline video playback for fi.mau.autoplay / fi.mau.gif rows.
    // Both must be set for inline playback to activate; if either is missing
    // the view falls back to the static thumbnail (clicks still open the overlay).
    using VideoPlayerFactory = std::function<std::unique_ptr<tk::VideoPlayer>()>;
    using VideoFetchProvider = std::function<void(
        const std::string& source_json,
        std::function<void(std::vector<std::uint8_t>)> on_ready)>;
    void set_video_player_factory(VideoPlayerFactory f);
    void set_video_fetch_provider(VideoFetchProvider f);

    // Click hooks. on_message_clicked fires on row click.
    std::function<void(const std::string& event_id)> on_message_clicked;

    // Reaction-chip clicks. `key` is the emoji (or `:shortcode:`) the
    // user tapped. The host should call `Client::send_reaction` — the
    // Rust toggle semantics handle both add-and-remove in one call.
    std::function<void(const std::string& event_id,
                        const std::string& key)>      on_reaction_toggled;

    // Add-reaction button (the trailing "+" pseudo-chip that appears on
    // row hover). The host should open the emoji picker anchored near
    // `anchor`, then call `send_reaction` with the chosen glyph.
    std::function<void(const std::string& event_id,
                        tk::Rect anchor)>             on_add_reaction_requested;

    // Reply affordance — fires when the user clicks the "↩" reply button
    // that appears on hover. The host should call
    // `compose_bar_->set_reply_to(event_id, sender_name, body_preview)`.
    std::function<void(const std::string& event_id,
                        const std::string& sender_name,
                        const std::string& body_preview)> on_reply_requested;

    // Fires when the user clicks the quote block of a reply to scroll to
    // the original message. If the event is currently loaded the view scrolls
    // to it internally; this callback fires only when the original is not found
    // in the current message list (host should back-paginate + scroll).
    std::function<void(const std::string& original_event_id)> on_scroll_to_original;

    // Edit affordance — fires when the user clicks the "✏" edit button on a
    // hover row they own. Only fires for Kind::Text rows. The host should call
    // `compose_bar_->set_editing(event_id)`, then pre-fill the text area.
    std::function<void(const std::string& event_id,
                        const std::string& current_body)> on_edit_requested;

    // Sticker right-click hit. Native shells call this with the world-coord
    // pointer position from their secondary-click handler. Returns the
    // sticker event metadata when the click lands on a Kind::Sticker rect,
    // or `std::nullopt` otherwise. The shell decides whether to show a
    // context menu (e.g. skip when `Client::user_pack_has_sticker` is true).
    struct StickerHit {
        std::string event_id;
        std::string mxc_url;
        std::string body;
        tk::Rect    world_rect;
    };
    std::optional<StickerHit> sticker_hit_at(tk::Point world) const;

    // Image / sticker left-click hit — fires `on_image_clicked`. Recorded
    // per paint pass (same pattern as sticker_geom_). Exposed for tests.
    struct ImageHit {
        std::string event_id;
        std::string media_url;   // source_json — pass to image_provider_
        std::string body;        // MSC2530 caption, may be empty
        int         natural_w   = 0;
        int         natural_h   = 0;
        tk::Rect    world_rect;
    };
    std::optional<ImageHit> image_hit_at(tk::Point world) const;

    /// Fires when the user left-clicks an image or sticker thumbnail.
    std::function<void(const MessageListView::ImageHit&)> on_image_clicked;

    // Video thumbnail left-click hit — fires `on_video_clicked`. Parallel
    // to ImageHit / on_image_clicked. `source_json` is the video MediaSource
    // JSON to pass to `fetch_source_bytes` for the overlay.
    struct VideoHit {
        std::string event_id;
        std::string source_json;   // video MediaSource JSON for fetch_source_bytes
        std::string thumbnail_url; // thumbnail cache key (image_provider_)
        std::string mime_type;
        int         natural_w   = 0;
        int         natural_h   = 0;
        std::uint64_t duration_ms = 0;
        // fi.mau.* hints forwarded to VideoViewerOverlay::open().
        bool autoplay      = false;
        bool loop          = false;
        bool no_audio      = false;
        bool hide_controls = false;
        bool gif           = false;
        tk::Rect    world_rect;
    };
    std::optional<VideoHit> video_hit_at(tk::Point world) const;

    /// Fires when the user left-clicks a video thumbnail card.
    std::function<void(const MessageListView::VideoHit&)> on_video_clicked;

    // Widget overrides — own pointer-move/down/up so we can hit-test
    // reaction chips before the ListView base sees the event.
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up  (tk::Point local, bool inside_self) override;
    void on_pointer_move(tk::Point local) override;
    void on_pointer_drag(tk::Point local) override;
    void on_pointer_leave()                override;
    void paint          (tk::PaintCtx&)    override;

    // Per-chip geometry for the currently hovered row. Populated by
    // `Adapter::paint_row` during the row's paint pass (geometry is
    // recomputed there because chip width depends on text measurement);
    // consumed by `on_pointer_move` / `on_pointer_down` on subsequent
    // events. Stored in world coordinates. Public for tests.
    struct RowChipGeom {
        std::size_t       row_index = static_cast<std::size_t>(-1);
        std::vector<tk::Rect> chips;          // one per Reaction in row
        std::vector<tk::Rect> receipt_discs;  // one per visible read-receipt disc
        tk::Rect          add_button{};    // 0-area when not painted
        bool              add_visible = false;
        tk::Rect          reply_button{};  // 0-area when not painted
        tk::Rect          edit_button{};   // 0-area when not painted
        tk::Rect          row_bounds{};
    };

    enum class HoverTarget { None, Chip, AddButton, Receipt };

    // Test introspection: the chip geometry recorded by the most
    // recent paint of the hovered row, and the resolved hover target.
    const RowChipGeom& hovered_row_geom() const { return hovered_row_geom_; }
    HoverTarget        hover_target()     const { return hover_target_;   }
    int                hover_chip_index() const { return hover_chip_idx_; }

    // Scroll-to-bottom pill — visible only when the user has scrolled
    // away from the live tail. World-coord rect, recomputed each paint.
    bool      pill_visible() const { return pill_visible_; }
    tk::Rect  pill_bounds()  const { return pill_rect_;    }

private:
    class Adapter;
    friend class Adapter;

    std::vector<MessageRowData>   messages_;
    ImageProvider                  avatar_provider_;
    ImageProvider                  image_provider_;
    std::unique_ptr<Adapter>       adapter_;

    // Per-frame chip geometry for the hovered row. Mutable so paint_row
    // can write into it from a const-ish paint pass.
    mutable RowChipGeom            hovered_row_geom_;

    // Per-frame inline-sticker geometry, keyed by event_id. Populated by
    // `Adapter::paint_row` when it paints a Kind::Sticker row; consumed by
    // `sticker_hit_at` on demand. Cleared at the top of each paint pass so
    // entries scrolled offscreen don't linger.
    mutable std::unordered_map<std::string, StickerHit> sticker_geom_;

    // Which chip (if any) the pointer is currently over within the
    // hovered row. -1 means "no chip"; HoverTarget chooses between an
    // existing reaction chip and the trailing add-button.
    HoverTarget                    hover_target_  = HoverTarget::None;
    int                            hover_chip_idx_ = -1;

    // Press-state — remember which chip the user pressed so we only
    // fire the callback on a clean down-up on the same chip.
    HoverTarget                    press_target_  = HoverTarget::None;
    int                            press_chip_idx_ = -1;
    std::string                    press_event_id_;

    // Reply button press state. Tracks a clean down-up on the hover reply btn.
    bool                           press_reply_btn_      = false;
    std::string                    press_reply_event_id_;

    // Edit button press state (own text messages only).
    bool                           press_edit_btn_       = false;
    std::string                    press_edit_event_id_;

    // Image / sticker click-to-view press state.
    mutable std::unordered_map<std::string, ImageHit> image_geom_;
    bool                           press_image_          = false;
    std::string                    press_image_eid_;

    // Video thumbnail click-to-view press state (parallel to image).
    mutable std::unordered_map<std::string, VideoHit> video_geom_;
    bool                           press_video_          = false;
    std::string                    press_video_eid_;

    // Quote-block press state. Fires on_scroll_to_original (or internal
    // scroll_to_index) when the user clicks the quote block of a reply.
    bool                           press_quote_          = false;
    std::string                    press_quote_event_id_;

    // Per-frame quote-block rects in world coordinates, keyed by event_id.
    // Cleared at the top of each paint pass (same pattern as voice_card_geom_).
    mutable std::unordered_map<std::string, tk::Rect> quote_block_geom_;

    // Scroll-to-bottom pill. Geometry is recomputed in paint() (after
    // ListView::paint has updated scroll state), so the rect + visible
    // flag are mutable — same trick as hovered_row_geom_ above.
    mutable tk::Rect               pill_rect_{};      // world coords
    mutable bool                   pill_visible_ = false;
    bool                           press_pill_   = false;

    bool should_show_pill() const;

    // Voice playback. The view owns a single AudioPlayer — at most one
    // voice clip plays at a time. The host hands ownership in via
    // `set_audio_player` after construction; until then click-to-play is
    // a no-op. `voice_card_geom_` is rebuilt every paint pass (world
    // coords, keyed by event_id) so `on_pointer_down` can hit-test the
    // play button, waveform strip, and speed pill without touching the
    // painter again.
    struct VoiceCardGeom {
        std::string event_id;
        tk::Rect    play_button{};   // play/pause hit rect
        tk::Rect    waveform_strip{};// scrub hit rect
        tk::Rect    speed_pill{};    // playback-rate cycle hit rect
        tk::Rect    card_bounds{};
    };
    mutable std::unordered_map<std::string, VoiceCardGeom> voice_card_geom_;

    std::unique_ptr<tk::AudioPlayer> audio_player_;
    VoiceBytesProvider               voice_bytes_provider_;
    std::function<void()>            request_repaint_;

    // The event_id of the currently-loaded clip in `audio_player_`. Empty
    // when nothing is loaded.
    std::string                      playing_event_id_;
    // Mirror of `audio_player_->position_ms()` — refreshed from the
    // AudioPlayer's on_progress callback so paint doesn't have to call
    // back into the player.
    std::uint64_t                    playing_position_ms_ = 0;
    bool                             playing_is_active_   = false;
    // Global playback rate. Cycles 1.0 → 1.5 → 2.0 → 1.0 via the per-row
    // speed pill. Applied to every play()/resume()/seek().
    float                            playback_rate_       = 1.0f;

    enum class VoicePressKind { None, PlayButton, Waveform, SpeedPill };
    VoicePressKind                   press_voice_kind_    = VoicePressKind::None;
    std::string                      press_voice_event_id_;

    void handle_voice_play_click (const MessageRowData& row);
    void handle_voice_scrub_at   (const MessageRowData& row, float local_x);
    void handle_voice_speed_click();
    void on_audio_progress();

    // Inline video playback — at most kMaxInlinePlayers active simultaneously.
    static constexpr int kMaxInlinePlayers = 10;
    struct InlinePlayer { std::unique_ptr<tk::VideoPlayer> player; };
    std::unordered_map<std::string, InlinePlayer> inline_players_;
    VideoPlayerFactory  video_player_factory_;
    VideoFetchProvider  video_fetch_provider_;
    void start_inline_video(const MessageRowData& m);
};

} // namespace tesseract::views
