#pragma once
#include "views/ListPopupBase.h"
#include "views/SlashCommandEngine.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <vector>

namespace tesseract::views
{

// Autocomplete popup shown while typing a `/command` in the composer.
// All list scaffolding lives in ListPopupBase; this class owns the suggestion
// model and a two-line (name + description) per-row paint. It also overrides
// set_selected_index with clamping (the shell keyboard handlers drive it
// directly), and exposes suggestion_at() for those handlers.
class SlashCommandPopup : public ListPopupBase
{
public:
    static constexpr float kRowHeight = 44.0f; // taller: shows two lines
    static constexpr float kWidth     = 320.0f;
    static constexpr int   kMaxRows   = 8;

    void set_suggestions(std::vector<SlashCommandSuggestion> suggestions);

    // Clamping setter (differs from the plain base assignment): -1 clears, any
    // other value is clamped into the visible range.
    void set_selected_index(int index);

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
        return suggestions_.size();
    }
    void paint_row(tk::PaintCtx& ctx, const tk::Rect& row, size_t index,
                   bool selected, bool hovered) override;
    void on_row_activated(size_t index) override
    {
        if (on_accepted)
            on_accepted(suggestions_[index]);
    }
    float row_height() const override { return kRowHeight; }
    float width() const override { return kWidth; }
    int max_visible_rows() const override { return kMaxRows; }

private:
    std::vector<SlashCommandSuggestion> suggestions_;
};

} // namespace tesseract::views
