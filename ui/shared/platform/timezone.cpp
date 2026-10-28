#include "timezone.h"

#include <cstdlib>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace tk
{

#if defined(__APPLE__)

namespace
{

std::string cf_to_string(CFStringRef s)
{
    if (!s)
        return {};
    char buf[256];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? buf : std::string{};
}

} // namespace

std::string system_timezone()
{
    CFTimeZoneRef tz = CFTimeZoneCopySystem();
    if (!tz)
        return {};
    std::string name = cf_to_string(CFTimeZoneGetName(tz));
    CFRelease(tz);
    return name;
}

std::optional<int> timezone_utc_offset_minutes(const std::string& iana,
                                               std::chrono::system_clock::time_point at)
{
    CFStringRef name = CFStringCreateWithCString(nullptr, iana.c_str(), kCFStringEncodingUTF8);
    if (!name)
        return std::nullopt;
    CFTimeZoneRef tz = CFTimeZoneCreateWithName(nullptr, name, false);
    CFRelease(name);
    if (!tz)
        return std::nullopt;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch());
    const CFAbsoluteTime abs = static_cast<CFAbsoluteTime>(secs.count()) - kCFAbsoluteTimeIntervalSince1970;
    const int offset = static_cast<int>(CFTimeZoneGetSecondsFromGMT(tz, abs) / 60);
    CFRelease(tz);
    return offset;
}

#else

std::string system_timezone()
{
    try
    {
        const std::chrono::time_zone* z = std::chrono::current_zone();
        return z ? std::string(z->name()) : std::string{};
    }
    catch (...)
    {
        return {};
    }
}

std::optional<int> timezone_utc_offset_minutes(const std::string& iana,
                                               std::chrono::system_clock::time_point at)
{
    try
    {
        const std::chrono::time_zone* z = std::chrono::locate_zone(iana);
        const auto info = z->get_info(std::chrono::time_point_cast<std::chrono::seconds>(at));
        return static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(info.offset).count());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

#endif

std::string format_offset_diff(int minutes)
{
    if (minutes == 0)
        return {};
    std::string out = minutes < 0 ? "-" : "+";
    const int abs_min = std::abs(minutes);
    out += std::to_string(abs_min / 60);
    if (const int rem = abs_min % 60)
    {
        out += ':';
        if (rem < 10)
            out += '0';
        out += std::to_string(rem);
    }
    else
    {
        out += 'h';
    }
    return out;
}

} // namespace tk
