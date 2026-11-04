#pragma once

#include <atomic>

namespace tk
{

/// Whether the text caret blinks. Screenshot mode turns it off so a capture
/// never lands on the "off" half of a blink, which made reruns differ.
inline std::atomic<bool>& caret_blink_enabled_flag()
{
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline bool caret_blink_enabled()
{
    return caret_blink_enabled_flag().load(std::memory_order_relaxed);
}

inline void set_caret_blink_enabled(bool enabled)
{
    caret_blink_enabled_flag().store(enabled, std::memory_order_relaxed);
}

/// Whether the canvas-owned caret is drawn: the backend's blink phase while
/// blinking, otherwise solid for as long as the widget has keyboard focus.
inline bool caret_drawn(bool backend_blink_visible, bool widget_has_focus)
{
    return caret_blink_enabled() ? backend_blink_visible : widget_has_focus;
}

} // namespace tk
