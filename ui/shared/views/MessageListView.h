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
    enum class Kind { Text, Image, Sticker, File, Voice, Redacted, Unhandled };

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

    std::vector<tesseract::Reaction> reactions;
    /// Users (excluding the current user) whose latest read receipt landed
    /// on this event. The view renders up to a small cap of mini-avatars
    /// at the bottom-right of the row; the rest fall into a "+N" overflow.
    std::vector<tesseract::ReadReceipt> read_receipts;
};

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
        std::vector<tk::Rect> chips;       // one per Reaction in row
        tk::Rect          add_button{};    // 0-area when not painted
        bool              add_visible = false;
        tk::Rect          row_bounds{};
    };

    enum class HoverTarget { None, Chip, AddButton };

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
};

} // namespace tesseract::views
