#include "shortcut_registry.h"

#include "tk/i18n.h"

#include <array>
#include <cassert>

namespace tesseract::views
{

namespace
{

using tk::Key;
using tk::KeyChord;
using G = ShortcutGroup;
using S = ShortcutScope;

KeyChord key(Key k, unsigned mods = tk::ModNone, unsigned optional = tk::ModNone)
{
    return {k, {}, mods, optional};
}

KeyChord chr(const char* text, unsigned mods = tk::ModNone,
             unsigned optional = tk::ModNone)
{
    return {Key::Character, text, mods, optional};
}

std::vector<ShortcutDef> build(tk::Platform platform)
{
    const bool mac = platform == tk::Platform::MacOS;
    constexpr unsigned P = tk::ModPrimary;
    constexpr unsigned Sh = tk::ModShift;

    // `/` needs Shift on some layouts (German, Nordic: Shift+7).
    std::vector<KeyChord> show_shortcuts{chr("/", P, Sh)};
    if (!mac)
        show_shortcuts.push_back(key(Key::F1));

    std::vector<ShortcutDef> v{
        // ── General ─────────────────────────────────────────────────────
        {ShortcutId::ShowShortcuts, G::General, S::Window,
         tk::N_("Show keyboard shortcuts"), show_shortcuts},
        {ShortcutId::QuickSwitcher, G::General, S::Application,
         tk::N_("Jump to a room"), {chr("k", P)}},
        {ShortcutId::FindInRoom, G::General, S::Application,
         tk::N_("Find in this room"), {chr("f", P)}},
        {ShortcutId::SearchMessages, G::General, S::Application,
         tk::N_("Search your messages"), {chr("f", P | Sh)}},
        {ShortcutId::RoomInfo, G::General, S::Window,
         tk::N_("Show room info"), {chr("i", P)}},
        {ShortcutId::Settings, G::General, S::Window,
         tk::N_("Open settings"), {chr(",", P)}},
        {ShortcutId::MoveFocus, G::General, S::Contextual,
         tk::N_("Move to the next / previous control"),
         {key(Key::Tab), key(Key::Tab, Sh)}},
        {ShortcutId::Activate, G::General, S::Contextual,
         tk::N_("Activate the focused control"),
         {key(Key::Enter), key(Key::Space)}},
        {ShortcutId::ContextMenu, G::General, S::Contextual,
         tk::N_("Open the menu for the focused item"), tk::context_menu_chords()},
        {ShortcutId::DismissOverlay, G::General, S::Contextual,
         tk::N_("Close the open dialog, panel or menu"), {key(Key::Escape)}},

        // ── Navigation ──────────────────────────────────────────────────
        {ShortcutId::HistoryBack, G::Navigation, S::Application,
         tk::N_("Go back to the previous room"),
         {mac ? chr("[", tk::ModMeta) : key(Key::Left, tk::ModAlt)}},
        {ShortcutId::HistoryForward, G::Navigation, S::Application,
         tk::N_("Go forward to the next room"),
         {mac ? chr("]", tk::ModMeta) : key(Key::Right, tk::ModAlt)}},
        // Literal Control on macOS too: ⌘Tab is the system app switcher.
        {ShortcutId::RecentRoomNext, G::Navigation, S::Application,
         tk::N_("Cycle through recent rooms"), {key(Key::Tab, tk::ModCtrl)}},
        {ShortcutId::RecentRoomPrev, G::Navigation, S::Application,
         tk::N_("Cycle through recent rooms backward"),
         {key(Key::Tab, tk::ModCtrl | Sh)}},
        {ShortcutId::ScrollPage, G::Navigation, S::Contextual,
         tk::N_("Scroll up / down a page"),
         {key(Key::PageUp), key(Key::PageDown)}},
        {ShortcutId::ScrollEnds, G::Navigation, S::Contextual,
         tk::N_("Scroll to the top / bottom"), {key(Key::Home), key(Key::End)}},

        // ── Timeline (while it has focus) ───────────────────────────────
        {ShortcutId::TimelineMove, G::Timeline, S::Contextual,
         tk::N_("Select the previous / next message"),
         {key(Key::Up), key(Key::Down)}},
        {ShortcutId::TimelinePage, G::Timeline, S::Contextual,
         tk::N_("Move a page of messages up / down"),
         {key(Key::PageUp), key(Key::PageDown)}},
        {ShortcutId::TimelineOldest, G::Timeline, S::Contextual,
         tk::N_("Select the oldest loaded message"), {key(Key::Home)}},
        {ShortcutId::TimelineNewest, G::Timeline, S::Contextual,
         tk::N_("Select the newest message"), {key(Key::End)}},
        {ShortcutId::TimelineFirstUnread, G::Timeline, S::Contextual,
         tk::N_("Jump to the first unread message"), {key(Key::PageUp, Sh)}},
        {ShortcutId::TimelineParts, G::Timeline, S::Contextual,
         tk::N_("Step through links and attachments in a message"),
         {key(Key::Left), key(Key::Right)}},
        {ShortcutId::TimelineActivate, G::Timeline, S::Contextual,
         tk::N_("Open the selected part or the message's actions"),
         {key(Key::Enter), key(Key::Space)}},
        {ShortcutId::CopyMessage, G::Timeline, S::Contextual,
         tk::N_("Copy the selected text or message"), {chr("c", P)}},
        {ShortcutId::TimelineExit, G::Timeline, S::Contextual,
         tk::N_("Go back to the message box"), {key(Key::Escape)}},

        // ── Composer ────────────────────────────────────────────────────
        {ShortcutId::ComposerSend, G::Composer, S::Contextual,
         tk::N_("Send the message"), {key(Key::Enter)}},
        {ShortcutId::ComposerNewLine, G::Composer, S::Contextual,
         tk::N_("Start a new line"), {key(Key::Enter, Sh)}},
        {ShortcutId::ComposerEditLast, G::Composer, S::Contextual,
         tk::N_("Edit your last message (in an empty message box)"),
         {key(Key::Up)}},

        // ── Pickers and lists ───────────────────────────────────────────
        {ShortcutId::PickerMove, G::Pickers, S::Contextual,
         tk::N_("Move through suggestions and results"),
         {key(Key::Up), key(Key::Down)}},
        {ShortcutId::PickerChoose, G::Pickers, S::Contextual,
         tk::N_("Choose the highlighted suggestion"),
         {key(Key::Enter), key(Key::Tab)}},
        {ShortcutId::SkinTone, G::Pickers, S::Contextual,
         tk::N_("Choose a skin tone for the selected emoji"), {tk::cell_secondary_action_chord()}},
        {ShortcutId::RemoveFromSpace, G::Pickers, S::Contextual,
         tk::N_("Remove the selected room from the space"), {key(Key::Delete)}},

        // ── Date picker ─────────────────────────────────────────────────
        {ShortcutId::DateMove, G::DatePicker, S::Contextual,
         tk::N_("Move by day / week"),
         {key(Key::Left), key(Key::Right), key(Key::Up), key(Key::Down)}},
        {ShortcutId::DateWeekEnds, G::DatePicker, S::Contextual,
         tk::N_("Go to the start / end of the week"),
         {key(Key::Home), key(Key::End)}},
        {ShortcutId::DatePrevMonth, G::DatePicker, S::Contextual,
         tk::N_("Previous month"), {key(Key::PageUp)}},
        {ShortcutId::DateNextMonth, G::DatePicker, S::Contextual,
         tk::N_("Next month"), {key(Key::PageDown)}},
        {ShortcutId::DatePrevYear, G::DatePicker, S::Contextual,
         tk::N_("Previous year"), {key(Key::PageUp, Sh)}},
        {ShortcutId::DateNextYear, G::DatePicker, S::Contextual,
         tk::N_("Next year"), {key(Key::PageDown, Sh)}},
        {ShortcutId::DateToday, G::DatePicker, S::Contextual,
         tk::N_("Go to today"), {chr("t", tk::ModNone, Sh)}},
        {ShortcutId::DatePick, G::DatePicker, S::Contextual,
         tk::N_("Pick the selected date"), {key(Key::Enter), key(Key::Space)}},

        // ── Media viewers ───────────────────────────────────────────────
        // `+` needs Shift on US layouts; Ctrl+Plus has always zoomed too.
        {ShortcutId::ImageZoomIn, G::MediaViewers, S::Contextual,
         tk::N_("Zoom in"),
         {chr("+", tk::ModNone, Sh | tk::ModCtrl), chr("=", tk::ModNone, Sh | tk::ModCtrl)}},
        {ShortcutId::ImageZoomOut, G::MediaViewers, S::Contextual,
         tk::N_("Zoom out"), {chr("-", tk::ModNone, Sh | tk::ModCtrl)}},
        {ShortcutId::ImageFit, G::MediaViewers, S::Contextual,
         tk::N_("Fit the image to the window"),
         {chr("0", tk::ModNone, Sh | tk::ModCtrl)}},
        {ShortcutId::ImagePan, G::MediaViewers, S::Contextual,
         tk::N_("Pan a zoomed image"),
         {key(Key::Left), key(Key::Right), key(Key::Up), key(Key::Down)}},
        {ShortcutId::VideoPlayPause, G::MediaViewers, S::Contextual,
         tk::N_("Play / pause media"),
         {key(Key::Space, tk::ModNone, Sh), chr("k", tk::ModNone, Sh)}},
        {ShortcutId::VideoSeekBack, G::MediaViewers, S::Contextual,
         tk::N_("Skip back 5 seconds"), {key(Key::Left, tk::ModNone, Sh)}},
        {ShortcutId::VideoSeekForward, G::MediaViewers, S::Contextual,
         tk::N_("Skip forward 5 seconds"), {key(Key::Right, tk::ModNone, Sh)}},
        {ShortcutId::VideoRestart, G::MediaViewers, S::Contextual,
         tk::N_("Restart the video"), {key(Key::Home, tk::ModNone, Sh)}},
        // A page gets the key first (video seeks on Left/Right, a zoomed image
        // pans), so the arrows only navigate when the page declines them.
        {ShortcutId::MediaPrev, G::MediaViewers, S::Contextual,
         tk::N_("Previous item in a gallery"),
         {key(Key::PageUp, tk::ModNone, Sh), key(Key::Left)}},
        {ShortcutId::MediaNext, G::MediaViewers, S::Contextual,
         tk::N_("Next item in a gallery"),
         {key(Key::PageDown, tk::ModNone, Sh), key(Key::Right)}},
    };
    return v;
}

// One platform's table plus an index by id, so matches() (called for most
// key presses) is a direct lookup rather than a scan.
struct Table
{
    explicit Table(tk::Platform platform) : defs(build(platform))
    {
        by_id.fill(nullptr);
        for (const auto& def : defs)
            by_id[static_cast<std::size_t>(def.id)] = &def;
        for ([[maybe_unused]] const ShortcutDef* def : by_id)
            assert(def && "ShortcutId missing from the registry");
    }
    std::vector<ShortcutDef> defs;
    std::array<const ShortcutDef*, kShortcutIdCount> by_id{};
};

const Table& table(tk::Platform platform)
{
    static const Table windows{tk::Platform::Windows};
    static const Table linux_{tk::Platform::Linux};
    static const Table macos{tk::Platform::MacOS};
    switch (platform)
    {
    case tk::Platform::Windows: return windows;
    case tk::Platform::MacOS: return macos;
    case tk::Platform::Linux: break;
    }
    return linux_;
}

} // namespace

const std::vector<ShortcutDef>& shortcuts(tk::Platform platform)
{
    return table(platform).defs;
}

const ShortcutDef& shortcut(ShortcutId id, tk::Platform platform)
{
    return *table(platform).by_id[static_cast<std::size_t>(id)];
}

bool matches(ShortcutId id, const tk::KeyEvent& event)
{
    for (const auto& chord : shortcut(id).chords)
        if (tk::chord_matches(chord, event))
            return true;
    return false;
}

std::string group_title(ShortcutGroup group)
{
    switch (group)
    {
    case G::General: return tk::tr("General");
    case G::Navigation: return tk::tr("Navigation");
    case G::Timeline: return tk::tr("Messages");
    case G::Composer: return tk::tr("Message box");
    case G::Pickers: return tk::tr("Pickers and lists");
    case G::DatePicker: return tk::tr("Date picker");
    case G::MediaViewers: return tk::tr("Media viewer");
    }
    return {};
}

std::string shortcut_label(ShortcutId id, tk::Platform platform)
{
    std::string out;
    for (const auto& chord : shortcut(id, platform).chords)
    {
        if (!out.empty())
            out += " / ";
        out += tk::chord_label(chord, platform);
    }
    return out;
}

} // namespace tesseract::views
