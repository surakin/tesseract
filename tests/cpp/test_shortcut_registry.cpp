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
            CHECK(!std::string(def.description).empty());
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

TEST_CASE("shortcut registry: the World group lists the walking keys", "[shortcuts]")
{
    for (ShortcutId id :
         {ShortcutId::WorldForward, ShortcutId::WorldBack, ShortcutId::WorldStrafeLeft,
          ShortcutId::WorldStrafeRight, ShortcutId::WorldTurnLeft, ShortcutId::WorldTurnRight,
          ShortcutId::WorldLook, ShortcutId::WorldBar})
    {
        const ShortcutDef& def = shortcut(id);
        CHECK(def.group == ShortcutGroup::World);
        CHECK(def.scope == ShortcutScope::Contextual);
    }
    CHECK(shortcut_label(ShortcutId::WorldForward, Platform::Linux) == "W / \xe2\x86\x91");
    CHECK(shortcut_label(ShortcutId::WorldBack, Platform::Linux) == "S / \xe2\x86\x93");
    CHECK(shortcut_label(ShortcutId::WorldStrafeLeft, Platform::Linux) == "A");
    CHECK(shortcut_label(ShortcutId::WorldStrafeRight, Platform::Linux) == "D");
    CHECK(shortcut_label(ShortcutId::WorldTurnLeft, Platform::Linux) == "\xe2\x86\x90");
    CHECK(shortcut_label(ShortcutId::WorldTurnRight, Platform::Linux) == "\xe2\x86\x92");
    CHECK(shortcut_label(ShortcutId::WorldLook, Platform::Linux) == "Drag");
    CHECK(shortcut_label(ShortcutId::WorldBar, Platform::Linux) == "B");
    CHECK_FALSE(group_title(ShortcutGroup::World).empty());
}

TEST_CASE("shortcut registry: overlay-only entries match no key press", "[shortcuts]")
{
    std::vector<tk::KeyEvent> events;
    tk::KeyEvent plain;
    plain.key = tk::Key::Character;
    plain.text = "x";
    events.push_back(plain);
    tk::KeyEvent lone_shift; // a modifier press with no key of its own
    lone_shift.shift = true;
    events.push_back(lone_shift);
    events.push_back(tk::KeyEvent{}); // an unnamed key, no modifiers
    tk::KeyEvent arrow;
    arrow.key = tk::Key::Left;
    events.push_back(arrow);
    for (const tk::KeyEvent& e : events)
    {
        CHECK_FALSE(matches(ShortcutId::WorldLook, e));
    }
    tk::KeyChord unknown;
    CHECK_FALSE(tk::chord_matches(unknown, tk::KeyEvent{}));
}
