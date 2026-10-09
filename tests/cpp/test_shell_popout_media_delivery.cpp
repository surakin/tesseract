#include <catch2/catch_test_macros.hpp>

#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "tk/theme.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::RoomWindowBase;
using tesseract::ShellBase;

namespace
{

// A ShellBase test double exposing the secondary-window registry plumbing
// (register_room_window_/unregister_room_window_) and the pop-out media-group
// allowlist it should maintain, without a window/canvas/live session. The
// pure-virtual surface is stubbed to no-ops, mirroring PriorityShell in
// test_shell_media_priority.cpp.
struct PopoutMediaShell : tesseract::test::TestShellBase
{

    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}

    // Expose the protected registry/gating plumbing under test.
    using ShellBase::active_media_group_;
    using ShellBase::active_media_view_groups_;
    using ShellBase::active_popout_media_groups_;
    using ShellBase::current_room_id_;
    using ShellBase::media_group_for_room_;
    using ShellBase::register_room_window_;
    using ShellBase::unregister_room_window_;
    using ShellBase::active_account_;
    using ShellBase::pending_popout_owner_;
    using ShellBase::find_secondary_;
    using ShellBase::find_event_secondary_;
    using ShellBase::EventAccountScope;
};

// Minimal concrete RoomWindowBase: never calls init_pane_()/finish_init_(), so
// pane_ stays null throughout — fine here, since register_room_window_/
// unregister_room_window_ only read room_id() and never touch pane_.
struct TestRoomWindow : RoomWindowBase
{
    TestRoomWindow(ShellBase* shell, std::string room_id)
        : RoomWindowBase(shell, std::move(room_id))
    {
    }
    void bring_to_front() override {}
    void close_window() override {}
    void request_relayout() override {}
    void apply_theme(const tk::Theme&) override {}
    void apply_scale_change(float) override {}

protected:
    void surface_repaint_() override {}
};

} // namespace

TEST_CASE(
    "register_room_window_ registers the popout's own room so its ordinary "
    "timeline media isn't dropped by should_deliver_",
    "[shell][popout][media]")
{
    // Regression test for: pop-out windows silently failed to load thumbnails
    // for old (backward-paginated) events. Root cause: fetch_media_pipeline_'s
    // should_deliver_ gate only ever allowed the main window's current room
    // (active_media_group_) or an open gallery's group
    // (active_media_view_groups_) through — a pop-out showing a DIFFERENT room
    // than the main window's current one had no equivalent allowlist entry, so
    // any media fetch for its room was dropped as "stale" before it ever
    // reached decode/cache/repaint.
    PopoutMediaShell s;
    s.current_room_id_ = "!main:example.org";
    s.active_media_group_ = s.media_group_for_room_(s.current_room_id_);

    const std::string popout_room = "!popout:example.org";
    const auto popout_grp = s.media_group_for_room_(popout_room);

    // Sanity: the popout's room is genuinely a different group than the main
    // window's, and not already covered by any gallery.
    REQUIRE(popout_grp != s.active_media_group_);
    REQUIRE(s.active_media_view_groups_.count(popout_grp) == 0);
    CHECK(s.active_popout_media_groups_.count(popout_grp) == 0);

    TestRoomWindow win(&s, popout_room);
    s.register_room_window_(&win);
    CHECK(s.active_popout_media_groups_.count(popout_grp) == 1);

    s.unregister_room_window_(&win);
    CHECK(s.active_popout_media_groups_.count(popout_grp) == 0);
}

TEST_CASE(
    "unregister_room_window_ only clears the group for the window that "
    "actually owns it",
    "[shell][popout][media]")
{
    // Mirrors register_room_window_'s own it->second == w guard for
    // secondary_windows_ — a stale unregister call (e.g. from a window that
    // already lost the registry slot to a newer one for the same room) must
    // not clear the group out from under the window that's still live.
    PopoutMediaShell s;
    const std::string room_id = "!popout:example.org";
    const auto grp = s.media_group_for_room_(room_id);

    TestRoomWindow win_a(&s, room_id);
    TestRoomWindow win_b(&s, room_id);

    s.register_room_window_(&win_a);
    s.register_room_window_(&win_b); // win_b now owns the registry slot
    CHECK(s.active_popout_media_groups_.count(grp) == 1);

    s.unregister_room_window_(&win_a); // stale — win_b owns it now
    CHECK(s.active_popout_media_groups_.count(grp) == 1);

    s.unregister_room_window_(&win_b);
    CHECK(s.active_popout_media_groups_.count(grp) == 0);
}

namespace
{
std::shared_ptr<tesseract::AccountSession> make_popout_session(const std::string& uid)
{
    auto sess = std::make_shared<tesseract::AccountSession>();
    sess->user_id = uid;
    return sess;
}
} // namespace

TEST_CASE("each account can have its own pop-out of the same room",
          "[shell][popout][accounts]")
{
    PopoutMediaShell s;
    auto alice = make_popout_session("@alice:example.org");
    auto bob = make_popout_session("@bob:example.org");
    const std::string room = "!shared:example.org";
    const auto grp = s.media_group_for_room_(room);

    s.active_account_ = alice;
    TestRoomWindow alice_win(&s, room); // owner: the active account
    s.pending_popout_owner_ = bob;      // as open_room_in_new_window does
    TestRoomWindow bob_win(&s, room);
    s.pending_popout_owner_.reset();
    CHECK(alice_win.owner_user_id() == "@alice:example.org");
    CHECK(bob_win.owner_user_id() == "@bob:example.org");

    s.register_room_window_(&alice_win);
    s.register_room_window_(&bob_win);
    CHECK(s.find_secondary_(room, "@alice:example.org") == &alice_win);
    CHECK(s.find_secondary_(room, "@bob:example.org") == &bob_win);

    // Untagged dispatches target the active account's window; an event
    // tagged with Bob's account targets Bob's.
    CHECK(s.find_event_secondary_(room) == &alice_win);
    {
        PopoutMediaShell::EventAccountScope scope(s, "@bob:example.org");
        CHECK(s.find_event_secondary_(room) == &bob_win);
    }

    // The room's media group stays allowed until its last pop-out closes.
    s.unregister_room_window_(&alice_win);
    CHECK(s.active_popout_media_groups_.count(grp) == 1);
    CHECK(s.find_secondary_(room, "@bob:example.org") == &bob_win);
    s.unregister_room_window_(&bob_win);
    CHECK(s.active_popout_media_groups_.count(grp) == 0);
}
