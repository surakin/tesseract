#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double exposing push_rooms_ + the known-users roster cache
// flags, so the room-set-change invalidation can be exercised without a window
// or client. Pure-virtual surface stubbed to no-ops.
struct RosterShell : tesseract::test::TestShellBase
{
    void post_to_ui_(std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
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

    std::vector<std::function<void()>> queue;

    using ShellBase::known_users_built_;
    using ShellBase::my_user_id_;
    using ShellBase::push_rooms_;
};

tesseract::RoomInfo room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    return r;
}

} // namespace

TEST_CASE("roster survives a same-set push (no invalidation)", "[shell][roster]")
{
    RosterShell s;
    s.my_user_id_ = "@me:x";
    s.push_rooms_("@me:x", {room("!a:x"), room("!b:x")});
    s.known_users_built_ = true; // simulate a built roster

    // Same set, different order → no invalidation.
    s.push_rooms_("@me:x", {room("!b:x"), room("!a:x")});
    CHECK(s.known_users_built_);
}

TEST_CASE("roster is invalidated when the room set changes with equal count", "[shell][roster]")
{
    RosterShell s;
    s.my_user_id_ = "@me:x";
    s.push_rooms_("@me:x", {room("!a:x"), room("!b:x")});
    s.known_users_built_ = true;

    // Join one, leave one — count unchanged, set changed → must invalidate.
    s.push_rooms_("@me:x", {room("!a:x"), room("!c:x")});
    CHECK_FALSE(s.known_users_built_);
}
