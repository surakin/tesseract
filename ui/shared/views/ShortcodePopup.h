#pragma once
#include "tk/widget.h"
#include "views/ShortcodeEngine.h"
#include <functional>
#include <vector>
#include <algorithm>

namespace tesseract::views {

class ShortcodePopup : public tk::Widget {
public:
    static constexpr float kRowHeight = 36.0f;
    static constexpr float kWidth     = 280.0f;
    static constexpr int   kMaxRows   = 8;

    void set_suggestions(std::vector<ShortcodeSuggestion> suggestions);
    void set_selected_index(int index);
    int  selected_index() const { return selected_index_; }

    int visible_rows() const {
        return std::min((int)suggestions_.size(), kMaxRows);
    }

    std::function<void(ShortcodeSuggestion)> on_accepted;
    std::function<void()>                   on_dismissed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void     arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    void     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

private:
    std::vector<ShortcodeSuggestion> suggestions_;
    int selected_index_ = -1;
    int hovered_index_  = -1;
    int pressed_index_  = -1;

    int row_at(float y) const {
        int r = (int)(y / kRowHeight);
        return (r >= 0 && r < visible_rows()) ? r : -1;
    }
};

} // namespace tesseract::views
