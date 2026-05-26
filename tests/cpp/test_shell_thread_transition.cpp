#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

using tesseract::ShellBase;
using P = ShellBase::ThreadPanel;
using Tr = ShellBase::ThreadTrigger;

TEST_CASE("ToggleList opens the list from Closed", "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::ToggleList, "");
    CHECK(t.new_state == P::List);
    CHECK(t.new_prev == P::Closed);
    CHECK(t.new_root.empty());
    CHECK(t.subscribe_room_threads_);
    CHECK_FALSE(t.unsubscribe_room_threads_);
    CHECK(t.threads_to_subscribe.empty());
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("ToggleList closes the list", "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::ToggleList, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("ToggleList while Open closes everything",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$root", Tr::ToggleList, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.new_root.empty());
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$root");
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("OpenFromList Open(a) subscribes the thread",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::OpenFromList, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
    CHECK(t.new_root == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$a");
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("OpenFromList from Open(a) swaps to Open(b)",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::OpenFromList, "$b");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
    CHECK(t.new_root == "$b");
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$b");
}

TEST_CASE("OpenFromMain from Closed remembers Closed as prev",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::OpenFromMain, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::Closed);
    CHECK(t.new_root == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$a");
    CHECK_FALSE(t.subscribe_room_threads_);
}

TEST_CASE("OpenFromMain from List remembers List as prev",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::OpenFromMain, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
}

TEST_CASE("CloseThread returns to previous state",
          "[shell][thread_transition]")
{
    auto from_list = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::CloseThread, "");
    CHECK(from_list.new_state == P::List);
    CHECK(from_list.new_prev == P::Closed);
    CHECK(from_list.new_root.empty());
    REQUIRE(from_list.threads_to_unsubscribe.size() == 1);
    CHECK(from_list.threads_to_unsubscribe[0] == "$a");
    CHECK_FALSE(from_list.unsubscribe_room_threads_);

    auto from_closed = ShellBase::compute_thread_transition_(
        P::Open, P::Closed, "$a", Tr::CloseThread, "");
    CHECK(from_closed.new_state == P::Closed);
    CHECK_FALSE(from_closed.unsubscribe_room_threads_);
}

TEST_CASE("CloseThread when not Open is a no-op",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::CloseThread, "");
    CHECK(t.new_state == P::List);
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("RoomSwitch from Open(List-prev) releases both subs",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.new_prev == P::Closed);
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$a");
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("RoomSwitch from List releases list sub only",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.threads_to_unsubscribe.empty());
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("RoomSwitch from Closed still releases the room-thread sub",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.threads_to_unsubscribe.empty());
    // Shell keeps a background thread-list subscription on the active room
    // (for the header threads-button visibility check) regardless of panel
    // state, so every RoomSwitch must release it. Underlying unsubscribe is
    // a no-op when no handle exists.
    CHECK(t.unsubscribe_room_threads_);
}
