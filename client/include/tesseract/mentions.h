#pragma once
#include <string>
#include <vector>

#include "tesseract/markdown.h"

namespace tesseract
{

/// One segment of a composer draft: either a run of typed text or an
/// intentional mention (a member, or the whole room via @room).
struct MentionSeg
{
    enum class Kind
    {
        Text,
        Mention
    };
    Kind kind = Kind::Text;
    std::string text;         ///< Kind::Text — verbatim typed text
    std::string user_id;      ///< Kind::Mention — Matrix user id (ignored if is_room)
    std::string display_name; ///< Kind::Mention — text shown for the mention
    bool is_room = false;     ///< Kind::Mention — an @room (notify-everyone) mention
};

/// Build a Matrix message `{body, formatted_body}` from composer segments.
///
/// Text segments are markdown-converted (via `markdown_to_html`); each mention
/// becomes a `matrix.to` anchor in `formatted_body` and its display name in the
/// plain `body`. `@room` mentions emit the `matrix.to/#/@room` sentinel anchor
/// that the SDK rewrites to plain `@room` while setting `m.mentions.room`.
///
/// When there are no mention segments this is equivalent to
/// `markdown_to_html(body)`. When there is at least one mention the returned
/// `formatted_body` is always non-empty (the SDK derives `m.mentions` from its
/// anchors), even for messages that contain no markdown.
MarkdownResult build_mention_message(const std::vector<MentionSeg>& segments);

} // namespace tesseract
