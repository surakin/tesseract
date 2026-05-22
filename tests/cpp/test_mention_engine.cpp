#include <catch2/catch_test_macros.hpp>

#include "views/MentionEngine.h"

using tesseract::RoomMember;
using tesseract::views::MentionEngine;

namespace
{
std::vector<RoomMember> sample_members()
{
    return {
        {"@alice:example.org", "Alice", ""},
        {"@bob:example.org", "Bob Smith", ""},
        {"@carol:example.org", "Carol", ""},
        {"@al:example.org", "", ""}, // no display name -> localpart
    };
}
} // namespace

// --- find_prefix --------------------------------------------------------

TEST_CASE("mention engine: finds @prefix at end of text", "[mention][engine]")
{
    MentionEngine e;
    std::string t = "hey @al";
    auto m = e.find_prefix(t, (int)t.size());
    REQUIRE(m.has_value());
    CHECK(m->start == 4);
    CHECK(m->end == 7);
    CHECK(m->prefix == "al");
}

TEST_CASE("mention engine: bare @ matches with empty prefix",
          "[mention][engine]")
{
    MentionEngine e;
    auto m = e.find_prefix("@", 1);
    REQUIRE(m.has_value());
    CHECK(m->start == 0);
    CHECK(m->prefix.empty());

    auto m2 = e.find_prefix("hi @", 4);
    REQUIRE(m2.has_value());
    CHECK(m2->start == 3);
}

TEST_CASE("mention engine: email addresses do not trigger",
          "[mention][engine]")
{
    MentionEngine e;
    CHECK_FALSE(e.find_prefix("mail foo@bar", 12).has_value());
    CHECK_FALSE(e.find_prefix("foo@bar", 7).has_value());
}

TEST_CASE("mention engine: cursor outside a mention returns nullopt",
          "[mention][engine]")
{
    MentionEngine e;
    CHECK_FALSE(e.find_prefix("just text", 4).has_value());
    CHECK_FALSE(e.find_prefix("@alice here", 11).has_value()); // after a space
}

// --- lookup -------------------------------------------------------------

TEST_CASE("mention engine: lookup filters by display name and localpart",
          "[mention][engine]")
{
    MentionEngine e;
    auto r = e.lookup("al", sample_members(), 8, /*include_room=*/false);
    // Alice (name "Alice") and @al (localpart "al") match; Bob/Carol do not.
    bool has_alice = false, has_al = false, has_bob = false;
    for (const auto& c : r)
    {
        if (c.user_id == "@alice:example.org")
            has_alice = true;
        if (c.user_id == "@al:example.org")
            has_al = true;
        if (c.user_id == "@bob:example.org")
            has_bob = true;
    }
    CHECK(has_alice);
    CHECK(has_al);
    CHECK_FALSE(has_bob);
}

TEST_CASE("mention engine: empty prefix lists all members",
          "[mention][engine]")
{
    MentionEngine e;
    auto r = e.lookup("", sample_members(), 8, /*include_room=*/false);
    CHECK(r.size() == 4);
}

TEST_CASE("mention engine: @room is first when included", "[mention][engine]")
{
    MentionEngine e;
    auto r = e.lookup("", sample_members(), 8, /*include_room=*/true);
    REQUIRE_FALSE(r.empty());
    CHECK(r.front().is_room);
    CHECK(r.front().display_name == "@room");
}

TEST_CASE("mention engine: @room appears when prefix matches 'room'",
          "[mention][engine]")
{
    MentionEngine e;
    auto with = e.lookup("ro", sample_members(), 8, true);
    bool has_room = false;
    for (const auto& c : with)
        if (c.is_room)
            has_room = true;
    CHECK(has_room);

    auto without = e.lookup("xyz", sample_members(), 8, true);
    for (const auto& c : without)
        CHECK_FALSE(c.is_room);
}

TEST_CASE("mention engine: starts-with ranks above substring",
          "[mention][engine]")
{
    MentionEngine e;
    std::vector<RoomMember> members = {
        {"@x:e.org", "Pascal", ""},  // contains "cal" as substring
        {"@y:e.org", "Calvin", ""},  // starts with "cal"
    };
    auto r = e.lookup("cal", members, 8, false);
    REQUIRE(r.size() == 2);
    CHECK(r.front().display_name == "Calvin");
}

TEST_CASE("mention engine: empty display name falls back to user id",
          "[mention][engine]")
{
    MentionEngine e;
    auto r = e.lookup("al", sample_members(), 8, false);
    for (const auto& c : r)
        if (c.user_id == "@al:example.org")
            CHECK(c.display_name == "@al:example.org");
}

TEST_CASE("mention engine: max_results is honoured", "[mention][engine]")
{
    MentionEngine e;
    auto r = e.lookup("", sample_members(), 2, false);
    CHECK(r.size() == 2);
}
