// GithubUpdateChecker / AurUpdateChecker scheduling semantics. The worker job
// is captured rather than run: running it would query the network.

#include <catch2/catch_test_macros.hpp>

#include "app/AurUpdateChecker.h"
#include "app/GithubUpdateChecker.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct UcQueue
{
    std::vector<std::function<void()>> jobs;
    std::function<void(std::function<void()>)> exec()
    {
        return [this](std::function<void()> fn) { jobs.push_back(std::move(fn)); };
    }
};
} // namespace

TEST_CASE("GithubUpdateChecker schedules exactly one check", "[update_checker]")
{
    tesseract::Client client;
    UcQueue async, ui;
    tesseract::GithubUpdateChecker c(client, async.exec(), ui.exec(), "o/r",
                                     "1.0.0");
    c.check_async([](std::string, std::string) {});
    c.check_async([](std::string, std::string) {});
    CHECK(async.jobs.size() == 1);
    CHECK(ui.jobs.empty());
}

TEST_CASE("GithubUpdateChecker skips a job whose checker is gone",
          "[update_checker]")
{
    tesseract::Client client;
    UcQueue async, ui;
    bool called = false;
    {
        tesseract::GithubUpdateChecker c(client, async.exec(), ui.exec(), "o/r",
                                         "1.0.0");
        c.check_async([&](std::string, std::string) { called = true; });
        REQUIRE(async.jobs.size() == 1);
    }
    async.jobs[0](); // would hit the network if it weren't skipped
    CHECK(ui.jobs.empty());
    CHECK_FALSE(called);
}

TEST_CASE("AurUpdateChecker schedules exactly one check", "[update_checker]")
{
    tesseract::Client client;
    UcQueue async, ui;
    tesseract::AurUpdateChecker c(client, async.exec(), ui.exec(), "pkg",
                                  "1.0.0");
    c.check_async([](std::string, std::string) {});
    c.check_async([](std::string, std::string) {});
    CHECK(async.jobs.size() == 1);
}

TEST_CASE("AurUpdateChecker skips a job whose checker is gone",
          "[update_checker]")
{
    tesseract::Client client;
    UcQueue async, ui;
    bool called = false;
    {
        tesseract::AurUpdateChecker c(client, async.exec(), ui.exec(), "pkg",
                                      "1.0.0");
        c.check_async([&](std::string, std::string) { called = true; });
        REQUIRE(async.jobs.size() == 1);
    }
    async.jobs[0]();
    CHECK(ui.jobs.empty());
    CHECK_FALSE(called);
}

TEST_CASE("update checks are gated by the build flag and the runtime switch",
          "[update_checker]")
{
    const bool was_disabled = tesseract::detail::update_checks_disabled;
    tesseract::disable_update_checks();
    CHECK_FALSE(tesseract::update_checks_enabled());
    // Restore for later tests in this process.
    tesseract::detail::update_checks_disabled = was_disabled;
}
