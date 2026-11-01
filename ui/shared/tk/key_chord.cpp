#include "tk/key_chord.h"

#include "tk/i18n.h"

#include <cctype>
#include <string_view>

namespace tk
{

namespace
{

std::string lower_ascii(std::string s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// ASCII case-insensitive equality, without copying (runs per key press).
bool equal_ignoring_ascii_case(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

std::string upper_ascii(std::string s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string key_name(const KeyChord& chord, Platform platform)
{
    const bool mac = platform == Platform::MacOS;
    switch (chord.key)
    {
    case Key::Escape:
        return mac ? "\xe2\x8e\x8b" : tr("Esc"); // ⎋
    case Key::Enter:
        return mac ? "\xe2\x86\xa9" : tr("Enter"); // ↩
    case Key::Space:
        return tr("Space");
    case Key::Tab:
    case Key::Backtab:
        return mac ? "\xe2\x87\xa5" : tr("Tab"); // ⇥
    case Key::Up:
        return "\xe2\x86\x91"; // ↑
    case Key::Down:
        return "\xe2\x86\x93"; // ↓
    case Key::Left:
        return "\xe2\x86\x90"; // ←
    case Key::Right:
        return "\xe2\x86\x92"; // →
    case Key::Home:
        return mac ? "\xe2\x86\x96" : tr("Home"); // ↖
    case Key::End:
        return mac ? "\xe2\x86\x98" : tr("End"); // ↘
    case Key::PageUp:
        return mac ? "\xe2\x87\x9e" : tr("Page Up"); // ⇞
    case Key::PageDown:
        return mac ? "\xe2\x87\x9f" : tr("Page Down"); // ⇟
    case Key::Backspace:
        return mac ? "\xe2\x8c\xab" : tr("Backspace"); // ⌫
    case Key::Delete:
        return mac ? "\xe2\x8c\xa6" : tr("Delete"); // ⌦
    // The Windows/Linux "Menu" (Application) key; macOS keyboards have none.
    case Key::Menu:
        return tr("Menu");
    case Key::F10:
        return "F10";
    case Key::F1:
        return "F1";
    case Key::Character:
        return upper_ascii(chord.text);
    case Key::Unknown:
        break;
    }
    return {};
}

} // namespace

bool chord_matches(const KeyChord& chord, const KeyEvent& event)
{
    bool shift = event.shift;
    if (chord.key == Key::Tab && event.key == Key::Backtab)
    {
        shift = true; // Shift+Tab arrives as Backtab from canvas key handling
    }
    else if (event.key != chord.key)
    {
        return false;
    }
    if (chord.key == Key::Character && !equal_ignoring_ascii_case(event.text, chord.text))
    {
        return false;
    }

    const unsigned allowed = chord.mods | chord.optional;
    const bool primary_ok = (allowed & ModPrimary) != 0;
    const bool ctrl_ok = primary_ok || (allowed & ModCtrl);
    const bool meta_ok = primary_ok || (allowed & ModMeta);
    if ((event.ctrl && !ctrl_ok) || (event.meta && !meta_ok) || (shift && !(allowed & ModShift)) ||
        (event.alt && !(allowed & ModAlt)))
    {
        return false;
    }

    if ((chord.mods & ModPrimary) && !event.ctrl && !event.meta)
    {
        return false;
    }
    if ((chord.mods & ModCtrl) && !event.ctrl)
    {
        return false;
    }
    if ((chord.mods & ModMeta) && !event.meta)
    {
        return false;
    }
    if ((chord.mods & ModShift) && !shift)
    {
        return false;
    }
    if ((chord.mods & ModAlt) && !event.alt)
    {
        return false;
    }
    return true;
}

KeyEvent to_key_event(const KeyChord& chord, Platform platform)
{
    KeyEvent event{};
    event.key = chord.key;
    if (chord.key == Key::Character)
    {
        event.text = lower_ascii(chord.text);
    }
    if (chord.mods & ModPrimary)
    {
        (platform == Platform::MacOS ? event.meta : event.ctrl) = true;
    }
    if (chord.mods & ModCtrl)
    {
        event.ctrl = true;
    }
    if (chord.mods & ModMeta)
    {
        event.meta = true;
    }
    event.shift = (chord.mods & ModShift) != 0;
    event.alt = (chord.mods & ModAlt) != 0;
    return event;
}

std::string chord_label(const KeyChord& chord, Platform platform)
{
    const unsigned m = chord.mods;
    if (platform == Platform::MacOS)
    {
        // Apple's modifier order: ⌃ ⌥ ⇧ ⌘, glyphs run together.
        std::string out;
        if (m & ModCtrl)
        {
            out += "\xe2\x8c\x83";
        }
        if (m & ModAlt)
        {
            out += "\xe2\x8c\xa5";
        }
        if (m & ModShift)
        {
            out += "\xe2\x87\xa7";
        }
        if (m & (ModPrimary | ModMeta))
        {
            out += "\xe2\x8c\x98";
        }
        return out + key_name(chord, platform);
    }
    std::string out;
    if (m & (ModPrimary | ModCtrl))
    {
        out += tr("Ctrl") + "+";
    }
    if (m & ModAlt)
    {
        out += tr("Alt") + "+";
    }
    if (m & ModShift)
    {
        out += tr("Shift") + "+";
    }
    if (m & ModMeta)
    {
        out += (platform == Platform::Windows ? tr("Win") : tr("Super")) + "+";
    }
    return out + key_name(chord, platform);
}

const std::vector<KeyChord>& context_menu_chords()
{
    static const std::vector<KeyChord> chords{
        // The Menu key opens the menu whatever else is held.
        {Key::Menu, {}, ModNone, ModCtrl | ModShift | ModAlt | ModMeta},
        {Key::F10, {}, ModShift, ModNone},
    };
    return chords;
}

const KeyChord& cell_secondary_action_chord()
{
    static const KeyChord chord{Key::Enter, {}, ModShift, ModNone};
    return chord;
}

} // namespace tk
