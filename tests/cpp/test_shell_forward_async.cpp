#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// Minimal ShellBase test double for the forward-async callback handlers.
// main_app_ stays null so picker calls are skipped; we exercise the
// pending_forwards_ state mutations directly.
struct ForwardShell : tesseract::test::TestShellBase
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

    using ShellBase::handle_forward_done_ui_;
    using ShellBase::handle_forward_failed_ui_;
    using ShellBase::my_user_id_;
    using ShellBase::pending_forwards_;
    using ShellBase::push_rooms_;
};

} // namespace

// ── handle_forward_done_ui_ ───────────────────────────────────────────────

TEST_CASE("handle_forward_done_ui_ erases the completed entry", "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[42] = "!room:x";
    s.handle_forward_done_ui_(42);
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_done_ui_ does not disturb other in-flight requests",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!a:x";
    s.pending_forwards_[2] = "!b:x";
    s.handle_forward_done_ui_(1);
    REQUIRE(s.pending_forwards_.size() == 1);
    REQUIRE(s.pending_forwards_.count(2) == 1);
}

TEST_CASE("handle_forward_done_ui_ with unknown request_id is a no-op", "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!room:x";
    s.handle_forward_done_ui_(99);
    REQUIRE(s.pending_forwards_.size() == 1);
}

// ── handle_forward_failed_ui_ ────────────────────────────────────────────

TEST_CASE("handle_forward_failed_ui_ ignores unknown request_id", "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!room:x";
    s.handle_forward_failed_ui_(99, "network error");
    REQUIRE(s.pending_forwards_.size() == 1);
}

TEST_CASE("handle_forward_failed_ui_ erases the failed entry", "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[7] = "!room:x";
    s.handle_forward_failed_ui_(7, "timeout");
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_failed_ui_ leaves other in-flight requests intact",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!a:x";
    s.pending_forwards_[2] = "!b:x";
    s.handle_forward_failed_ui_(1, "error");
    REQUIRE(s.pending_forwards_.size() == 1);
    REQUIRE(s.pending_forwards_.count(2) == 1);
}

TEST_CASE("handle_forward_failed_ui_ falls back to room_id when room unknown",
          "[shell][forward_async]")
{
    // Room is not in the room list — room_by_id_ returns null.
    // The handler must not crash; it falls back to the raw room_id string.
    ForwardShell s;
    s.my_user_id_ = "@me:x";
    s.pending_forwards_[3] = "!unknown:x";
    s.handle_forward_failed_ui_(3, "error"); // must not crash
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_failed_ui_ uses room name when room is known", "[shell][forward_async]")
{
    ForwardShell s;
    s.my_user_id_ = "@me:x";
    tesseract::RoomInfo r;
    r.id = "!a:x";
    r.name = "General";
    s.push_rooms_("@me:x", {r});

    s.pending_forwards_[5] = "!a:x";
    // fp is null (no MainAppWidget in tests) so add_forward_error is not called,
    // but the handler must not crash and must erase the entry regardless.
    s.handle_forward_failed_ui_(5, "send failed");
    REQUIRE(s.pending_forwards_.empty());
}
