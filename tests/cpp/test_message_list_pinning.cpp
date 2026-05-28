#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"
#include "tesseract/types.h"

#include <unordered_set>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::make_row_data;

namespace
{

MessageRowData make_row(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("set_pinned_event_ids stores the set", "[message_list][pinning]")
{
    MessageListView v;
    CHECK(v.pinned_event_ids().empty());
    v.set_pinned_event_ids({"$a", "$b"});
    CHECK(v.pinned_event_ids().size() == 2);
    CHECK(v.pinned_event_ids().count("$a") == 1);
    v.set_pinned_event_ids({});
    CHECK(v.pinned_event_ids().empty());
}

TEST_CASE("set_can_pin flips the flag", "[message_list][pinning]")
{
    MessageListView v;
    CHECK_FALSE(v.can_pin());
    v.set_can_pin(true);
    CHECK(v.can_pin());
    v.set_can_pin(false);
    CHECK_FALSE(v.can_pin());
}

TEST_CASE("set_messages(.., room_switch=true) clears pinning state",
          "[message_list][pinning]")
{
    MessageListView v;
    v.set_pinned_event_ids({"$a"});
    v.set_can_pin(true);
    v.set_messages({make_row("$x")}, /*room_switch=*/true);
    CHECK(v.pinned_event_ids().empty());
    CHECK_FALSE(v.can_pin());
}

TEST_CASE("set_messages(.., room_switch=false) preserves pinning state",
          "[message_list][pinning]")
{
    MessageListView v;
    v.set_pinned_event_ids({"$a"});
    v.set_can_pin(true);
    v.set_messages({make_row("$x")}, /*room_switch=*/false);
    CHECK(v.pinned_event_ids().count("$a") == 1);
    CHECK(v.can_pin());
}

TEST_CASE("PinnedStateEvent produces Kind::PinnedEvent row with correct fields",
          "[message_list][pinning]")
{
    // Build a PinnedStateEvent using the SDK type.
    tesseract::PinnedStateEvent ev;
    ev.event_id    = "$pin:server";
    ev.sender      = "@alice:server";
    ev.sender_name = "Alice";
    ev.body        = "pinned a message";
    ev.timestamp   = 1700000042000ULL;

    // Convert via make_row_data (the same path the shell uses at runtime).
    MessageRowData row = make_row_data(ev, /*my_user_id=*/"@bot:server");

    REQUIRE(row.kind == MessageRowData::Kind::PinnedEvent);
    CHECK(row.event_id    == "$pin:server");
    CHECK(row.sender_name == "Alice");
    CHECK(row.body        == "pinned a message");
    CHECK(row.timestamp_ms == 1700000042000ULL);
    CHECK_FALSE(row.is_own); // sender != my_user_id

    // Verify the same row survives a set_messages round-trip.
    MessageListView v;
    v.set_messages({row}, /*room_switch=*/false);
    REQUIRE(v.messages().size() == 1);
    CHECK(v.messages()[0].kind         == MessageRowData::Kind::PinnedEvent);
    CHECK(v.messages()[0].sender_name  == "Alice");
    CHECK(v.messages()[0].body         == "pinned a message");
    CHECK(v.messages()[0].timestamp_ms == 1700000042000ULL);
}
