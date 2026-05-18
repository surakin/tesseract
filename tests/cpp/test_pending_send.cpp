#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

#include <tesseract/types.h>

#include <string>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using PS = MessageRowData::PendingState;

// ─────────────────────────────────────────────────────────────────────────────
//  make_row_data: PendingState mapping
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_row_data maps sending state to PendingState::Sending",
          "[ui][pending]")
{
    tesseract::TextEvent ev;
    ev.event_id = "$txn1";
    ev.sender = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body = "hello";
    ev.pending_state = "sending";
    ev.pending_txn_id = "txn-1";

    const std::string my_user_id = "@alice:example.org";
    MessageRowData row = tesseract::views::make_row_data(ev, my_user_id);

    CHECK(row.pending_state == PS::Sending);
    CHECK(row.pending_txn_id == "txn-1");
    CHECK(row.is_own == true); // sender matches my_user_id
}

TEST_CASE(
    "make_row_data maps failed recoverable to PendingState::Failed with error",
    "[ui][pending]")
{
    tesseract::TextEvent ev;
    ev.event_id = "$txn2";
    ev.sender = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body = "hello";
    ev.pending_state = "failed";
    ev.pending_txn_id = "txn-2";
    ev.pending_error = "network timeout";
    ev.pending_recoverable = true;

    const std::string my_user_id = "@alice:example.org";
    MessageRowData row = tesseract::views::make_row_data(ev, my_user_id);

    CHECK(row.pending_state == PS::Failed);
    CHECK(row.pending_recoverable == true);
    CHECK(row.pending_error == "network timeout");
}

TEST_CASE("make_row_data maps failed unrecoverable to PendingState::Failed "
          "recoverable=false",
          "[ui][pending]")
{
    tesseract::TextEvent ev;
    ev.event_id = "$txn3";
    ev.sender = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body = "hello";
    ev.pending_state = "failed";
    ev.pending_txn_id = "txn-3";
    ev.pending_error = "forbidden";
    ev.pending_recoverable = false;

    const std::string my_user_id = "@alice:example.org";
    MessageRowData row = tesseract::views::make_row_data(ev, my_user_id);

    CHECK(row.pending_state == PS::Failed);
    CHECK(row.pending_recoverable == false);
}

TEST_CASE("make_row_data maps empty pending_state to PendingState::None",
          "[ui][pending]")
{
    tesseract::TextEvent ev;
    ev.event_id = "$evt1";
    ev.sender = "@bob:example.org";
    ev.sender_name = "Bob";
    ev.body = "a server message";
    ev.pending_state = ""; // empty → confirmed server event

    const std::string my_user_id = "@alice:example.org";
    MessageRowData row = tesseract::views::make_row_data(ev, my_user_id);

    CHECK(row.pending_state == PS::None);
    CHECK(row.is_own == false); // sender does not match my_user_id
}

// ─────────────────────────────────────────────────────────────────────────────
//  update_message: Sending → None fires on_just_sent only for own messages
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("update_message fires on_just_sent on Sending→None for own message, "
          "not for non-own",
          "[ui][pending]")
{
    // Set up a view with two rows: one own-sending, one non-own-sending.
    MessageListView view;

    MessageRowData own{};
    own.kind = MessageRowData::Kind::Text;
    own.event_id = "$own-sending";
    own.sender = "@alice:example.org";
    own.sender_name = "Alice";
    own.body = "my message";
    own.is_own = true;
    own.pending_state = PS::Sending;

    MessageRowData other{};
    other.kind = MessageRowData::Kind::Text;
    other.event_id = "$other-sending";
    other.sender = "@bob:example.org";
    other.sender_name = "Bob";
    other.body = "bob's message";
    other.is_own = false;
    other.pending_state = PS::Sending;

    view.set_messages({own, other});

    std::string just_sent_event;
    int just_sent_count = 0;
    view.on_just_sent = [&](const std::string& eid)
    {
        just_sent_event = eid;
        ++just_sent_count;
    };

    // Transition own message from Sending → None.
    MessageRowData own_confirmed{};
    own_confirmed.kind = MessageRowData::Kind::Text;
    own_confirmed.event_id = "$own-confirmed";
    own_confirmed.sender = "@alice:example.org";
    own_confirmed.sender_name = "Alice";
    own_confirmed.body = "my message";
    own_confirmed.is_own = true;
    own_confirmed.pending_state = PS::None;
    view.update_message(0, own_confirmed);

    CHECK(just_sent_count == 1);
    CHECK(just_sent_event == "$own-confirmed");

    // Transition non-own message from Sending → None.
    MessageRowData other_confirmed{};
    other_confirmed.kind = MessageRowData::Kind::Text;
    other_confirmed.event_id = "$other-confirmed";
    other_confirmed.sender = "@bob:example.org";
    other_confirmed.sender_name = "Bob";
    other_confirmed.body = "bob's message";
    other_confirmed.is_own = false;
    other_confirmed.pending_state = PS::None;
    view.update_message(1, other_confirmed);

    // on_just_sent must NOT fire for the non-own row.
    CHECK(just_sent_count == 1);
}
