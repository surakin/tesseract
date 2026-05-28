#pragma once
#include "tk/widget.h"
#include "views/SlashCommandEngine.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <vector>

namespace tesseract::views
{

class SlashCommandPopup : public tk::Widget
{
public:
    static constexpr float kRowHeight = 44.0f; // taller than shortcode: shows two lines
    static constexpr float kWidth     = 320.0f;
    static constexpr int   kMaxRows   = 8;

    void set_suggestions(std::vector<SlashCommandSuggestion> suggestions);
    void set_selected_index(int index);
    int  selected_index() const { return selected_index_; }
    int  visible_rows() const
    {
        return std::min((int)suggestions_.size(), kMaxRows);
    }
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

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void     arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;

private:
    std::vector<SlashCommandSuggestion> suggestions_;
    int selected_index_ = -1;
    int hovered_index_  = -1;
    int pressed_index_  = -1;

    int row_at(float y) const
    {
        int r = (int)(y / kRowHeight);
        return (r >= 0 && r < visible_rows()) ? r : -1;
    }
};

} // namespace tesseract::views
