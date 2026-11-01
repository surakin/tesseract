#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace tk
{

// IANA name of this computer's timezone (e.g. "Europe/Berlin"); empty when it
// cannot be determined.
std::string system_timezone();

// UTC offset in minutes of an IANA zone at `at`; nullopt for an unknown zone.
std::optional<int> timezone_utc_offset_minutes(const std::string& iana,
                                               std::chrono::system_clock::time_point at);

// Compact signed offset: "+2h", "-5:30"; empty for zero.
std::string format_offset_diff(int minutes);

} // namespace tk
