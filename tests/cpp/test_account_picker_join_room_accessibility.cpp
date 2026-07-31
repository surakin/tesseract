#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/AccountPicker.h"
#include "views/JoinRoomView.h"

#include <tesseract/types.h>

using tesseract::views::AccountEntry;
using tesseract::views::AccountPicker;
using tesseract::views::JoinRoomView;
using namespace tk;

// AccountPicker composes real UserInfo widget children (already mapped —
// see test_thread_view_user_info_accessibility.cpp), so this exercises the
// one new piece: active_indicator_ mapping to AccessState::selected.

TEST_CASE("AccountPicker's active-account row reports selected state; "
         "others don't",
         "[account_picker][accessibility]")
{
    AccountPicker picker;
    AccountEntry alice;
    alice.user_id = "@alice:example.org";
    alice.display_name = "Alice";
    alice.active = true;
    AccountEntry bob;
    bob.user_id = "@bob:example.org";
    bob.display_name = "Bob";
    bob.active = false;
    picker.set_entries({alice, bob});

    AccessNode tree = build_access_tree(&picker);
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].role == Role::Button);
    CHECK(tree.children[0].name == "Alice (@alice:example.org)");
    CHECK(tree.children[0].state.selected);
    CHECK_FALSE(tree.children[1].state.selected);
}

// JoinRoomView's preview card (room name/join-rule/member-count/topic) has
// no backing widget — access_role()/access_name() summarize it directly,
// only while a real preview is showing.

TEST_CASE("JoinRoomView stays Role::None (flattening through to its "
         "buttons) before any lookup has completed",
         "[join_room_view][accessibility]")
{
    auto view_owner = tk::create_root_widget<JoinRoomView>(nullptr);
    JoinRoomView& view = *view_owner;
    CHECK(view.access_role() == Role::None);
}

TEST_CASE("JoinRoomView summarizes the preview card as one Group node: "
         "name, join rule, member count, and topic",
         "[join_room_view][accessibility]")
{
    auto view_owner = tk::create_root_widget<JoinRoomView>(nullptr);
    JoinRoomView& view = *view_owner;

    tesseract::RoomSummary summary;
    summary.room_id = "!abc:example.org";
    summary.name = "General Discussion";
    summary.join_rule = "public";
    summary.num_joined_members = 42;
    summary.topic = "Talk about anything";
    view.set_preview(summary);

    CHECK(view.access_role() == Role::Group);
    CHECK(view.access_name() ==
         "General Discussion, Public, 42 members: Talk about anything");
}

TEST_CASE("JoinRoomView's preview summary falls back to the room id when "
         "no display name was resolved, and omits the topic when empty",
         "[join_room_view][accessibility]")
{
    auto view_owner = tk::create_root_widget<JoinRoomView>(nullptr);
    JoinRoomView& view = *view_owner;

    tesseract::RoomSummary summary;
    summary.room_id = "!xyz:example.org";
    summary.num_joined_members = 1;
    view.set_preview(summary);

    CHECK(view.access_name() == "!xyz:example.org, 1 member");
}
