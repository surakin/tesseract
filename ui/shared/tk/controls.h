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
protected:
    explicit Label(std::string text, FontRole role = FontRole::Body)
        : text_(std::move(text)), role_(role)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(Label)

public:
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

protected:
    explicit Separator(Orientation o = Orientation::Horizontal)
        : orientation_(o)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(Separator)

public:
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
        Icon,
        Destructive,
    };

protected:
    explicit Button(std::string label, std::function<void()> on_click = {},
                    Variant variant = Variant::Primary)
        : label_(std::move(label)), on_click_(std::move(on_click)),
          variant_(variant)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(Button)

public:
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
    Button& set_min_size(Size s)
    {
        min_size_ = s;
        return *this;
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

    // Keyboard-focusable whenever enabled; Enter/Space triggers the same
    // synthetic click() used by pointer activation.
    bool focusable() const override
    {
        return enabled_;
    }
    bool on_key_down(const KeyEvent& event) override
    {
        if (event.key == Key::Enter || event.key == Key::Space)
        {
            click();
            return true;
        }
        return false;
    }

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
    bool hovered_ = false;
    bool pressed_ = false;
    Size min_size_{0, 32};

    std::unique_ptr<TextLayout> cached_;
    Size cached_size_{};
};

// A labelled two-state checkbox. Hover and press are tracked internally;
// on_change fires with the new boolean state on every user toggle.
class CheckButton : public Widget
{
protected:
    explicit CheckButton(std::string label, bool checked = false);
    TK_WIDGET_FACTORY_FRIEND(CheckButton)

public:
    void set_checked(bool checked);
    bool checked() const
    {
        return checked_;
    }
    void set_font_role(FontRole role);

    std::function<void(bool)> on_change;
    // Fired on the false->true hover transition / on leaving hover. Used by
    // callers that want to show a tooltip for this checkbox.
    std::function<void()> on_hover_enter;
    std::function<void()> on_hover_leave;

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;

    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    bool focusable() const override
    {
        return enabled_;
    }
    bool on_key_down(const KeyEvent& e) override
    {
        if (!enabled_) return false;
        if (e.key == Key::Enter || e.key == Key::Space)
        {
            checked_ = !checked_;
            if (on_change)
            {
                // Copy before invoking — matches Button::click()/
                // SwitchButton's convention, in case the handler rebuilds
                // this widget's parent.
                auto cb = on_change;
                cb(checked_);
            }
            return true;
        }
        return false;
    }

private:
    std::string label_;
    bool checked_ = false;
    bool hovered_ = false;
    bool pressed_ = false;

    FontRole font_role_ = FontRole::Body;

    std::unique_ptr<TextLayout> label_layout_;
    float cached_max_w_ = -2.0f;
    Size label_size_{};
};

// A label + sliding on/off switch (settings-style). The whole row is the hit
// target: clicking anywhere toggles. Track is accent-filled when on, muted when
// off, with a knob that slides between the two ends. Fires `on_change(new_state)`
// on user click; `set_checked()` is programmatic and silent.
class SwitchButton : public Widget
{
protected:
    explicit SwitchButton(std::string label, bool checked = false)
        : label_(std::move(label)), checked_(checked) { }
    TK_WIDGET_FACTORY_FRIEND(SwitchButton)

public:
    SwitchButton& set_label(std::string l) { label_ = std::move(l); cached_.reset(); return *this; }
    void set_checked(bool c) { checked_ = c; }
    bool checked() const { return checked_; }

    std::function<void(bool)> on_change; // fires with the new state on user click

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    bool focusable() const override
    {
        return enabled_;
    }
    bool on_key_down(const KeyEvent& e) override
    {
        if (!enabled_) return false;
        if (e.key == Key::Enter || e.key == Key::Space)
        {
            checked_ = !checked_;
            if (on_change)
            {
                auto cb = on_change;
                cb(checked_);
            }
            return true;
        }
        return false;
    }

private:
    std::string                 label_;
    bool                        checked_ = false;
    bool                        hovered_ = false;
    bool                        pressed_ = false;
    std::unique_ptr<TextLayout> cached_;
    Size                        cached_size_{};
};

} // namespace tk
