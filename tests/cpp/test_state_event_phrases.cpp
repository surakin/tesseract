#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"
#include "views/StateEventPhrases.h"

#include <string>
#include <vector>

using tesseract::views::MessageRowData;
using tesseract::views::known_state_label;
using tesseract::views::state_event_phrase;
using tesseract::views::state_event_run_phrase;

namespace
{
MessageRowData state_row(const std::string& type, const std::string& change,
                         std::vector<std::string> params, const std::string& sender = "Alice")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::StateEvent;
    r.sender = sender;
    r.sender_name = sender;
    r.state_type = type;
    r.state_change = change;
    r.state_params = std::move(params);
    return r;
}

bool contains(const std::string& s, const std::string& needle)
{
    return s.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("join_rules phrase names the sender and the new rule", "[state_event]")
{
    const auto p =
        state_event_phrase(state_row("m.room.join_rules", "changed", {"invite", "public"}));
    CHECK(contains(p, "Alice"));
    CHECK(contains(p, "invite"));
}

TEST_CASE("history_visibility set phrase reads as a sentence", "[state_event]")
{
    const auto p =
        state_event_phrase(state_row("m.room.history_visibility", "set", {"shared", ""}));
    CHECK(contains(p, "Alice"));
    CHECK_FALSE(contains(p, "updated the room state"));
}

TEST_CASE("power_levels phrase mentions the new value", "[state_event]")
{
    const auto p =
        state_event_phrase(state_row("m.room.power_levels", "changed", {"field|ban|50|75"}));
    CHECK(contains(p, "75"));
}

TEST_CASE("power_levels with no params still reads as a sentence", "[state_event]")
{
    const auto p = state_event_phrase(state_row("m.room.power_levels", "changed", {}));
    CHECK(contains(p, "Alice"));
    CHECK(contains(p, "permissions"));
}

TEST_CASE("server_acl phrase keeps the counted wording", "[state_event]")
{
    const auto p = state_event_phrase(state_row("m.room.server_acl", "changed", {"deny+3"}));
    CHECK(contains(p, "3 servers now denied"));
}

TEST_CASE("server_acl with no params reads as a sentence", "[state_event]")
{
    const auto p = state_event_phrase(state_row("m.room.server_acl", "changed", {}));
    CHECK(contains(p, "server ACL"));
}

TEST_CASE("canonical_alias with identical params reads as the alias", "[state_event]")
{
    const auto p = state_event_phrase(
        state_row("m.room.canonical_alias", "changed", {"#a:example.org", "#a:example.org"}));
    CHECK(contains(p, "#a:example.org"));
    CHECK_FALSE(contains(p, "from"));
}

TEST_CASE("unknown state type falls back to the generic update phrase", "[state_event]")
{
    const auto p = state_event_phrase(state_row("org.example.thing", "set", {}));
    CHECK(contains(p, "Alice updated the room state"));
    CHECK(contains(p, "org.example.thing"));
}

TEST_CASE("run phrase reflects the repeat count", "[state_event]")
{
    const auto last = state_row("m.room.join_rules", "changed", {"invite", "public"});
    const auto one = state_event_run_phrase("Alice", "m.room.join_rules", 1, last);
    const auto three = state_event_run_phrase("Alice", "m.room.join_rules", 3, last);
    CHECK(one != three);
    CHECK(contains(three, "3"));
}

TEST_CASE("power_levels counts overflow and several entries", "[state_event]")
{
    const auto p = state_event_phrase(state_row(
        "m.room.power_levels", "changed", {"field|ban|50|75", "field|kick|50|75", "more|||4"}));
    CHECK(contains(p, "6 permissions"));
}

TEST_CASE("power_levels user and event scopes name their key", "[state_event]")
{
    const auto u =
        state_event_phrase(state_row("m.room.power_levels", "changed", {"user|@bob:x|0|50"}));
    CHECK(contains(u, "@bob:x"));
    CHECK(contains(u, "50"));
    const auto e =
        state_event_phrase(state_row("m.room.power_levels", "changed", {"event|m.room.name||100"}));
    CHECK(contains(e, "m.room.name"));
    CHECK(contains(e, "100"));
}

TEST_CASE("power_levels removed event override has no dangling value", "[state_event]")
{
    const auto p =
        state_event_phrase(state_row("m.room.power_levels", "changed", {"event|m.room.topic|50|"}));
    CHECK(contains(p, "removed the custom power level needed for m.room.topic"));
    CHECK_FALSE(contains(p, "from 50"));
}

TEST_CASE("space child and parent name the other room", "[state_event]")
{
    auto child = state_row("m.space.child", "added", {});
    child.state_key = "!other:x";
    CHECK(contains(state_event_phrase(child), "!other:x"));
    auto parent = state_row("m.space.parent", "removed", {});
    parent.state_key = "!other:x";
    const auto p = state_event_phrase(parent);
    CHECK(contains(p, "!other:x"));
    CHECK(contains(p, "removed"));
}

TEST_CASE("a first power levels event reads as set, not changed", "[state_event]")
{
    const auto user =
        state_event_phrase(state_row("m.room.power_levels", "set", {"user|@a:x|0|100"}));
    CHECK(contains(user, "set the power level of"));
    CHECK_FALSE(contains(user, "changed"));
    const auto field =
        state_event_phrase(state_row("m.room.power_levels", "set", {"field|ban|50|75"}));
    CHECK_FALSE(contains(field, "changed"));
    CHECK(contains(field, "75"));
}

TEST_CASE("policy rule phrase names the entity", "[state_event]")
{
    const auto p = state_event_phrase(state_row("m.policy.rule.user", "set", {"@spam:x"}));
    CHECK(contains(p, "@spam:x"));
}

TEST_CASE("run phrase for a server_acl run is counted", "[state_event]")
{
    const auto last = state_row("m.room.server_acl", "changed", {"deny+3"});
    const auto p = state_event_run_phrase("Alice", "m.room.server_acl", 3, last);
    CHECK(contains(p, "Alice"));
    CHECK(contains(p, "3"));
}

TEST_CASE("known_state_label covers bridge events", "[state_event]")
{
    CHECK_FALSE(known_state_label("m.bridge").empty());
}

TEST_CASE("state_summary_run_phrase counts the changes and names one sender", "[state_event]")
{
    using tesseract::views::state_summary_run_phrase;
    const auto two = state_summary_run_phrase("Alice", 2);
    const auto five = state_summary_run_phrase("Alice", 5);
    const auto several = state_summary_run_phrase("", 5);
    CHECK(contains(two, "Alice"));
    CHECK(contains(two, "2"));
    CHECK(contains(five, "5"));
    CHECK(two != five);
    CHECK_FALSE(contains(several, "Alice"));
    CHECK(contains(several, "5"));
}
