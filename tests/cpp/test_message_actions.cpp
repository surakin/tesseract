#include <catch2/catch_test_macros.hpp>

#include "views/MessageActions.h"

#include <algorithm>

using tesseract::views::build_message_actions;
using tesseract::views::MessageAction;
using tesseract::views::MessageActionId;
using tesseract::views::MessageActionInputs;

namespace
{
bool has(const std::vector<MessageAction>& v, MessageActionId id)
{
    return std::any_of(v.begin(), v.end(),
                       [id](const MessageAction& a) { return a.id == id; });
}

std::vector<MessageActionId> ids(const std::vector<MessageAction>& v)
{
    std::vector<MessageActionId> out;
    for (const auto& a : v)
        out.push_back(a.id);
    return out;
}
} // namespace

TEST_CASE("message actions: someone else's plain message", "[message_actions]")
{
    MessageActionInputs in;
    in.can_thread = true;
    in.thread_button_visible = true;
    const auto a = build_message_actions(in);
    CHECK(ids(a) == std::vector{MessageActionId::React, MessageActionId::Reply,
                                MessageActionId::ReplyInThread,
                                MessageActionId::Forward,
                                MessageActionId::Report});
    CHECK_FALSE(has(a, MessageActionId::Edit));
    CHECK_FALSE(has(a, MessageActionId::Delete));
}

TEST_CASE("message actions: own editable message lists pill actions first",
          "[message_actions]")
{
    MessageActionInputs in;
    in.is_own = true;
    in.editable_kind = true;
    in.can_pin = true;
    in.developer_mode = true;
    const auto a = build_message_actions(in);
    CHECK(ids(a) == std::vector{MessageActionId::React, MessageActionId::Reply,
                                MessageActionId::Edit, MessageActionId::Forward,
                                MessageActionId::Delete, MessageActionId::Pin,
                                MessageActionId::CopyEventSource});
    for (const auto& act : a)
        CHECK(act.in_pill == (act.id == MessageActionId::React ||
                              act.id == MessageActionId::Reply ||
                              act.id == MessageActionId::Edit));
    CHECK(a[4].destructive);
}

TEST_CASE("message actions: own forwarded message gets no Edit but keeps the rest",
          "[message_actions]")
{
    MessageActionInputs in;
    in.is_own = true;
    in.editable_kind = true;
    in.forwarded = true;
    const auto a = build_message_actions(in);
    CHECK_FALSE(has(a, MessageActionId::Edit));
    CHECK(has(a, MessageActionId::Reply));
    CHECK(has(a, MessageActionId::Forward));
    CHECK(has(a, MessageActionId::Delete));

    in.forwarded = false;
    CHECK(has(build_message_actions(in), MessageActionId::Edit));
}

TEST_CASE("message actions: pinned message offers Unpin", "[message_actions]")
{
    MessageActionInputs in;
    in.can_pin = true;
    in.is_pinned = true;
    const auto a = build_message_actions(in);
    CHECK(has(a, MessageActionId::Unpin));
    CHECK_FALSE(has(a, MessageActionId::Pin));
}

TEST_CASE("message actions: polls cannot be forwarded", "[message_actions]")
{
    MessageActionInputs in;
    in.poll = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Forward));
}

TEST_CASE("message actions: redacted and undecryptable rows", "[message_actions]")
{
    MessageActionInputs in;
    in.is_own = true;
    in.redacted = true;
    in.thread_button_visible = true;
    in.can_thread = true;
    CHECK(ids(build_message_actions(in)) ==
          std::vector{MessageActionId::Reply});

    in.redacted = false;
    in.utd = true;
    CHECK(ids(build_message_actions(in)) ==
          std::vector{MessageActionId::Reply, MessageActionId::ReplyInThread});
}

TEST_CASE("message actions: a message already in a thread cannot start one",
          "[message_actions]")
{
    MessageActionInputs in;
    in.thread_button_visible = true;
    in.can_thread = false;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::ReplyInThread));
}

TEST_CASE("message actions: moderators can delete others' messages",
          "[message_actions]")
{
    MessageActionInputs in;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Delete));
    in.can_redact_others = true;
    CHECK(has(build_message_actions(in), MessageActionId::Delete));
}

TEST_CASE("message actions: unconfirmed echoes are not forwardable",
          "[message_actions]")
{
    MessageActionInputs in;
    in.pending = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Forward));
}

TEST_CASE("message actions: only confirmed messages from others are reportable",
          "[message_actions]")
{
    MessageActionInputs in;
    CHECK(has(build_message_actions(in), MessageActionId::Report));

    in.poll = true;
    CHECK(has(build_message_actions(in), MessageActionId::Report));

    in = {};
    in.is_own = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Report));

    in = {};
    in.pending = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Report));

    in = {};
    in.redacted = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Report));
    in = {};
    in.utd = true;
    CHECK_FALSE(has(build_message_actions(in), MessageActionId::Report));
}

TEST_CASE("message actions: Report is destructive and lives in the overflow",
          "[message_actions]")
{
    const auto a = build_message_actions({});
    const auto it = std::find_if(a.begin(), a.end(), [](const MessageAction& m)
                                 { return m.id == MessageActionId::Report; });
    REQUIRE(it != a.end());
    CHECK(it->destructive);
    CHECK_FALSE(it->in_pill);
}
