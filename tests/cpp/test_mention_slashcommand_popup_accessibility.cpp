#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandPopup.h"

using tesseract::views::MentionCandidate;
using tesseract::views::MentionPopup;
using tesseract::views::SlashCommandPopup;
using tesseract::views::SlashCommandSuggestion;
using namespace tk;

// Exercises MentionPopup's and SlashCommandPopup's tk::WidgetRowAccessibility
// implementations — the same ListPopupBase-level mechanism as
// ShortcodePopup, applied to the other two ListPopupBase subclasses.

TEST_CASE("MentionPopup names a regular candidate \"{display_name} "
         "({user_id})\" and the @room candidate with its hint text",
         "[mention_popup][accessibility]")
{
    MentionPopup popup;
    MentionCandidate alice;
    alice.user_id = "@alice:example.org";
    alice.display_name = "Alice";
    MentionCandidate room;
    room.is_room = true;
    popup.set_candidates({alice, room});

    AccessNode tree = build_access_tree(&popup);
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].role == Role::ListItem);
    CHECK(tree.children[0].name == "Alice (@alice:example.org)");
    CHECK(tree.children[1].name == "@room (Notify the whole room)");
}

TEST_CASE("SlashCommandPopup names a suggestion \"/name args_hint: "
         "description\", with a bot-disambiguation suffix when not builtin",
         "[slash_command_popup][accessibility]")
{
    SlashCommandPopup popup;
    SlashCommandSuggestion cmd;
    cmd.name = "ban";
    cmd.args_hint = "<user> [reason]";
    cmd.description = "Ban a user from the room";
    popup.set_suggestions({cmd});

    AccessNode tree = build_access_tree(&popup);
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].role == Role::ListItem);
    CHECK(tree.children[0].name ==
         "/ban <user> [reason]: Ban a user from the room");
}

TEST_CASE("SlashCommandPopup's hint row is StaticText, not a selectable "
         "ListItem, matching on_row_activated's own hint-mode no-op",
         "[slash_command_popup][accessibility]")
{
    SlashCommandPopup popup;
    popup.show_hint("Waiting for a reason...", /*is_error=*/false);

    AccessNode tree = build_access_tree(&popup);
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].role == Role::StaticText);
    CHECK(tree.children[0].name == "Waiting for a reason...");
}
