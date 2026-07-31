#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/ShortcodePopup.h"

using tesseract::views::ShortcodePopup;
using tesseract::views::ShortcodeSuggestion;
using namespace tk;

// Exercises ListPopupBase's tk::WidgetRowAccessibility implementation
// through ShortcodePopup — a real consumer of a widget that paints its own
// rows directly (no tk::ListView underneath, unlike RoomListView/
// MessageListView/ThreadListView), mirroring the generic-mechanism +
// real-view test pattern used for those.

TEST_CASE("ShortcodePopup exposes its suggestions as accessible ListItems "
         "named after their shortcode (no colons, matching the plain-token "
         "convention settled on for the emoji/sticker grids)",
         "[shortcode_popup][accessibility]")
{
    ShortcodePopup popup;
    ShortcodeSuggestion smile;
    smile.shortcode = "smile";
    smile.glyph = "\xF0\x9F\x98\x8A";
    ShortcodeSuggestion wave;
    wave.shortcode = "wave";
    wave.glyph = "\xF0\x9F\x91\x8B";
    popup.set_suggestions({smile, wave});

    CHECK(popup.access_role() == Role::List);

    AccessNode tree = build_access_tree(&popup);
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].role == Role::ListItem);
    CHECK(tree.children[0].name == "smile");
    CHECK(tree.children[0].row_index == 0);
    CHECK(tree.children[0].row_set_size == 2);
    CHECK(tree.children[1].name == "wave");
}

TEST_CASE("ShortcodePopup with no suggestions produces no accessible "
         "children",
         "[shortcode_popup][accessibility]")
{
    ShortcodePopup popup;
    AccessNode tree = build_access_tree(&popup);
    CHECK(tree.children.empty());
}
