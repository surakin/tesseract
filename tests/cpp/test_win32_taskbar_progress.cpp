#include <catch2/catch_test_macros.hpp>

#include "Win32Taskbar.h"

TEST_CASE("taskbar upload progress is byte weighted")
{
    win32::TaskbarProgressModel model;
    model.update(1, 25, 100);
    model.update(2, 50, 900);
    const auto progress = model.snapshot();
    REQUIRE(progress.active);
    REQUIRE(progress.current == 75);
    REQUIRE(progress.total == 1000);
}

TEST_CASE("taskbar upload progress validates and completes requests")
{
    win32::TaskbarProgressModel model;
    model.update(0, 5, 10);
    model.update(1, 5, 0);
    REQUIRE_FALSE(model.snapshot().active);

    model.update(7, 20, 10);
    REQUIRE(model.snapshot().current == 10);
    const auto before = model.finish(7);
    REQUIRE(before);
    REQUIRE(before->active);
    REQUIRE(before->current == 10);
    REQUIRE_FALSE(model.snapshot().active);

    model.update(7, 1, 2);
    REQUIRE_FALSE(model.finish(99)); // stale completion is ignored
    REQUIRE(model.snapshot().active);
    model.clear();
    REQUIRE_FALSE(model.snapshot().active);
}
