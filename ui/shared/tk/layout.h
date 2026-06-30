#pragma once

// Common layout containers. VBox stacks children top-to-bottom; HBox
// left-to-right. Both honour padding, spacing, Main/Cross alignment, and
// per-child LayoutHints::fill_main / fill_cross. Stack layers children in the
// same bounds, useful for root content + modal overlays.

#include "widget.h"

namespace tk
{

class FlexBox : public Widget
{
public:
    enum class Axis
    {
        Vertical,
        Horizontal
    };

    explicit FlexBox(Axis a) : axis_(a)
    {
    }

    // Builder-style setters. Returning *this lets callers chain.
    FlexBox& set_spacing(float v)
    {
        spacing_ = v;
        return *this;
    }
    FlexBox& set_padding(Edges e)
    {
        padding_ = e;
        return *this;
    }
    FlexBox& set_main(Main m)
    {
        main_align_ = m;
        return *this;
    }
    FlexBox& set_cross(Cross c)
    {
        cross_align_ = c;
        return *this;
    }

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;

    Axis axis() const
    {
        return axis_;
    }
    float spacing() const
    {
        return spacing_;
    }
    Edges padding() const
    {
        return padding_;
    }

private:
    Axis axis_;
    float spacing_ = 0;
    Edges padding_ = {};
    Main main_align_ = Main::Start;
    Cross cross_align_ = Cross::Stretch;
};

class VBox : public FlexBox
{
public:
    VBox() : FlexBox(Axis::Vertical)
    {
    }
};

class HBox : public FlexBox
{
public:
    HBox() : FlexBox(Axis::Horizontal)
    {
    }
};

// Fixed-size gap that fills its main-axis allotment when fill_main is set.
class Spacer : public Widget
{
public:
    explicit Spacer(float main_size = 0) : main_size_(main_size)
    {
    }
    Size measure(LayoutCtx&, Size /*constraints*/) override
    {
        return {0, main_size_}; // axis is decided by the parent box
    }
    void paint(PaintCtx&) override
    {
    }

private:
    float main_size_;
};

class FixedBox : public Widget
{
public:
    explicit FixedBox(Size size) : size_(size)
    {
    }

    Size measure(LayoutCtx&, Size constraints) override;

private:
    Size size_{};
};

class Stack : public Widget
{
public:
    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
};

} // namespace tk
