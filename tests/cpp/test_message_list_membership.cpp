#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"
#include "views/MembershipGroupExpander.h"
#include "tesseract/types.h"

using tesseract::MembershipAction;
using tesseract::views::is_membership_group_start;
using tesseract::views::MembershipGroupExpander;
using tesseract::views::membership_group_end;
using tesseract::views::membership_group_start_of;
using tesseract::views::MessageRowData;
using tesseract::views::make_row_data;

namespace
{

MessageRowData membership_row(const std::string& event_id,
                              MembershipAction action)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Membership;
    r.event_id = event_id;
    r.membership_action = action;
    return r;
}

MessageRowData text_row(const std::string& event_id)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = event_id;
    return r;
}

MessageRowData day_separator_row()
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::DaySeparator;
    return r;
}

} // namespace

TEST_CASE("consecutive same-action membership rows form one group",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Joined),
        membership_row("$c", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK_FALSE(is_membership_group_start(msgs, 1));
    CHECK_FALSE(is_membership_group_start(msgs, 2));
    CHECK(membership_group_end(msgs, 0) == 3);
    CHECK(membership_group_start_of(msgs, 2) == 0);
}

TEST_CASE("a change in membership action starts a new group",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Joined),
        membership_row("$c", MembershipAction::Left),
        membership_row("$d", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK_FALSE(is_membership_group_start(msgs, 1));
    CHECK(is_membership_group_start(msgs, 2)); // Left starts a new group
    CHECK(is_membership_group_start(msgs, 3)); // back to Joined: new group again

    CHECK(membership_group_end(msgs, 0) == 2); // {a, b}
    CHECK(membership_group_end(msgs, 2) == 3); // {c}
    CHECK(membership_group_end(msgs, 3) == 4); // {d}
}

TEST_CASE("a non-membership row breaks a group", "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        text_row("$msg"),
        membership_row("$b", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK(membership_group_end(msgs, 0) == 1); // {a} only — text row breaks it
    CHECK(is_membership_group_start(msgs, 2)); // $b starts its own new group
}

TEST_CASE("a day separator breaks a group even with the same action",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        day_separator_row(),
        membership_row("$b", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK(membership_group_end(msgs, 0) == 1);
    CHECK(is_membership_group_start(msgs, 2));
}

TEST_CASE("MembershipGroupExpander toggles independently per group key",
          "[message_list][membership]")
{
    MembershipGroupExpander e;
    CHECK_FALSE(e.is_expanded("$a"));
    e.toggle("$a");
    CHECK(e.is_expanded("$a"));
    CHECK_FALSE(e.is_expanded("$b"));
    e.toggle("$b");
    CHECK(e.is_expanded("$a"));
    CHECK(e.is_expanded("$b"));
    e.toggle("$a");
    CHECK_FALSE(e.is_expanded("$a"));
    CHECK(e.is_expanded("$b"));
    e.clear();
    CHECK_FALSE(e.is_expanded("$b"));
}

TEST_CASE("MembershipStateEvent produces Kind::Membership row with correct fields",
          "[message_list][membership]")
{
    tesseract::MembershipStateEvent ev;
    ev.event_id = "$mem:server";
    ev.sender = "@admin:server";
    ev.sender_name = "Admin";
    ev.timestamp = 1700000042000ULL;
    ev.action = MembershipAction::Kicked;
    ev.target_user_id = "@bob:server";
    ev.target_display_name = "Bob";
    ev.target_avatar_url = "mxc://server/bob";

    MessageRowData row = make_row_data(ev, /*my_user_id=*/"@me:server");

    REQUIRE(row.kind == MessageRowData::Kind::Membership);
    CHECK(row.event_id == "$mem:server");
    CHECK(row.sender_name == "Admin");
    CHECK(row.timestamp_ms == 1700000042000ULL);
    CHECK(row.membership_action == MembershipAction::Kicked);
    CHECK(row.membership_target_user_id == "@bob:server");
    CHECK(row.membership_target_name == "Bob");
    CHECK(row.membership_target_avatar_url == "mxc://server/bob");
}
