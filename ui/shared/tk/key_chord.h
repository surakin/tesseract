#pragma once

// KeyChord — a platform-neutral description of one keyboard shortcut: the
// key plus its modifiers. One chord value serves three jobs, so a binding
// is written down once:
//   * matching a tk::KeyEvent (chord_matches), for shared key handlers;
//   * synthesizing the tk::KeyEvent a shell's native accelerator forwards
//     into the widget tree (to_key_event);
//   * labelling it for display ("Ctrl+Shift+F" / "⇧⌘F", chord_label).
// The app-level table of named shortcuts built from these lives in
// views/shortcut_registry.h; tk-level keys it also lists (the context-menu
// chords) are defined here so tk code can match them without depending on
// views/.

#include "tk/widget.h"

#include <string>
#include <vector>

namespace tk
{

enum class Platform
{
    Windows,
    Linux,
    MacOS,
};

// The platform this binary was built for.
constexpr Platform current_platform()
{
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

// Modifier bits for KeyChord::mods / KeyChord::optional.
enum Mod : unsigned
{
    ModNone = 0,
    // The platform's primary shortcut modifier: Ctrl on Windows/Linux, Cmd
    // on macOS. Matches either ctrl or meta.
    ModPrimary = 1u << 0,
    // Literal Control on every platform (Ctrl+Tab stays ⌃Tab on macOS,
    // since ⌘Tab belongs to the OS).
    ModCtrl = 1u << 1,
    ModShift = 1u << 2,
    ModAlt = 1u << 3,
    // Literal Cmd/Super. Only macOS-specific chords use it.
    ModMeta = 1u << 4,
};

struct KeyChord
{
    Key key = Key::Unknown;
    // The character for key == Key::Character. Letters are stored lowercase
    // and match case-insensitively.
    std::string text;
    // Modifiers that must be held.
    unsigned mods = ModNone;
    // Modifiers that may be held or not. Used for characters some layouts
    // only reach with Shift (`/`, `+`), and for keys whose handler has
    // always ignored Shift. Not shown in labels.
    unsigned optional = ModNone;
};

// Never true for a chord whose key is Unknown.
// True when `event` is `chord`: same key, every required modifier held and
// no modifier held that is neither required nor optional.
bool chord_matches(const KeyChord& chord, const KeyEvent& event);

// The KeyEvent a native accelerator for `chord` forwards into the widget
// tree on `platform` (ModPrimary becomes meta on macOS, ctrl elsewhere).
// Satisfies chord_matches(chord, to_key_event(chord, p)).
KeyEvent to_key_event(const KeyChord& chord, Platform platform = current_platform());

// Display label: "Ctrl+Shift+F" on Windows/Linux, "⇧⌘F" on macOS.
// Translated key names (Esc, Enter, Page Up, ...) go through tk::tr.
std::string chord_label(const KeyChord& chord, Platform platform = current_platform());

// The keyboard context-menu chords: the Menu key, and Shift+F10.
const std::vector<KeyChord>& context_menu_chords();

// A grid/list cell's secondary action (ListView::on_cell_context_requested,
// e.g. the emoji skin-tone menu): Shift+Enter.
const KeyChord& cell_secondary_action_chord();

} // namespace tk
