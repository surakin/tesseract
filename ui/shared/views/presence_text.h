#pragma once

// Shared formatting for a user's presence, mirroring FluffyChat's
// "Currently active" / "Last active: …" line. Header-only so both the views
// and the shell layers can use it without growing tesseract_tk.
//
// Conventions (see CLAUDE.md and the Matrix spec):
//   • Online      → "Online" (no timestamp shown: being online right now is
//                   more precise than a stale "last active" value).
//   • Unavailable → "Away" plus, when known, "last seen …".
//   • Offline     → "Last seen …" when the server reported an activity time,
//                   otherwise an empty string (unknown presence — the caller
//                   hides the line rather than claim "Offline").
// The server's `last_active_ago` is only a lower bound on accuracy (fuzzy
// across devices and idle timers), and the poller refreshes it once a minute,
// so the minute/hour/day granularity below is the honest one to show.

#include "format.h"

#include "tk/i18n.h"

#include <tesseract/types.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>

namespace tesseract::views
{

// "just now" / "5m ago" / "3h ago" / "2d ago" / "3mo ago".
// Reuses the exact msgids DevicesSection's device "last seen" text uses
// ("{0}m", "{0}h", "{0}d", "{0}mo", "{0} ago", "just now").
inline std::string format_relative_ago(std::uint64_t ts_ms)
{
    if (ts_ms == 0)
    {
        return {};
    }
    using namespace std::chrono;
    const auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if (static_cast<std::int64_t>(ts_ms) > now + 60'000)
    {
        return std::string(tk::tr("just now")); // clock skew: no negative age
    }
    const auto delta_ms =
        std::cmp_greater(now, ts_ms) ? (now - static_cast<std::int64_t>(ts_ms)) : 0;
    const auto secs = delta_ms / 1000;
    if (secs < 60)
    {
        return std::string(tk::tr("just now"));
    }
    std::string span;
    if (secs < 3600)
    {
        span = std::string(tk::trf(tk::tr("{0}m"), {std::to_string(secs / 60)}));
    }
    else if (secs < 86400)
    {
        span = std::string(tk::trf(tk::tr("{0}h"), {std::to_string(secs / 3600)}));
    }
    else if (secs < 86400 * 30)
    {
        span = std::string(tk::trf(tk::tr("{0}d"), {std::to_string(secs / 86400)}));
    }
    else
    {
        span = std::string(tk::trf(tk::tr("{0}mo"), {std::to_string(secs / (86400 * 30))}));
    }
    return std::string(tk::trf(tk::tr("{0} ago"), {span}));
}

// The full presence line for a DM header / member row / profile field.
// Returns an empty string when nothing reliable can be shown.
inline std::string format_last_seen(std::uint64_t last_active_ts)
{
    const std::string ago = format_relative_ago(last_active_ts);
    if (ago.empty())
    {
        return {};
    }
    return std::string(tk::trf(tk::tr("Last seen {0}"), {ago}));
}

// A compact one-line presence summary for a header subtitle. `state` is the
// C++ PresenceState; `currently_active` is the server's own flag (when true
// we say "Active now" even for Unavailable-with-a-fresh-timestamp).
inline std::string presence_line(tesseract::PresenceState state, std::uint64_t last_active_ts,
                                 bool currently_active)
{
    if (state == tesseract::PresenceState::Online)
    {
        return std::string(tk::tr("Online"));
    }
    if (currently_active)
    {
        return std::string(tk::tr("Active now"));
    }
    if (state == tesseract::PresenceState::Unavailable)
    {
        const std::string seen = format_relative_ago(last_active_ts);
        return seen.empty() ? std::string(tk::tr("Away"))
                            : std::string(tk::trf(tk::tr("Away, last seen {0}"), {seen}));
    }
    return format_last_seen(last_active_ts);
}

} // namespace tesseract::views
