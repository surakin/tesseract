#pragma once
#include "tk/i18n.h"
#include "views/ListPopupBase.h"
#include "views/SlashCommandEngine.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

// Autocomplete popup shown while typing a `/command` in the composer.
// All list scaffolding lives in ListPopupBase; this class owns the suggestion
// model and a two-line (name + description) per-row paint. It also overrides
// the virtual set_selected_index with clamping (the shell keyboard handlers
// drive it directly), and exposes suggestion_at() for those handlers.
class SlashCommandPopup : public ListPopupBase
{
public:
    static constexpr float kRowHeight = 44.0f; // taller: shows two lines
    static constexpr float kWidth     = 320.0f;
    static constexpr int   kMaxRows   = 8;

    void set_suggestions(std::vector<SlashCommandSuggestion> suggestions);

    // Switch to (or update) hint mode: a single, non-selectable informational
    // row shown while the user types positional arguments for an accepted
    // MSC4391 bot command — see SlashCommandController's CollectingArgs
    // state. `is_error` styles the row in the error color (validation
    // failure) instead of the muted hint color. Clears any suggestion list.
    void show_hint(std::string text, bool is_error);

    // Leave hint mode and go back to rendering `suggestions_` (used when the
    // controller re-enters normal autocomplete after CollectingArgs exits).
    void clear_hint();

    bool hint_mode() const
    {
        return hint_mode_;
    }

    // Clamping setter (differs from the plain base assignment): -1 clears, any
    // other value is clamped into the visible range.
    void set_selected_index(int index) override;

    // Public accessor used by shell keyboard handlers — they consume
    // Up/Down/Enter themselves, then need to fire on_accepted with the
    // suggestion that lives at the current selected_index_.
    const SlashCommandSuggestion& suggestion_at(int i) const
    {
        assert(i >= 0 && i < (int)suggestions_.size());
        return suggestions_[i];
    }

    std::function<void(SlashCommandSuggestion)> on_accepted;
    std::function<void()>                       on_dismissed;

protected:
    size_t row_count() const override
    {
        return hint_mode_ ? 1 : suggestions_.size();
    }
    void paint_row(tk::PaintCtx& ctx, const tk::Rect& row, size_t index,
                   bool selected, bool hovered) override;
    void on_row_activated(size_t index) override
    {
        // No-op in hint mode — there's nothing to "accept"; the controller
        // handles Enter itself (validate + send) before it would reach here.
        if (!hint_mode_ && on_accepted)
            on_accepted(suggestions_[index]);
    }
    float row_height() const override { return kRowHeight; }
    float width() const override { return kWidth; }
    int max_visible_rows() const override { return kMaxRows; }

public:
    // tk::WidgetRowAccessibility — mirrors paint_row's own hint-mode branch
    // and primary/secondary text ("/name args_hint", description + "via
    // {bot}" disambiguation for bot-advertised commands). The hint row
    // (validation feedback while typing positional args) is StaticText,
    // not ListItem — it isn't selectable/activatable, matching
    // on_row_activated's own no-op in hint mode.
    tk::Role access_role_for_widget_row(std::size_t index) const override
    {
        if (hint_mode_)
            return index == 0 ? tk::Role::StaticText : tk::Role::None;
        return index < suggestions_.size() ? tk::Role::ListItem : tk::Role::None;
    }
    std::string access_name_for_widget_row(std::size_t index) const override
    {
        if (hint_mode_)
            return index == 0 ? hint_text_ : std::string{};
        if (index >= suggestions_.size())
            return {};
        const auto& s = suggestions_[index];
        std::string name = "/" + s.name;
        if (!s.args_hint.empty())
            name += " " + s.args_hint;
        if (!s.description.empty())
            name += ": " + s.description;
        if (!s.is_builtin)
        {
            const std::string& who =
                s.bot_display_name.empty() ? s.bot_sender_id : s.bot_display_name;
            name += " (" + tk::trf(tk::tr("via {0}"), {who}) + ")";
        }
        return name;
    }

private:
    void paint_hint_row(tk::PaintCtx& ctx, const tk::Rect& row);

    std::vector<SlashCommandSuggestion> suggestions_;
    bool hint_mode_ = false;
    std::string hint_text_;
    bool hint_is_error_ = false;
};

} // namespace tesseract::views
