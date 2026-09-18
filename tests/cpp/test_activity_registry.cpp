#include <catch2/catch_test_macros.hpp>

#include "app/ActivityRegistry.h"

#include <cstdint>

using tesseract::ActivityRegistry;
using tesseract::ActivityState;

namespace
{
tesseract::ActivityEntry find(const ActivityRegistry& r, const std::string& name)
{
    for (auto& e : r.snapshot())
        if (e.name == name)
            return e;
    FAIL("entry not found: " << name);
    return {};
}
} // namespace

TEST_CASE("ActivityRegistry: scope marks running then idle", "[activity]")
{
    std::int64_t t = 100;
    ActivityRegistry r([&] { return t; });
    {
        auto s = r.begin("job", "G", "periodic");
        CHECK(find(r, "job").state == ActivityState::Running);
        t = 250;
    }
    auto e = find(r, "job");
    CHECK(e.state == ActivityState::Idle);
    CHECK(e.run_count == 1);
    CHECK(e.last_started_ms == 100);
    CHECK(e.last_finished_ms == 250);
    CHECK(e.kind == "periodic");
}

TEST_CASE("ActivityRegistry: concurrent scopes stack", "[activity]")
{
    ActivityRegistry r;
    auto a = r.begin("job", "G", "one-shot");
    {
        auto b = r.begin("job", "G", "one-shot");
    }
    CHECK(find(r, "job").state == ActivityState::Running);
    a = {};
    CHECK(find(r, "job").state == ActivityState::Idle);
    CHECK(find(r, "job").run_count == 2);
}

TEST_CASE("ActivityRegistry: error is sticky until next begin or clear", "[activity]")
{
    ActivityRegistry r;
    { auto s = r.begin("job", "G", "one-shot"); }
    r.set_error("job", "boom");
    CHECK(find(r, "job").state == ActivityState::Error);
    CHECK(find(r, "job").detail == "boom");
    { auto s = r.begin("job", "G", "one-shot"); }
    CHECK(find(r, "job").state == ActivityState::Idle);
    r.set_error("job", "again");
    r.set_error("job", std::nullopt);
    CHECK(find(r, "job").state == ActivityState::Idle);
}

TEST_CASE("ActivityRegistry: detail shown and snapshot sorted by group then name", "[activity]")
{
    ActivityRegistry r;
    { auto s = r.begin("b", "Z", "loop"); }
    { auto s = r.begin("a", "Z", "loop"); }
    { auto s = r.begin("c", "A", "loop"); }
    r.set_detail("a", "12 rooms");
    auto snap = r.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].name == "c");
    CHECK(snap[1].name == "a");
    CHECK(snap[1].detail == "12 rooms");
    CHECK(snap[2].name == "b");
}

TEST_CASE("ActivityRegistry: set_detail on unknown job is ignored", "[activity]")
{
    ActivityRegistry r;
    r.set_detail("nope", "x");
    r.set_error("nope", "x");
    CHECK(r.snapshot().empty());
}
