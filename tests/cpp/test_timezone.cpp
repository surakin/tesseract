#include "platform/timezone.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

TEST_CASE("format_offset_diff: compact signed hours and minutes", "[timezone]")
{
    CHECK(tk::format_offset_diff(0).empty());
    CHECK(tk::format_offset_diff(120) == "+2h");
    CHECK(tk::format_offset_diff(-60) == "-1h");
    CHECK(tk::format_offset_diff(-330) == "-5:30");
    CHECK(tk::format_offset_diff(345) == "+5:45");
    CHECK(tk::format_offset_diff(15) == "+0:15");
}

TEST_CASE("timezone_utc_offset_minutes: known zones, DST and unknown ids", "[timezone]")
{
    using namespace std::chrono;
    const auto jan = sys_days{year{2026} / January / 15};
    const auto jul = sys_days{year{2026} / July / 15};

    CHECK(tk::timezone_utc_offset_minutes("Etc/UTC", jan) == 0);
    CHECK(tk::timezone_utc_offset_minutes("Asia/Kolkata", jan) == 330);
    CHECK(tk::timezone_utc_offset_minutes("Europe/Berlin", jan) == 60);
    CHECK(tk::timezone_utc_offset_minutes("Europe/Berlin", jul) == 120);
    CHECK_FALSE(tk::timezone_utc_offset_minutes("Not/AZone", jan).has_value());
}

TEST_CASE("system_timezone: resolves to a zone the offset lookup knows", "[timezone]")
{
    const std::string tz = tk::system_timezone();
    if (!tz.empty())
        CHECK(tk::timezone_utc_offset_minutes(tz, std::chrono::system_clock::now()).has_value());
}
