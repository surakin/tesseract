#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::KnockedRoomInfo;
using tesseract::KnockRequestInfo;

namespace
{

// Minimal concrete ShellBase, mirroring test_shell_dispatch_room_send.cpp's
// harness: every pure virtual gets a no-op body, and `using` declarations
// expose the private knock-related members under test.
struct KnockShell : tesseract::test::TestShellBase
{

    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }

    using ShellBase::client_;
    using ShellBase::my_user_id_;
    using ShellBase::my_knocks_;
    using ShellBase::current_room_knock_requests_;
    using ShellBase::knock_requests_panel_room_id_;
    using ShellBase::pending_room_actions_;
    using ShellBase::RoomActionKind;
    using ShellBase::find_my_knock_;
    using ShellBase::push_my_knocks_;
    using ShellBase::decline_knock_request_async_;
    using ShellBase::decline_and_ban_knock_request_async_;
    using ShellBase::handle_room_action_complete_ui_;
    using ShellBase::next_room_action_id_;
};

KnockedRoomInfo make_knock(std::string room_id)
{
    KnockedRoomInfo k;
    k.room_id = std::move(room_id);
    k.room_name = "Test Room";
    return k;
}

KnockRequestInfo make_request(std::string room_id, std::string user_id)
{
    KnockRequestInfo r;
    r.room_id = std::move(room_id);
    r.user_id = std::move(user_id);
    r.display_name = "Alice";
    return r;
}

} // namespace

TEST_CASE(
    "push_my_knocks_ populates my_knocks_ for the active account and find_my_knock_ resolves it",
    "[shell][knock]")
{
    KnockShell s;
    s.my_user_id_ = "@me:example.org";

    std::vector<KnockedRoomInfo> knocks;
    knocks.push_back(make_knock("!a:example.org"));
    knocks.push_back(make_knock("!b:example.org"));

    s.push_my_knocks_("@me:example.org", knocks);

    REQUIRE(s.my_knocks_.size() == 2);
    const auto* found = s.find_my_knock_("!b:example.org");
    REQUIRE(found != nullptr);
    CHECK(found->room_id == "!b:example.org");
    CHECK(s.find_my_knock_("!missing:example.org") == nullptr);
}

TEST_CASE("push_my_knocks_ for a non-active account does not touch my_knocks_", "[shell][knock]")
{
    KnockShell s;
    s.my_user_id_ = "@me:example.org";

    std::vector<KnockedRoomInfo> other_knocks;
    other_knocks.push_back(make_knock("!other:example.org"));

    s.push_my_knocks_("@someone-else:example.org", other_knocks);

    CHECK(s.my_knocks_.empty());
}

TEST_CASE("decline_knock_request_async_ optimistically removes the request immediately",
          "[shell][knock]")
{
    KnockShell s;
    tesseract::Client client; // unauthenticated; the FFI call itself no-ops
    s.client_ = &client;

    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@bob:example.org"));

    s.decline_knock_request_async_("!r:example.org", "@alice:example.org");

    REQUIRE(s.current_room_knock_requests_.size() == 1);
    CHECK(s.current_room_knock_requests_.front().user_id == "@bob:example.org");
}

TEST_CASE("decline_and_ban_knock_request_async_ optimistically removes the request immediately",
          "[shell][knock]")
{
    KnockShell s;
    tesseract::Client client;
    s.client_ = &client;

    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));

    s.decline_and_ban_knock_request_async_("!r:example.org", "@alice:example.org", "spam");

    CHECK(s.current_room_knock_requests_.empty());
}

TEST_CASE("decline_knock_request_async_ is a no-op without a live client", "[shell][knock]")
{
    KnockShell s;
    // client_ defaults to nullptr.
    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));

    s.decline_knock_request_async_("!r:example.org", "@alice:example.org");

    // Guarded on !client_ before any local-list mutation — the request stays.
    CHECK(s.current_room_knock_requests_.size() == 1);
}

TEST_CASE(
    "handle_room_action_complete_ui_ resolves a pending Knock/AcceptKnock action without crashing",
    "[shell][knock]")
{
    KnockShell s;

    auto knock_id = s.next_room_action_id_++;
    s.pending_room_actions_[knock_id] = {"!r:example.org", ShellBase::RoomActionKind::Knock};
    s.handle_room_action_complete_ui_(knock_id, /*ok=*/true, "", "");
    CHECK(s.pending_room_actions_.count(knock_id) == 0);

    auto accept_id = s.next_room_action_id_++;
    s.pending_room_actions_[accept_id] = {"!r:example.org", ShellBase::RoomActionKind::AcceptKnock};
    s.handle_room_action_complete_ui_(accept_id, /*ok=*/true, "", "");
    CHECK(s.pending_room_actions_.count(accept_id) == 0);

    // Failure path also resolves cleanly (exercises the "send/accept join
    // request" verb strings in the failure-message switch).
    auto failed_id = s.next_room_action_id_++;
    s.pending_room_actions_[failed_id] = {"!r:example.org", ShellBase::RoomActionKind::Knock};
    s.handle_room_action_complete_ui_(failed_id, /*ok=*/false, "", "M_FORBIDDEN");
    CHECK(s.pending_room_actions_.count(failed_id) == 0);
}
