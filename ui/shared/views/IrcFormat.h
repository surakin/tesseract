#pragma once

// Pure formatting helpers for the timeline's optional IRC layout
// (Settings::message_layout == Irc). Kept free of any MessageListView /
// widget dependency so it can be unit-tested in isolation (see
// tests/cpp/test_message_row_irc.cpp). IrcRowRenderer in
// MessageListView.cpp is the production consumer.
//
// The look is mIRC's: every line is a flat, left-aligned, monospaced
//   [HH:MM] <nick> message
// with the nick coloured from the classic 16-colour mIRC palette and
// wrapped continuation lines flush to the left edge.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "tk/canvas.h" // tk::Color
#include "tk/theme.h"  // tk::ThemeMode

namespace tesseract::views::msgirc
{

// ── Geometry ──────────────────────────────────────────────────────────────
// One flat body column. No avatar gutter, no bubble.
inline constexpr float kIrcPadX     = 12.0f; // list edge → text column
inline constexpr float kIrcRowGapY  = 3.0f;  // vertical breathing room per row
inline constexpr float kIrcPrefixGap = 2.0f; // standalone prefix line → body

// ── mIRC 16-colour palette (codes 0-15) ───────────────────────────────────
// Reference values, mIRC's canonical hexes. Not all are legible as nick
// colours on a text background, so nick_color() draws from the curated
// per-theme subsets below rather than this whole table.
inline constexpr tk::Color kMirc16[16] = {
    tk::Color::rgb(0xFFFFFF), // 0  white
    tk::Color::rgb(0x000000), // 1  black
    tk::Color::rgb(0x00007F), // 2  blue (navy)
    tk::Color::rgb(0x009300), // 3  green
    tk::Color::rgb(0xFF0000), // 4  red
    tk::Color::rgb(0x7F0000), // 5  brown (maroon)
    tk::Color::rgb(0x9C009C), // 6  purple
    tk::Color::rgb(0xFC7F00), // 7  orange
    tk::Color::rgb(0xFFFF00), // 8  yellow
    tk::Color::rgb(0x00FC00), // 9  light green
    tk::Color::rgb(0x009393), // 10 teal (cyan)
    tk::Color::rgb(0x00FFFF), // 11 light cyan
    tk::Color::rgb(0x0000FC), // 12 light blue (royal)
    tk::Color::rgb(0xFF00FF), // 13 pink (light magenta)
    tk::Color::rgb(0x7F7F7F), // 14 grey
    tk::Color::rgb(0xD2D2D2), // 15 light grey
};

// Legible nick colours on a light background — darker, saturated hues
// drawn from the mIRC table (navy/green/red/maroon/purple/orange/teal/
// royal-blue/magenta/olive/steel/rust).
inline constexpr tk::Color kNickLight[] = {
    tk::Color::rgb(0x00007F), tk::Color::rgb(0x007000),
    tk::Color::rgb(0xC80000), tk::Color::rgb(0x7F0000),
    tk::Color::rgb(0x9C009C), tk::Color::rgb(0xB35A00),
    tk::Color::rgb(0x008080), tk::Color::rgb(0x0000CD),
    tk::Color::rgb(0xB000B0), tk::Color::rgb(0x5F6E00),
    tk::Color::rgb(0x3A5FCD), tk::Color::rgb(0xA0522D),
};

// Legible nick colours on a dark background — the bright end of the mIRC
// table (periwinkle/green/red/orange/yellow/mint/cyan/sky/pink/lime/
// aqua/salmon).
inline constexpr tk::Color kNickDark[] = {
    tk::Color::rgb(0x8C8CFF), tk::Color::rgb(0x5CD65C),
    tk::Color::rgb(0xFF6B6B), tk::Color::rgb(0xFF9E5C),
    tk::Color::rgb(0xE8E85C), tk::Color::rgb(0x7BE0A8),
    tk::Color::rgb(0x5CD6D6), tk::Color::rgb(0x6FB8FF),
    tk::Color::rgb(0xFF87D1), tk::Color::rgb(0xB6E05C),
    tk::Color::rgb(0x5CE0C4), tk::Color::rgb(0xFFA79E),
};

// Deterministic nick → colour. Keyed on the canonical Matrix id so a
// display-name change never moves the colour.
inline tk::Color nick_color(std::string_view user_id, tk::ThemeMode mode)
{
    const std::size_t h = std::hash<std::string_view>{}(user_id);
    if (mode == tk::ThemeMode::Dark)
        return kNickDark[h % (sizeof(kNickDark) / sizeof(kNickDark[0]))];
    return kNickLight[h % (sizeof(kNickLight) / sizeof(kNickLight[0]))];
}

// ── Line prefix ───────────────────────────────────────────────────────────
enum class PrefixKind
{
    Message, // "<nick> "
    Action,  // "* nick "   (m.emote / /me)
    Notice,  // "-nick- "   (m.notice)
};

// "[12:34] " — empty when hhmm is empty (unknown timestamp).
inline std::string timestamp_part(std::string_view hhmm)
{
    if (hhmm.empty())
        return {};
    std::string s;
    s.reserve(hhmm.size() + 3);
    s += '[';
    s += hhmm;
    s += "] ";
    return s;
}

// "<nick> " / "* nick " / "-nick- "
inline std::string nick_part(std::string_view nick, PrefixKind kind)
{
    std::string s;
    switch (kind)
    {
    case PrefixKind::Message:
        s.reserve(nick.size() + 3);
        s += '<';
        s += nick;
        s += "> ";
        break;
    case PrefixKind::Action:
        s.reserve(nick.size() + 3);
        s += "* ";
        s += nick;
        s += ' ';
        break;
    case PrefixKind::Notice:
        s.reserve(nick.size() + 3);
        s += '-';
        s += nick;
        s += "- ";
        break;
    }
    return s;
}

// Full plain prefix, e.g. "[12:34] <alice> ".
inline std::string line_prefix(std::string_view hhmm, std::string_view nick,
                               PrefixKind kind)
{
    return timestamp_part(hhmm) + nick_part(nick, kind);
}

} // namespace tesseract::views::msgirc
