#pragma once

// tk::SearchablePicker — base for a searchable-dropdown field: an editable
// tk::TextField showing the current committed value, backed by a filtered,
// ranked dropdown of candidate entries shown while the field is being
// edited. Concrete leaf classes (views::LanguagePicker, views::TimezonePicker,
// ...) supply the entry data via a handful of pure-virtual hooks; this base
// owns the field, the popup, and all of the filter/commit/keyboard-nav
// orchestration.
//
// The dropdown is a real, standalone native popup surface
// (host()->make_popup_surface(), see host.h's PopupSurfaceHandle) rather
// than a canvas-drawn overlay — a canvas popup can never reliably occlude a
// native tk::TextField it happens to overlap (e.g. a sibling settings-page
// field), since a native control always composites above its own canvas
// parent's painted content regardless of paint()/paint_overlay() ordering,
// on every backend. Generalizes the same native-popup-surface pattern
// already used for MentionPopup/SlashCommandPopup/ShortcodePopup/the Gif
// popup/tk::ComboBox.
//
// Keyboard focus stays on the internal field the whole time the dropdown is
// open — mirroring those same popups — so Up/Down/Enter/Escape keep working
// exactly as expected; the dropdown's own row widgets never take focus (see
// DropdownList) and only handle mouse hover/click.

#include "tk/host.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tk
{

class SearchablePicker : public Widget
{
protected:
    SearchablePicker() = default;

    // Builds the internal field and wires filter/commit/nav/blur handling.
    // Must be called exactly once by each leaf class's constructor, after
    // its own `if (!host()) return;` guard — the entry_*_() hooks below are
    // pure virtual and unsafe to invoke before the leaf object is fully
    // constructed, so this can't happen from this base class's constructor.
    void init_(float field_h, float row_h, float width, int max_rows,
              std::string placeholder);

public:
    // Sets the committed value and mirrors it into the field's text. No-op
    // (beyond updating state) while the user is actively editing — callers
    // push data in on load, not mid-edit.
    void set_value(std::string value);
    const std::string& value() const
    {
        return value_;
    }

    void set_enabled(bool enabled) override;

    // Shadows Widget::set_visible (not virtual) so this widget's own
    // visible_ flag and the internal native TextField's OS-level visibility
    // always move together — mirrors tk::TextField's own set_visible
    // override. Also collapses (hides) the dropdown, if open.
    void set_visible(bool v);

    // Forwarded to the internal TextField — GTK's native GtkEntry needs
    // compact mode for a snug visual fit in some contexts.
    void set_compact(bool compact);

    // Fired once a value is committed: either a dropdown row is picked, or
    // the field is submitted (Enter) with text that exactly matches a known
    // entry. Never fired for a blur/Escape revert.
    std::function<void(std::string value)> on_changed;

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void on_theme_changed(const Theme& t) override;

protected:
    class DropdownList; // popup surface's root widget — defined in the .cpp

    // Data source hooks, implemented by each leaf class. `index` ranges over
    // [0, entry_count_()).
    virtual std::size_t entry_count_() const = 0;
    // Rank of entry `index` against `query`: 0 = best match, increasing =
    // weaker match, -1 = no match at all. Empty `query` should match every
    // entry (lowest-priority tier).
    virtual int match_rank_(std::size_t index, std::string_view query) const = 0;
    // The value committed via on_changed / round-tripped through set_value's
    // identity check.
    virtual std::string entry_key_(std::size_t index) const = 0;
    // Dropdown row display text.
    virtual std::string entry_label_(std::size_t index) const = 0;
    // Text shown in the field itself once a value is committed (e.g. a
    // language's full name rather than its BCP-47 code) — distinct from
    // entry_label_ since a dropdown row can afford to show more than a
    // compact field can.
    virtual std::string entry_display_(std::size_t index) const = 0;

private:
    // Text to show in the field for committed value `key`: the matching
    // entry's entry_display_(), or `key` itself if no entry matches (e.g. a
    // value set externally that isn't in this picker's own data — keeps
    // set_value() a graceful no-op rather than blanking the field).
    std::string display_for_(const std::string& key) const;

    void refilter_(const std::string& query);
    void collapse_();
    void commit_row_(std::size_t filtered_index);
    void set_hovered_(int row);
    bool handle_nav_(NavKey key);
    // Single choke point for every expanded_ mutation.
    void set_expanded_(bool expanded);
    void reposition_popup_();

    TextField* field_ = nullptr;

    std::string value_; // last committed value; reverted to on blur w/o a pick

    // Indices into the leaf class's entry data, ranked, capped at max_rows_.
    std::vector<std::size_t> filtered_;

    // Created lazily on first expand — most instances never open a dropdown,
    // so eagerly creating a native popup surface per instance would be
    // wasteful (mirrors e.g. PronounsEditor's pre-allocated fixed row slots).
    std::unique_ptr<PopupSurfaceHandle> popup_;
    DropdownList* dropdown_ = nullptr; // borrowed — owned by popup_ once set

    bool expanded_    = false;
    int  hovered_row_ = -1;

    Rect field_rect_{};

    float field_h_  = 32.0f;
    float row_h_    = 28.0f;
    float width_    = 220.0f;
    int   max_rows_ = 8;
};

} // namespace tk
