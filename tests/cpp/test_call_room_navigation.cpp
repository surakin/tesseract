#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

using tesseract::CallRoomAction;
using tesseract::decide_call_room_action;

TEST_CASE("decide_call_room_action: no active call, plain room -> None")
{
    CHECK(decide_call_room_action(/*has_active_call=*/false,
                                  /*active_call_room_is_new_room=*/false,
                                  /*new_room_is_call_room=*/false,
                                  /*call_auto_floated=*/false,
                                  /*overlay_is_docked=*/false) ==
          CallRoomAction::None);
}

TEST_CASE("decide_call_room_action: no active call, call room -> AutoJoin")
{
    CHECK(decide_call_room_action(false, false, /*new_room_is_call_room=*/true,
                                  false, false) == CallRoomAction::AutoJoin);
}

TEST_CASE("decide_call_room_action: returning to the active call's room, "
          "auto-floated -> AutoRestore")
{
    CHECK(decide_call_room_action(/*has_active_call=*/true,
                                  /*active_call_room_is_new_room=*/true,
                                  /*new_room_is_call_room=*/true,
                                  /*call_auto_floated=*/true,
                                  /*overlay_is_docked=*/false) ==
          CallRoomAction::AutoRestore);
}

TEST_CASE("decide_call_room_action: returning to the active call's room, "
          "not auto-floated -> None (user's own mode choice stands)")
{
    CHECK(decide_call_room_action(true, true, true,
                                  /*call_auto_floated=*/false, false) ==
          CallRoomAction::None);
}

TEST_CASE("decide_call_room_action: switching directly to another call room "
          "-> LeaveAndJoin, regardless of overlay mode")
{
    CHECK(decide_call_room_action(/*has_active_call=*/true,
                                  /*active_call_room_is_new_room=*/false,
                                  /*new_room_is_call_room=*/true,
                                  false, /*overlay_is_docked=*/true) ==
          CallRoomAction::LeaveAndJoin);
    CHECK(decide_call_room_action(true, false, true, false,
                                  /*overlay_is_docked=*/false) ==
          CallRoomAction::LeaveAndJoin);
}

TEST_CASE("decide_call_room_action: leaving the active call's room while "
          "docked -> AutoFloat")
{
    CHECK(decide_call_room_action(/*has_active_call=*/true,
                                  /*active_call_room_is_new_room=*/false,
                                  /*new_room_is_call_room=*/false,
                                  false, /*overlay_is_docked=*/true) ==
          CallRoomAction::AutoFloat);
}

TEST_CASE("decide_call_room_action: leaving the active call's room while "
          "already floating/popout -> NoOp")
{
    CHECK(decide_call_room_action(true, false, false, false,
                                  /*overlay_is_docked=*/false) ==
          CallRoomAction::NoOp);
}
