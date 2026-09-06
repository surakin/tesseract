#include <catch2/catch_test_macros.hpp>

#include "app/ThreadPanelController.h"

using tesseract::ThreadPanelController;

// Regression coverage for the reentrancy guard that ThreadView's now-eager
// arrange-time autofill (see test_tk_thread_view.cpp) depends on: with
// autofill_only_when_empty disabled, ListView::arrange() may call on_near_top
// (-> RoomPane::paginate_thread_back_ -> begin_paginate) on every relayout
// pass while a fetch is still in flight. begin_paginate's in-flight/
// reached-start guard must make those redundant calls harmless no-ops —
// otherwise a sparse thread would spam duplicate pagination requests, the
// same class of bug test_tk_room_media_view.cpp guards against for
// RoomMediaView's own (differently-guarded) pagination path.

TEST_CASE("begin_paginate rejects a second call while one is in flight",
          "[thread_panel_controller]")
{
    ThreadPanelController ctl;
    int runs = 0;
    ctl.set_run_paginate([&] { ++runs; });

    CHECK(ctl.begin_paginate(/*can_paginate=*/true));
    CHECK(runs == 1);

    // Simulates repeated arrange() passes firing on_near_top again before
    // the async fetch's result has landed.
    CHECK_FALSE(ctl.begin_paginate(/*can_paginate=*/true));
    CHECK_FALSE(ctl.begin_paginate(/*can_paginate=*/true));
    CHECK(runs == 1);
}

TEST_CASE("begin_paginate succeeds again once on_paginate_result clears in-flight",
          "[thread_panel_controller]")
{
    ThreadPanelController ctl;
    int runs = 0;
    ctl.set_run_paginate([&] { ++runs; });

    REQUIRE(ctl.begin_paginate(true));
    REQUIRE(runs == 1);

    // Fetch resolves with more history remaining.
    CHECK_FALSE(ctl.on_paginate_result(/*reached=*/false, /*want_more=*/false));
    CHECK_FALSE(ctl.reached_start());
    CHECK_FALSE(ctl.paginating());

    CHECK(ctl.begin_paginate(true));
    CHECK(runs == 2);
}

TEST_CASE("begin_paginate stays rejected forever once reached_start is reported",
          "[thread_panel_controller]")
{
    ThreadPanelController ctl;
    int runs = 0;
    ctl.set_run_paginate([&] { ++runs; });

    REQUIRE(ctl.begin_paginate(true));
    REQUIRE(runs == 1);

    CHECK_FALSE(ctl.on_paginate_result(/*reached=*/true, /*want_more=*/false));
    CHECK(ctl.reached_start());

    // A short thread with no more history: further arrange-time fires must
    // stay harmless no-ops indefinitely.
    CHECK_FALSE(ctl.begin_paginate(true));
    CHECK_FALSE(ctl.begin_paginate(true));
    CHECK(runs == 1);
}
