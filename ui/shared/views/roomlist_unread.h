#pragma once

// Pure decision for how a room-list row's unread state should be shown. Kept
// header-only and dependency-free so it can be unit-tested without constructing
// the canvas-backed RoomListView. Used by RoomListView::paint_room (per-row
// bold + indicator) and the collapsed section-header rendering.

#include <cstdint>

namespace tesseract::views
{

enum class UnreadStyle
{
    None,    ///< read (or muted with nothing notifying) → regular weight, no badge
    Dot,     ///< unread messages but no notification → bold name + small neutral dot
    Count,   ///< notifying → bold name + neutral count pill
    Mention, ///< mention/highlight → bold name + accent count pill
};

/// Decide the unread treatment for a room from its counts + mute state.
///
/// Precedence: a mention wins over a plain notification, which wins over a
/// quiet unread. Muted rooms never show the quiet-unread dot (the user silenced
/// them on purpose) — but a mute that still produced a highlight/notification
/// count is honoured, since that is an explicit signal.
inline UnreadStyle unread_style_for(std::uint64_t notification_count,
                                    std::uint64_t highlight_count,
                                    std::uint64_t unread_count, bool muted)
{
    if (highlight_count > 0)
        return UnreadStyle::Mention;
    if (notification_count > 0)
        return UnreadStyle::Count;
    if (unread_count > 0 && !muted)
        return UnreadStyle::Dot;
    return UnreadStyle::None;
}

} // namespace tesseract::views
