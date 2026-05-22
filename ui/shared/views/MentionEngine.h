#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tesseract/types.h> // tesseract::RoomMember

namespace tesseract::views
{

struct MentionMatch
{
    int start;          ///< UTF-8 byte offset of the leading '@'.
    int end;            ///< UTF-8 byte offset one past the last typed char.
    std::string prefix; ///< Characters after '@' (no '@'); may be empty.
};

struct MentionCandidate
{
    std::string user_id;      ///< Matrix user id; empty for an @room mention.
    std::string display_name; ///< Text shown for the mention ("@room" for room).
    std::string avatar_url;   ///< mxc:// or empty.
    bool is_room = false;     ///< The notify-everyone @room candidate.
};

/// Stateless helper — all inputs passed per call; no members.
class MentionEngine
{
public:
    /// Find an open `@mention` prefix under or immediately before the cursor.
    /// The `@` must begin the text or follow whitespace, so e-mail addresses
    /// (`foo@bar`) do not trigger. The prefix may be empty: a bare `@` opens
    /// the popup with the full member list. Returns nullopt otherwise.
    std::optional<MentionMatch> find_prefix(std::string_view text,
                                            int cursor_byte_pos) const;

    /// Filter `members` by display name / user-id localpart (case-insensitive),
    /// ranked exact < starts-with < substring. When `include_room` and the
    /// prefix matches "room" (or is empty), an `@room` candidate is placed
    /// first. Returns at most `max_results` candidates.
    std::vector<MentionCandidate>
    lookup(std::string_view prefix,
           const std::vector<tesseract::RoomMember>& members,
           int max_results = 8, bool include_room = true) const;
};

} // namespace tesseract::views
