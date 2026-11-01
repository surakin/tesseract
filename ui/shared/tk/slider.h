#pragma once

// Horizontal slider: a track with a draggable thumb over a [min, max] range.
// Click on the track jumps the thumb there, dragging tracks the pointer even
// outside the widget, arrow/Page/Home/End keys step the value when focused.
//
// Optional extras the media scrub bar needs: a secondary "buffered" band
// painted under the fill, and a selectable-value limit (the highest value the
// user can reach, e.g. the downloaded part of a stream).

#include "widget.h"

#include <functional>
#include <optional>
#include <string>

namespace tk
{

class Slider : public Widget
{
protected:
    explicit Slider(double min = 0.0, double max = 1.0, double step = 0.01);
    TK_WIDGET_FACTORY_FRIEND(Slider)

public:
    struct Colors
    {
        Color track;
        Color buffered;
        Color fill;
        Color thumb;
    };

    // The setters below are silent: they never fire on_changed/on_committed.
    void set_range(double min, double max);
    void set_step(double step);
    void set_page_step(double step); // default 10 * step
    void set_value(double v);        // clamped to [min, limit]
    double value() const
    {
        return value_;
    }
    void set_limit(double v);    // highest selectable value; default = max
    void set_buffered(double v); // band from min to v; below min = none
    void set_keyboard_focusable(bool f);
    void set_colors(std::optional<Colors> c); // nullopt = theme palette
    void set_accessible_name(std::string name);
    // Abandons a drag in progress (no commit).
    void cancel_drag()
    {
        dragging_ = false;
    }
    // A hidden or disabled slider is not being dragged, whatever the stale flag
    // says (a widget's own set_visible() fires no visibility hook on itself).
    bool dragging() const
    {
        return dragging_ && enabled_ && visible();
    }

    // User-driven change (press, drag, keys), live while dragging.
    std::function<void(double)> on_changed;
    // The user is done: pointer release, or each key press.
    std::function<void(double)> on_committed;

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    void on_pointer_drag(Point local) override;
    bool on_key_down(const KeyEvent& e) override;

    bool focusable() const override
    {
        return enabled_ && keyboard_focusable_;
    }
    bool focus_on_click() const override
    {
        return enabled_ && keyboard_focusable_;
    }
    CursorShape cursor() const override
    {
        return enabled_ ? CursorShape::Pointer : CursorShape::Default;
    }

    Role access_role() const override
    {
        return Role::Slider;
    }
    std::string access_name() const override
    {
        return name_;
    }
    AccessValue access_value() const override;
    bool access_set_value(double value) override;

    void set_enabled(bool enabled) override;

protected:
    // A hidden slider never sees the pointer-up that would end a drag.
    void on_effective_visibility_changed_(bool now_visible) override;

private:
    double clamp_(double v) const;
    double value_at_(float local_x) const;
    float frac_(double v) const;
    // Applies a user-driven value; fires on_changed when it moved.
    void apply_user_(double v, bool commit);

    double min_;
    double max_;
    double step_;
    double page_step_;
    double value_;
    double limit_;
    bool limit_explicit_ = false; // set_limit() called: set_range() keeps it
    double buffered_;
    bool keyboard_focusable_ = true;
    bool dragging_ = false;
    std::optional<Colors> colors_;
    std::string name_;
};

} // namespace tk
