#include <catch2/catch_test_macros.hpp>

#include "tk/key_chord.h"

// tk::KeyChord: matching, the KeyEvent a native accelerator forwards, and
// per-platform labels.

using namespace tk;

namespace
{
KeyEvent chr(const char* text, bool ctrl = false, bool shift = false, bool alt = false,
             bool meta = false)
{
    KeyEvent e{};
    e.key = Key::Character;
    e.text = text;
    e.ctrl = ctrl;
    e.shift = shift;
    e.alt = alt;
    e.meta = meta;
    return e;
}
} // namespace

TEST_CASE("KeyChord matches exact modifiers only", "[tk][key_chord]")
{
    const KeyChord ctrl_k{Key::Character, "k", ModPrimary};
    CHECK(chord_matches(ctrl_k, chr("k", true)));
    CHECK(chord_matches(ctrl_k, chr("K", true)));                      // case-insensitive
    CHECK(chord_matches(ctrl_k, chr("k", false, false, false, true))); // Cmd
    CHECK_FALSE(chord_matches(ctrl_k, chr("k")));                      // no modifier
    CHECK_FALSE(chord_matches(ctrl_k, chr("k", true, true)));          // extra Shift
    CHECK_FALSE(chord_matches(ctrl_k, chr("k", true, false, true)));   // extra Alt
    CHECK_FALSE(chord_matches(ctrl_k, chr("j", true)));

    const KeyChord ctrl_tab{Key::Tab, {}, ModCtrl};
    KeyEvent tab{Key::Tab};
    tab.ctrl = true;
    CHECK(chord_matches(ctrl_tab, tab));
    tab.ctrl = false;
    tab.meta = true; // literal Ctrl: Cmd doesn't stand in for it
    CHECK_FALSE(chord_matches(ctrl_tab, tab));
}

TEST_CASE("KeyChord optional modifiers may be held or not", "[tk][key_chord]")
{
    const KeyChord slash{Key::Character, "/", ModPrimary, ModShift};
    CHECK(chord_matches(slash, chr("/", true)));
    CHECK(chord_matches(slash, chr("/", true, true))); // Shift+7 layouts
    CHECK_FALSE(chord_matches(slash, chr("/")));       // Primary still required
}

TEST_CASE("KeyChord Shift+Tab matches a Backtab event", "[tk][key_chord]")
{
    const KeyChord prev{Key::Tab, {}, ModCtrl | ModShift};
    KeyEvent backtab{Key::Backtab};
    backtab.ctrl = true;
    CHECK(chord_matches(prev, backtab));
    // ...but a plain Ctrl+Tab chord doesn't take one.
    CHECK_FALSE(chord_matches(KeyChord{Key::Tab, {}, ModCtrl}, backtab));
}

TEST_CASE("to_key_event round-trips and resolves Primary per platform", "[tk][key_chord]")
{
    const KeyChord search{Key::Character, "f", ModPrimary | ModShift};
    const KeyEvent lin = to_key_event(search, Platform::Linux);
    CHECK(lin.ctrl);
    CHECK_FALSE(lin.meta);
    CHECK(lin.shift);
    CHECK(lin.text == "f");
    const KeyEvent mac = to_key_event(search, Platform::MacOS);
    CHECK(mac.meta);
    CHECK_FALSE(mac.ctrl);
    for (Platform p : {Platform::Windows, Platform::Linux, Platform::MacOS})
    {
        CHECK(chord_matches(search, to_key_event(search, p)));
    }
}

TEST_CASE("chord_label spells chords per platform", "[tk][key_chord]")
{
    const KeyChord search{Key::Character, "f", ModPrimary | ModShift};
    CHECK(chord_label(search, Platform::Linux) == "Ctrl+Shift+F");
    CHECK(chord_label(search, Platform::Windows) == "Ctrl+Shift+F");
    CHECK(chord_label(search, Platform::MacOS) == "\xe2\x87\xa7\xe2\x8c\x98"
                                                  "F"); // ⇧⌘F
    CHECK(chord_label(KeyChord{Key::Left, {}, ModAlt}, Platform::Linux) == "Alt+\xe2\x86\x90");
    CHECK(chord_label(KeyChord{Key::F1}, Platform::Linux) == "F1");
    CHECK(chord_label(KeyChord{Key::PageUp}, Platform::Linux) == "Page Up");
    // Optional modifiers aren't shown.
    CHECK(chord_label(KeyChord{Key::Character, "/", ModPrimary, ModShift}, Platform::Linux) ==
          "Ctrl+/");
}

TEST_CASE("context-menu chords: Menu with anything, Shift+F10 exactly", "[tk][key_chord]")
{
    auto any = [](const KeyEvent& e)
    {
        for (const auto& c : context_menu_chords())
        {
            if (chord_matches(c, e))
            {
                return true;
            }
        }
        return false;
    };
    CHECK(any(KeyEvent{Key::Menu}));
    KeyEvent menu_shift{Key::Menu};
    menu_shift.shift = true;
    CHECK(any(menu_shift));
    KeyEvent f10{Key::F10};
    CHECK_FALSE(any(f10));
    f10.shift = true;
    CHECK(any(f10));
    f10.ctrl = true;
    CHECK_FALSE(any(f10));
}
