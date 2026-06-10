#include <catch2/catch_test_macros.hpp>

#include "views/roomlist_unread.h"

using tesseract::views::UnreadStyle;
using tesseract::views::unread_style_for;

// Args: (notification_count, highlight_count, unread_count, muted)

TEST_CASE("mention wins regardless of other state", "[roomlist][unread]")
{
    CHECK(unread_style_for(5, 2, 9, false) == UnreadStyle::Mention);
    // A highlight is an explicit signal even in a muted room.
    CHECK(unread_style_for(5, 2, 9, true) == UnreadStyle::Mention);
    // Highlight with no aggregate notification count still counts as a mention.
    CHECK(unread_style_for(0, 1, 0, false) == UnreadStyle::Mention);
}

TEST_CASE("notifying room shows a count pill", "[roomlist][unread]")
{
    CHECK(unread_style_for(3, 0, 3, false) == UnreadStyle::Count);
    // Notification count present but no highlight → Count, not Mention.
    CHECK(unread_style_for(1, 0, 0, false) == UnreadStyle::Count);
}

TEST_CASE("quiet unread (no notification, not muted) shows a dot",
          "[roomlist][unread]")
{
    CHECK(unread_style_for(0, 0, 4, false) == UnreadStyle::Dot);
    CHECK(unread_style_for(0, 0, 1, false) == UnreadStyle::Dot);
}

TEST_CASE("muted room with quiet unread shows nothing", "[roomlist][unread]")
{
    CHECK(unread_style_for(0, 0, 12, true) == UnreadStyle::None);
}

TEST_CASE("fully read room shows nothing", "[roomlist][unread]")
{
    CHECK(unread_style_for(0, 0, 0, false) == UnreadStyle::None);
    CHECK(unread_style_for(0, 0, 0, true) == UnreadStyle::None);
}
