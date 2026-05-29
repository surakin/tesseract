#pragma once

#include "widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

class ComboBox : public Widget
{
public:
    struct Option
    {
        std::string label;
        std::string value;
    };

    void set_options(std::vector<Option> options);
    void set_selected_value(const std::string& value);
    const std::string& selected_value() const { return selected_value_; }

    bool is_expanded() const { return expanded_; }
    void collapse();

    // Constrain the popup to this world-space rect (e.g. the parent panel).
    // Call from the parent's arrange(). An empty rect means unconstrained.
    void set_popup_clip(Rect r) { popup_clip_ = r; }

    std::function<void(std::string value)> on_changed;

    Size     measure(LayoutCtx&, Size constraints) override;
    void     arrange(LayoutCtx&, Rect bounds) override;
    void     paint(PaintCtx&) override;
    void     paint_overlay(PaintCtx&) override;
    void     on_popup_dismiss() override;
    bool     contains_world(Point world) const override;
    bool     on_pointer_down(Point local) override;
    void     on_pointer_up(Point local, bool inside_self) override;
    bool     on_pointer_move(Point local) override;
    void     on_pointer_leave() override;

private:
    std::vector<Option> options_;
    std::string         selected_value_;

    // Index 0 = button label layout, 1..N = dropdown row label layouts.
    // Invalidated when options change or when arrange() width changes.
    std::vector<std::unique_ptr<TextLayout>> layouts_;

    bool expanded_       = false;
    int  hovered_option_ = -1;
    bool button_hovered_ = false;
    bool button_pressed_ = false;

    Rect  button_rect_{};
    Rect  dropdown_rect_{};
    Rect  popup_clip_{};
    float last_w_ = -1.0f;

    // Returns the index of the option whose dropdown row contains world-space
    // point `w`, or -1 when the point is outside all rows or not expanded.
    int hit_dropdown_row(Point w) const;
};

} // namespace tk
