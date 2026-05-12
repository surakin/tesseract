#pragma once

// Shared message list. Renders a `std::vector<MessageRowData>` as
// avatar + sender name + body (text or inline media or file card) +
// reactions + timestamp. Variable row heights — sizes are recomputed
// when the data set or list width changes.
//
// The data model is deliberately flat — integration code unpacks the
// SDK's polymorphic Event hierarchy into MessageRowData on the UI
// thread so the shared view doesn't see virtual Events.

#include "tk/canvas.h"
#include "tk/list_view.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views {

struct MessageRowData {
    enum class Kind { Text, Image, Sticker, File, Redacted, Unhandled };

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

    std::vector<tesseract::Reaction> reactions;
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

    // Append a single message (typical live-update path) and scroll to
    // the bottom if the user was already pinned there.
    void append_message(MessageRowData msg);

    // Avatar bytes come from the host-side media cache. Returning null
    // falls back to an initials disc.
    void set_avatar_provider(ImageProvider p);

    // Inline image / sticker bytes come from the same kind of cache.
    void set_image_provider(ImageProvider p);

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

    // Widget overrides — own pointer-move/down/up so we can hit-test
    // reaction chips before the ListView base sees the event.
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up  (tk::Point local, bool inside_self) override;
    void on_pointer_move(tk::Point local) override;
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
};

} // namespace tesseract::views
