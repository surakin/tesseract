#pragma once

// Atomic widgets: Label, Button, Separator. Each is a leaf widget that
// owns its own paint + measure logic and is built from tk::Canvas
// primitives only.

#include "widget.h"

#include <functional>
#include <optional>
#include <string>

namespace tk
{

class Label : public Widget
{
public:
    explicit Label(std::string text, FontRole role = FontRole::Body)
        : text_(std::move(text)), role_(role)
    {
    }

    Label& set_text(std::string t)
    {
        text_ = std::move(t);
        invalidate_cache();
        return *this;
    }
    Label& set_role(FontRole r)
    {
        role_ = r;
        invalidate_cache();
        return *this;
    }
    Label& set_colour(std::optional<Color> c)
    {
        colour_ = c;
        return *this;
    }
    Label& set_halign(TextHAlign a)
    {
        halign_ = a;
        invalidate_cache();
        return *this;
    }
    Label& set_wrap(bool w)
    {
        wrap_ = w;
        invalidate_cache();
        return *this;
    }
    Label& set_trim(TextTrim t)
    {
        trim_ = t;
        invalidate_cache();
        return *this;
    }
    Label& set_min_size(Size s)
    {
        min_size_ = s;
        return *this;
    }

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

    const std::string& text() const
    {
        return text_;
    }

private:
    void invalidate_cache()
    {
        cached_.reset();
        cached_max_w_ = -2;
    }

    std::string text_;
    FontRole role_;
    std::optional<Color> colour_; // null = theme.text_primary
    TextHAlign halign_ = TextHAlign::Leading;
    bool wrap_ = false;
    TextTrim trim_ = TextTrim::None;
    Size min_size_ = {};

    // Layout cache. cached_max_w_ tracks the max-width measure() last
    // built for so subsequent paint reuses the layout when constraints
    // are unchanged.
    std::unique_ptr<TextLayout> cached_;
    float cached_max_w_ = -2;
    Size cached_size_{};
};

class Separator : public Widget
{
public:
    enum class Orientation
    {
        Horizontal,
        Vertical
    };

    explicit Separator(Orientation o = Orientation::Horizontal)
        : orientation_(o)
    {
    }

    Separator& set_thickness(float t)
    {
        thickness_ = t;
        return *this;
    }
    Separator& set_colour(std::optional<Color> c)
    {
        colour_ = c;
        return *this;
    }

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

private:
    Orientation orientation_;
    float thickness_ = 1.0f;
    std::optional<Color> colour_; // null = theme.separator
};

class Button : public Widget
{
public:
    enum class Variant
    {
        Primary,
        Subtle,
        Icon
    };

    explicit Button(std::string label, std::function<void()> on_click = {},
                    Variant variant = Variant::Primary)
        : label_(std::move(label)), on_click_(std::move(on_click)),
          variant_(variant)
    {
    }

    Button& set_label(std::string l)
    {
        label_ = std::move(l);
        invalidate_cache();
        return *this;
    }
    Button& set_on_click(std::function<void()> f)
    {
        on_click_ = std::move(f);
        return *this;
    }
    Button& set_variant(Variant v)
    {
        variant_ = v;
        return *this;
    }
    Button& set_enabled(bool e)
    {
        enabled_ = e;
        return *this;
    }
    Button& set_min_size(Size s)
    {
        min_size_ = s;
        return *this;
    }

    bool enabled() const
    {
        return enabled_;
    }
    bool hovered() const
    {
        return hovered_;
    }
    bool pressed() const
    {
        return pressed_;
    }
    const std::string& label() const
    {
        return label_;
    }

    // Synthetic click — bypasses pointer state. Used by tests and by the
    // host when keyboard activation triggers the button.
    void click();

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;

    // Hover is driven externally by the host's "topmost hovered button"
    // tracking — see Host::on_pointer_move in each host_*.cpp. Pressed
    // is managed here via on_pointer_down/up.
    void set_hovered(bool h)
    {
        hovered_ = h;
    }

private:
    void invalidate_cache()
    {
        cached_.reset();
    }

    std::string label_;
    std::function<void()> on_click_;
    Variant variant_ = Variant::Primary;
    bool enabled_ = true;
    bool hovered_ = false;
    bool pressed_ = false;
    Size min_size_{0, 32};

    std::unique_ptr<TextLayout> cached_;
    Size cached_size_{};
};

} // namespace tk
