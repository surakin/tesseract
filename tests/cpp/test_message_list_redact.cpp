#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

MessageRowData make_redact_test_row(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("set_can_redact_others flips the flag", "[message_list][redact]")
{
    MessageListView v;
    CHECK_FALSE(v.can_redact_others());
    v.set_can_redact_others(true);
    CHECK(v.can_redact_others());
    v.set_can_redact_others(false);
    CHECK_FALSE(v.can_redact_others());
}

TEST_CASE("set_messages(.., room_switch=true) preserves can_redact_others",
          "[message_list][redact]")
{
    // Mirrors ShellBase::refresh_pinned_for_current_room_()'s call ordering
    // for can_pin_ (see test_message_list_pinning.cpp): the host pushes the
    // new room's redact-others permission via set_can_redact_others()
    // synchronously on room switch, before the async timeline-reset
    // callback that calls set_messages(room_switch=true) can land.
    // set_messages() must not clear that already-correct state back to
    // false.
    MessageListView v;
    v.set_can_redact_others(true);
    v.set_messages({make_redact_test_row("$x")}, /*room_switch=*/true);
    CHECK(v.can_redact_others());
}

TEST_CASE("set_messages(.., room_switch=false) preserves can_redact_others",
          "[message_list][redact]")
{
    MessageListView v;
    v.set_can_redact_others(true);
    v.set_messages({make_redact_test_row("$x")}, /*room_switch=*/false);
    CHECK(v.can_redact_others());
}
