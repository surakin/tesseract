// presence_text.h: the shared presence formatting helpers used by the DM
// header subtitle (and available to member rows / profile panels).
//
// These are pure functions of (state, timestamp, flag) plus tk::tr/trf, so a
// silent formatting failure here is exactly the class of bug that shows as
// "no presence line anywhere" while every other test stays green.

#include <catch2/catch_test_macros.hpp>

#include "views/presence_text.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace
{

std::uint64_t now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace

TEST_CASE("format_relative_ago produces text for every magnitude", "[presence][format]")
{
    using tesseract::views::format_relative_ago;
    const std::uint64_t now = now_ms();

    // 0 means "unknown" — the caller hides the line rather than guess.
    CHECK(format_relative_ago(0).empty());

    // Every real magnitude must yield something non-empty. A regression here
    // (e.g. a broken trf format string) makes the whole presence line vanish.
    CHECK_FALSE(format_relative_ago(now - 5'000).empty());              // seconds
    CHECK_FALSE(format_relative_ago(now - 5ULL * 60'000).empty());      // minutes
    CHECK_FALSE(format_relative_ago(now - 3ULL * 3'600'000).empty());   // hours
    CHECK_FALSE(format_relative_ago(now - 2ULL * 86'400'000).empty());  // days
    CHECK_FALSE(format_relative_ago(now - 90ULL * 86'400'000).empty()); // months

    // A timestamp in the future (clock skew) must still not produce "" or a
    // negative age.
    CHECK_FALSE(format_relative_ago(now + 10ULL * 60'000).empty());
}

TEST_CASE("format_relative_ago uses the shared short-unit wording", "[presence][format]")
{
    using tesseract::views::format_relative_ago;
    const std::uint64_t now = now_ms();

    const std::string minutes = format_relative_ago(now - 5ULL * 60'000);
    // Same "{0}m" + "{0} ago" msgids the device list already uses.
    CHECK(minutes.find('m') != std::string::npos);
    CHECK(minutes.find("ago") != std::string::npos);

    const std::string hours = format_relative_ago(now - 3ULL * 3'600'000);
    CHECK(hours.find('h') != std::string::npos);
}

TEST_CASE("presence_line covers online, away and offline", "[presence][format]")
{
    using tesseract::views::presence_line;
    const std::uint64_t now = now_ms();

    // Online always has text, even with no timestamp.
    CHECK(presence_line(tesseract::PresenceState::Online, 0, false) == "Online");

    // "Active now" wins over the unavailable/offline timestamp text.
    CHECK(presence_line(tesseract::PresenceState::Offline, 0, true) == "Active now");

    // Away with and without a known timestamp.
    CHECK_FALSE(presence_line(tesseract::PresenceState::Unavailable, 0, false).empty());
    CHECK(presence_line(tesseract::PresenceState::Unavailable, 0, false) == "Away");
    const std::string away =
        presence_line(tesseract::PresenceState::Unavailable, now - 5ULL * 60'000, false);
    CHECK(away.find("Away") != std::string::npos);

    // Offline: "Last seen …" when a timestamp is known, empty otherwise.
    const std::string seen =
        presence_line(tesseract::PresenceState::Offline, now - 5ULL * 60'000, false);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.find("Last seen") != std::string::npos);
    CHECK(presence_line(tesseract::PresenceState::Offline, 0, false).empty());
}

TEST_CASE("format_last_seen is empty only when the timestamp is unknown", "[presence][format]")
{
    using tesseract::views::format_last_seen;
    CHECK(format_last_seen(0).empty());
    CHECK_FALSE(format_last_seen(now_ms() - 60'000).empty());
}
