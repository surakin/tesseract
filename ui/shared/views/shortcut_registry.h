#pragma once

// The app's keyboard shortcuts, in one table. Every shortcut a user can
// press is listed here once, with its description and key chords; this is
// what the Keyboard Shortcuts overlay shows.
//
// Global entries (scope Application / Window) are also *bound* from here:
// each shell turns them into native accelerators (QShortcut, GtkShortcut,
// a Win32 ACCEL table, NSMenuItem key equivalents) that forward
// tk::to_key_event(chord) to MainAppWidget::dispatch_key_down, and
// MainAppWidget matches them with matches(). Contextual entries are handled
// by the widget that owns them; the ones with distinctive keys match through
// matches() too, while generic navigation keys (arrows, Home/End, Escape,
// Tab) are only described here.

#include "tk/key_chord.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tesseract::views
{

enum class ShortcutId
{
    // General
    ShowShortcuts,
    QuickSwitcher,
    FindInRoom,
    SearchMessages,
    RoomInfo,
    Settings,
    MoveFocus,
    Activate,
    ContextMenu,
    DismissOverlay,
    // Navigation
    HistoryBack,
    HistoryForward,
    RecentRoomNext,
    RecentRoomPrev,
    ScrollPage,
    ScrollEnds,
    // Timeline
    TimelineMove,
    TimelinePage,
    TimelineOldest,
    TimelineNewest,
    TimelineFirstUnread,
    TimelineParts,
    TimelineActivate,
    CopyMessage,
    TimelineExit,
    // Composer
    ComposerSend,
    ComposerNewLine,
    ComposerEditLast,
    // Pickers and lists
    PickerMove,
    PickerChoose,
    SkinTone,
    RemoveFromSpace,
    // Date picker
    DateMove,
    DateWeekEnds,
    DatePrevMonth,
    DateNextMonth,
    DatePrevYear,
    DateNextYear,
    DateToday,
    DatePick,
    // Media viewers
    ImageZoomIn,
    ImageZoomOut,
    ImageFit,
    ImagePan,
    VideoPlayPause,
    VideoSeekBack,
    VideoSeekForward,
    VideoRestart,
    MediaPrev,
    MediaNext,
};

// Number of ShortcutId values (MediaNext is the last).
inline constexpr std::size_t kShortcutIdCount =
    static_cast<std::size_t>(ShortcutId::MediaNext) + 1;

enum class ShortcutGroup
{
    General,
    Navigation,
    Timeline,
    Composer,
    Pickers,
    DatePicker,
    MediaViewers,
};

enum class ShortcutScope
{
    // Native accelerator active in every window, even while a native text
    // control has focus.
    Application,
    // Native accelerator for the main window only (it acts on that
    // window's room, so a pop-out room window must not trigger it).
    Window,
    // No native binding; the widget that owns the key handles it.
    Contextual,
};

struct ShortcutDef
{
    ShortcutId id;
    ShortcutGroup group;
    ShortcutScope scope;
    // Untranslated (tk::N_-marked); show through tk::tr.
    const char* description;
    std::vector<tk::KeyChord> chords;
};

// All shortcuts for `platform`, in display order (grouped). Platform
// differences are resolved here: history is ⌘[ / ⌘] on macOS, and macOS
// has no F1 (it is a media key there).
const std::vector<ShortcutDef>& shortcuts(tk::Platform platform = tk::current_platform());

// The entry for `id` on `platform`.
const ShortcutDef& shortcut(ShortcutId id, tk::Platform platform = tk::current_platform());

// True when `event` matches any of `id`'s chords on the current platform.
bool matches(ShortcutId id, const tk::KeyEvent& event);

// Translated heading for a group.
std::string group_title(ShortcutGroup group);

// Every chord of `id`, labelled and joined with " / ".
std::string shortcut_label(ShortcutId id, tk::Platform platform = tk::current_platform());

} // namespace tesseract::views
