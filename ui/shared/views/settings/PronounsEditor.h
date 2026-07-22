#pragma once

// PronounsEditor — repeatable-row editor for MSC4247 `m.pronouns` entries
// (one row per language: a LanguagePicker for the BCP-47 tag + a summary
// TextField, e.g. "she/her", plus a remove button), with an "Add language"
// button below. Replaces AccountSection::ExtendedFields' old single pronouns
// TextField now that a user can register more than one language.
//
// `grammatical_gender` is parsed/round-tripped (see tesseract::PronounEntry)
// but not exposed here — editing it is out of scope; an entry that already
// has one set by another client keeps it untouched across edits to its
// language/summary, since this widget only ever mutates those two fields.
//
// Uses a fixed pool of kMaxRows row-widget slots (shown/hidden by current
// entry count) rather than dynamically creating/destroying child widgets per
// edit, mirroring AccountSection::ExtendedFields' own fixed-field-slots
// pattern. kMaxRows language variants for one person's pronouns is a
// generous practical cap, not a silent truncation of real data — the "Add
// language" button simply disables once reached.

#include "LanguagePicker.h"

#include "tk/controls.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class PronounsEditor : public tk::Widget
{
protected:
    // host() is nullable — mirrors AccountSection::ExtendedFields: when
    // null (e.g. unit tests constructing this directly), no row widgets are
    // created and this widget renders nothing.
    PronounsEditor();
    TK_WIDGET_FACTORY_FRIEND(PronounsEditor)

public:
    // Baseline load — pushes `entries` into the row widgets. Entries with an
    // empty summary are kept (an in-progress row from a prior open) but
    // never surface through on_changed until completed.
    void set_pronouns(std::vector<tesseract::PronounEntry> entries);

    // The full staged list, including any in-progress empty-summary row.
    const std::vector<tesseract::PronounEntry>& pronouns() const
    {
        return entries_;
    }

    void set_editable(bool editable);
    void set_busy(bool busy);
    void set_error(std::string error);

    // Shadows Widget::set_visible (not virtual) so hiding this widget always
    // also hides every row's native TextField/LanguagePicker overlays —
    // mirrors tk::TextField's/LanguagePicker's own set_visible overrides
    // (needed for e.g. SettingsView's inactive-tab hide, whose subtree stops
    // being arrange()'d so a plain non-cascading hide would leave a native
    // overlay's OS-level visibility stuck at whatever it last was).
    void set_visible(bool v);

    // Forwarded to every row's summary field and LanguagePicker — GTK's
    // native GtkEntry needs compact mode for a snug visual fit; every other
    // backend uses the default (non-compact) chrome.
    void set_compact(bool compact);

    // Fired from flush() — never directly from a language pick or summary
    // edit — with the subset of entries that have both a language and a
    // summary (never an in-progress empty row), and only when that subset
    // actually differs from what was last sent. See flush()'s doc comment
    // for when that happens.
    std::function<void(std::vector<tesseract::PronounEntry>)> on_changed;

    // Re-syncs every row's live widget text/value into entries_ (so a
    // summary typed but never Enter-submitted is never lost), then fires
    // on_changed if the resulting complete-entries subset actually changed
    // since the last flush. Called on: a row field losing focus, the
    // Account settings tab being switched away from, and the settings
    // screen being closed — see SettingsView.cpp's wiring. Row removal
    // flushes immediately on its own (see remove_row_()), since it's a
    // complete action rather than an in-progress edit.
    void flush();

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    void     on_theme_changed(const tk::Theme& t) override;

private:
    struct Row
    {
        LanguagePicker* lang    = nullptr;
        tk::TextField*  summary = nullptr;
        tk::Button*     remove  = nullptr;
    };

    void refresh_slots_();     // pushes entries_ into the fixed row widgets
    void add_row_();
    void remove_row_(std::size_t index);
    void notify_changed_();
    // The subset of entries_ with both a language and a summary — what
    // actually gets sent over on_changed. Shared by notify_changed_() and
    // flush()'s dirty-check.
    std::vector<tesseract::PronounEntry> complete_entries_() const;
    float row_y_(int slot) const;

    std::vector<tesseract::PronounEntry> entries_;
    // Snapshot of complete_entries_() as of the last flush() that actually
    // fired on_changed — lets flush() no-op when nothing changed.
    std::vector<tesseract::PronounEntry> last_saved_;
    std::vector<Row> rows_; // kMaxRows fixed slots, shown/hidden by count

    tk::Button* add_button_ = nullptr;

    bool editable_ = false;
    bool busy_     = false;
    std::string error_;
    mutable std::unique_ptr<tk::TextLayout> error_layout_;

    // tk::Button::Variant::Icon never draws its own label (see Button::
    // paint()'s "icon glyph drawn by parent" comment) — this widget draws
    // the "×" glyph itself on top of each row's remove button, mirroring
    // UserProfilePanel::close_btn_'s identical pattern. One shared layout
    // (same glyph, every row) redrawn at each button's own position.
    mutable std::unique_ptr<tk::TextLayout> remove_glyph_layout_;

    static constexpr int   kMaxRows    = 8;
    static constexpr float kRowH       = 32.0f;
    // Wide enough to clear two rows' card padding (kCardPadY on each side)
    // plus a visible gap between adjacent cards — see paint()'s per-row
    // fill_rounded_rect card background.
    static constexpr float kRowGap     = 20.0f;
    static constexpr float kLangW      = 96.0f;
    static constexpr float kSummaryW   = 240.0f; // capped, not stretched — see arrange()
    static constexpr float kRemoveW    = 28.0f;
    static constexpr float kFieldGap   = 6.0f;
    static constexpr float kAddButtonH = 28.0f;
    static constexpr float kErrorH     = 14.0f;
    static constexpr float kErrorGap   = 4.0f;
    // Padding between a row's actual controls and its card background's
    // edge (see paint()).
    static constexpr float kCardPadX   = 10.0f;
    static constexpr float kCardPadY   = 6.0f;
};

} // namespace tesseract::views
