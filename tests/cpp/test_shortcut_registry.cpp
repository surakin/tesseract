#include <catch2/catch_test_macros.hpp>

#include "views/shortcut_registry.h"

#include <set>
#include <string>

// The shared keyboard-shortcut table: every shortcut listed once, the global
// ones bound natively by the shells from it.

using namespace tesseract::views;
using tk::Platform;

namespace
{
constexpr Platform kPlatforms[] = {Platform::Windows, Platform::Linux, Platform::MacOS};

std::string chord_key(const tk::KeyChord& c, Platform p)
{
    // The forwarded event, which is what MainAppWidget actually sees.
    const tk::KeyEvent e = tk::to_key_event(c, p);
    return std::to_string(static_cast<int>(e.key)) + e.text + (e.ctrl ? "C" : "") +
           (e.shift ? "S" : "") + (e.alt ? "A" : "") + (e.meta ? "M" : "");
}
} // namespace

TEST_CASE("shortcut registry: ids unique, entries complete", "[shortcuts]")
{
    for (Platform p : kPlatforms)
    {
        std::set<ShortcutId> ids;
        for (const auto& def : shortcuts(p))
        {
            CHECK(ids.insert(def.id).second);
            CHECK(def.description != nullptr);
            CHECK(std::string(def.description).size() > 0);
            CHECK_FALSE(def.chords.empty());
            CHECK_FALSE(group_title(def.group).empty());
        }
        // Every ShortcutId is in every platform's table.
        CHECK(ids.size() == kShortcutIdCount);
    }
}

TEST_CASE("shortcut registry: groups are contiguous", "[shortcuts]")
{
    // The overlay starts a new heading whenever the group changes, so a
    // group split in two would show twice.
    std::set<ShortcutGroup> seen;
    ShortcutGroup current = shortcuts().front().group;
    seen.insert(current);
    for (const auto& def : shortcuts())
    {
        if (def.group == current)
        {
            continue;
        }
        current = def.group;
        CHECK(seen.insert(current).second);
    }
}

TEST_CASE("shortcut registry: bound chords match their own forwarded event", "[shortcuts]")
{
    for (Platform p : kPlatforms)
    {
        for (const auto& def : shortcuts(p))
        {
            if (def.scope == ShortcutScope::Contextual)
            {
                continue;
            }
            for (const auto& chord : def.chords)
            {
                CHECK(tk::chord_matches(chord, tk::to_key_event(chord, p)));
            }
        }
    }
}

TEST_CASE("shortcut registry: no two bound shortcuts share a chord", "[shortcuts]")
{
    for (Platform p : kPlatforms)
    {
        std::set<std::string> seen;
        for (const auto& def : shortcuts(p))
        {
            if (def.scope == ShortcutScope::Contextual)
            {
                continue;
            }
            for (const auto& chord : def.chords)
            {
                CHECK(seen.insert(chord_key(chord, p)).second);
            }
        }
    }
}

TEST_CASE("shortcut registry: platform variants", "[shortcuts]")
{
    CHECK(shortcut_label(ShortcutId::HistoryBack, Platform::Linux) == "Alt+\xe2\x86\x90");
    CHECK(shortcut_label(ShortcutId::HistoryBack, Platform::MacOS) == "\xe2\x8c\x98[");
    CHECK(shortcut_label(ShortcutId::ShowShortcuts, Platform::Windows) == "Ctrl+/ / F1");
    // F1 is a media key on Mac keyboards.
    CHECK(shortcut_label(ShortcutId::ShowShortcuts, Platform::MacOS) == "\xe2\x8c\x98/");
    // Recent-room cycling stays on literal Control on macOS (⌘Tab is the
    // system app switcher).
    CHECK(shortcut_label(ShortcutId::RecentRoomNext, Platform::MacOS) ==
          "\xe2\x8c\x83\xe2\x87\xa5");
}

TEST_CASE("shortcut registry: matches() follows the chords", "[shortcuts]")
{
    tk::KeyEvent ctrl_k{};
    ctrl_k.key = tk::Key::Character;
    ctrl_k.text = "k";
    ctrl_k.ctrl = true;
    CHECK(matches(ShortcutId::QuickSwitcher, ctrl_k));
    CHECK_FALSE(matches(ShortcutId::RoomInfo, ctrl_k));

    tk::KeyEvent plus{};
    plus.key = tk::Key::Character;
    plus.text = "+";
    plus.shift = true; // US layouts
    CHECK(matches(ShortcutId::ImageZoomIn, plus));
    plus.alt = true;
    CHECK_FALSE(matches(ShortcutId::ImageZoomIn, plus));

    tk::KeyEvent pgup{tk::Key::PageUp};
    CHECK(matches(ShortcutId::DatePrevMonth, pgup));
    CHECK_FALSE(matches(ShortcutId::DatePrevYear, pgup));
    pgup.shift = true;
    CHECK(matches(ShortcutId::DatePrevYear, pgup));
}
