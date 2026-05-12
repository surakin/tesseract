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

    // Click hooks. on_message_clicked fires on row click. Reaction-chip
    // hit testing is deferred until per-row sub-widget routing lands.
    std::function<void(const std::string& event_id)> on_message_clicked;

private:
    class Adapter;

    std::vector<MessageRowData>   messages_;
    ImageProvider                  avatar_provider_;
    ImageProvider                  image_provider_;
    std::unique_ptr<Adapter>       adapter_;
};

} // namespace tesseract::views
