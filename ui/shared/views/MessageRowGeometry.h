#pragma once

// Pure horizontal-geometry math for the timeline's optional bubble layout
// (Settings::message_layout == Bubbles). Deliberately free of any tk/canvas
// dependency so it can be unit-tested in isolation (see
// tests/cpp/test_message_row_renderer.cpp). BubbleRowRenderer in
// MessageListView.cpp is the only production consumer.

#include <algorithm>

#include "tesseract/visual.h"

namespace tesseract::views::msgbubble
{

// The first five mirror the anon-namespace values in MessageListView.cpp
// (kMsgListPadX / kMsgListAvatarSize / …); both are derived from the same
// tesseract::visual constants, so they cannot drift.
inline constexpr float kEdgePadX     = static_cast<float>(tesseract::visual::kSpaceMD);       // 12
inline constexpr float kAvatarSize   = static_cast<float>(tesseract::visual::kMsgAvatarSize); // 32
inline constexpr float kAvatarGap    = static_cast<float>(tesseract::visual::kMsgAvatarGap);  // 8
inline constexpr float kMaxBubbleW   = static_cast<float>(tesseract::visual::kMsgMaxWidth);   // 520
inline constexpr float kBubbleRadius = tesseract::visual::kRadiusMD;                          // 8

inline constexpr float kBubblePadX   = 10.0f; // bubble inner horizontal padding
inline constexpr float kBubblePadY   = 6.0f;  // bubble inner vertical padding
inline constexpr float kFurnitureGap = 6.0f;  // gap between an own bubble and its left-side furniture
inline constexpr float kBubbleMinW   = 48.0f; // floor for a bubble's content width
inline constexpr float kQuoteMinW    = 220.0f; // reply-quote card readability floor

// Width the message body is shaped/measured at for a given row width. Does
// not depend on the body's actual content width (only known post-measure).
inline float shaping_width(float row_w, bool is_own)
{
    const float avail = row_w - 2.0f * kEdgePadX
                        - (is_own ? 0.0f : (kAvatarSize + kAvatarGap));
    const float outer = std::clamp(avail, kBubbleMinW, kMaxBubbleW);
    return std::max(kBubbleMinW, outer - 2.0f * kBubblePadX);
}

// Resolved horizontal geometry for one message row. All coordinates are
// row-local (0 == the row's left edge); the caller adds bounds.x.
struct Box
{
    float content_x = 0.0f;         // left edge of the body column
    float content_w = 0.0f;         // body column width (== bubble_w - 2*kBubblePadX)
    float bubble_x = 0.0f;          // left edge of the bubble fill
    float bubble_w = 0.0f;          // bubble fill width
    float header_h = 0.0f;          // reserved band above the body (avatar/sender)
    float furniture_right_x = 0.0f; // right anchor for hover pill / receipts / pending
    float chip_x = 0.0f;            // left edge of the reaction strip + thread chip
    bool draw_avatar = false;
    bool draw_sender = false;
    bool furniture_center_in_row = false; // vertical anchor hint for the hover pill
    bool reserve_receipt_width = false;   // subtract the receipt cluster width from the body
};

// `natural_w` is the body's measured intrinsic width (longest wrapped line
// or media width). Pass <= 0 when it is not yet known — the bubble then
// simply fills the available width.
inline Box layout(float row_w, bool is_own, bool is_cont, float natural_w)
{
    Box b;
    const float cap = shaping_width(row_w, is_own);
    const float nat = natural_w > 0.0f ? natural_w : cap;
    b.content_w = std::clamp(nat, kBubbleMinW, cap);
    b.bubble_w = b.content_w + 2.0f * kBubblePadX;

    if (is_own)
    {
        b.bubble_x = std::max(kEdgePadX, row_w - kEdgePadX - b.bubble_w);
        b.content_x = b.bubble_x + kBubblePadX;
        b.header_h = 0.0f;
        b.draw_avatar = false;
        b.draw_sender = false;
        b.furniture_right_x = b.bubble_x - kFurnitureGap;
        b.furniture_center_in_row = true;
        b.reserve_receipt_width = false;
    }
    else
    {
        b.bubble_x = kEdgePadX + kAvatarSize + kAvatarGap; // 52
        b.content_x = b.bubble_x + kBubblePadX;
        b.header_h = is_cont ? 0.0f : kAvatarSize;
        b.draw_avatar = !is_cont;
        b.draw_sender = !is_cont;
        b.furniture_right_x = row_w - kEdgePadX;
        b.furniture_center_in_row = is_cont;
        b.reserve_receipt_width = true;
    }
    b.chip_x = b.content_x;
    return b;
}

} // namespace tesseract::views::msgbubble
