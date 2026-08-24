#pragma once

// Small formatting helpers shared by the views layer. Header-only so
// the shell layers (Qt6 / GTK4 / Win32 / macOS) can drop into either
// `tesseract::views` or their own translation units without growing the
// tesseract_tk static archive.

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace tesseract::views
{

// hh:mm in the local time zone, DST-aware. Empty when timestamp_ms is 0
// (unknown — e.g. a read receipt the homeserver sent without a `ts`).
inline std::string format_hhmm(std::uint64_t timestamp_ms)
{
    if (timestamp_ms == 0)
    {
        return {};
    }
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return std::string(buf);
}

// Human-readable byte count: "823 B" / "12 KB" / "4.3 MB" / "1.2 GB".
// Single-fraction-digit precision in the >=KB ranges; integer in the
// B/KB ranges.
inline std::string format_size(std::uint64_t bytes)
{
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
    {
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    }
    else if (bytes >= 1024ull * 1024)
    {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024));
    }
    else if (bytes >= 1024ull)
    {
        std::snprintf(buf, sizeof(buf), "%.0f KB",
                      static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

} // namespace tesseract::views
