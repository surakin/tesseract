#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

#include <unordered_set>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

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
