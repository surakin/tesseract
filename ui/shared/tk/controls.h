#pragma once

// Atomic widgets: Label, Button, Separator. Each is a leaf widget that
// owns its own paint + measure logic and is built from tk::Canvas
// primitives only.

#include "animator.h"
#include "svg.h"
#include "widget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
    // A font change alters measure(): re-measure (the layout itself rebuilds
    // via CachedTextLayout).
    void on_fonts_changed() override { invalidate_cache(); }

    FontRole role() const { return role_; }

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
    // A theme-resolved colour used while no explicit colour is set, so the
    // label follows theme changes (including High Contrast).
    enum class Tone
    {
        Normal,
        Error,  // palette.text_error
        Notice, // palette.notice_text
    };
    Label& set_tone(Tone t)
    {
        tone_ = t;
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

    Role access_role() const override
    {
        return Role::StaticText;
    }
    std::string access_name() const override
    {
        return text_;
    }

private:
    // Drop the built text layout AND schedule a relayout: a text/role/wrap/
    // trim/alignment change alters measure()'s result, so the surface has to
    // re-measure + repaint. Coalesced (mark_needs_relayout) so repeated
    // updates — e.g. a status label ticking on a poll — fold into one pass.
    // Out of line because it needs Host's full type.
    void invalidate_cache();

    std::string text_;
    FontRole role_;
    std::optional<Color> colour_; // null = by tone_
    Tone tone_ = Tone::Normal;
    TextHAlign halign_ = TextHAlign::Leading;
    bool wrap_ = false;
    TextTrim trim_ = TextTrim::None;
    Size min_size_ = {};

    // Layout cache. cached_max_w_ tracks the max-width measure() last
    // built for so subsequent paint reuses the layout when constraints
    // are unchanged.
    CachedTextLayout cached_;
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
        // Background-less accent-coloured text (a "Skip for now" style
        // secondary affordance). Underlined while hovered.
        Link,
    };

protected:
    explicit Button(std::string label, std::function<void()> on_click = {},
                    Variant variant = Variant::Primary)
        : label_(std::move(label)), variant_(variant)
    {
        // Hover/press/click/keyboard activation/cursor live on Widget; the
        // Button only paints its variant look from hovered()/pressed().
        set_hoverable(true, HoverStyle::None);
        set_clickable(true); // claim presses even before a handler is set
        Widget::set_on_click(std::move(on_click));
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
        Widget::set_on_click(std::move(f));
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
    // Corner radius of the fill; unset = the standard control radius. A
    // radius of half the button's side makes a circular badge.
    Button& set_corner_radius(float r)
    {
        corner_radius_ = r;
        return *this;
    }

    // Optional replacement for the Icon/Subtle variant's default theme-driven
    // fill (rgba(0,0,0,0) rest / palette.subtle_hover / palette.subtle_pressed).
    // Used by callers painting over a permanently dark backdrop (e.g. the
    // media viewer overlays) where the app's light/dark theme palette was
    // never designed to read against near-black content. Unset (default)
    // preserves existing behaviour for every other caller.
    struct FillOverride
    {
        Color rest;
        Color hover;
        Color pressed;
    };
    Button& set_fill_override(std::optional<FillOverride> f)
    {
        fill_override_ = f;
        return *this;
    }

    // Self-contained icon glyph for Variant::Icon, drawn by this button's
    // own paint() instead of relying on the parent to draw it on top after
    // the fact. `svg` must outlive this Button (or every subsequent call
    // to set_icon) — callers pass a span over an embedded resource byte
    // array with static/program lifetime, never an owned temporary.
    Button& set_icon(std::span<const std::uint8_t> svg, float logical_px = 20.0f)
    {
        icon_svg_ = svg;
        icon_logical_px_ = logical_px;
        icon_leading_ = false;
        return *this;
    }
    // Icon drawn before the label (icon + text), rather than replacing it as
    // set_icon() does. Same lifetime rule as set_icon(); an empty span clears
    // the icon and leaves a plain text button. Not meaningful for
    // Variant::Icon, which never paints its label.
    Button& set_leading_icon(std::span<const std::uint8_t> svg, float logical_px = 16.0f)
    {
        icon_svg_ = svg;
        icon_logical_px_ = logical_px;
        icon_leading_ = true;
        invalidate_cache();
        return *this;
    }
    // Overrides the default theme/enabled-driven tint (text_primary /
    // text_muted). Unset (default) preserves the ordinary look.
    Button& set_icon_color_override(std::optional<Color> color)
    {
        icon_color_override_ = color;
        return *this;
    }

    const std::string& label() const
    {
        return label_;
    }

    // Overrides access_name() below when set. Needed for icon-only buttons
    // whose label_ isn't a real accessible name — e.g. ComposeBar's emoji/
    // sticker/mic buttons set label_ to a raw Unicode glyph purely as an
    // internal test-scaffolding hook (never painted; the actual icon is a
    // Lucide SVG drawn separately), which would otherwise announce as
    // whatever that glyph happens to be named (e.g. the remove button's ×
    // glyph reads as "heavy multiplication x", not "remove attachment").
    Button& set_accessible_name(std::string n)
    {
        accessible_name_ = std::move(n);
        return *this;
    }

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

    // Keyboard-focusable whenever enabled (even with no handler yet);
    // Enter/Space triggers the same synthetic click() as pointer activation
    // (Widget::on_key_down).
    bool focusable() const override
    {
        return enabled_;
    }

    Role access_role() const override
    {
        return Role::Button;
    }
    std::string access_name() const override
    {
        return accessible_name_.empty() ? label_ : accessible_name_;
    }
    // Icon-only buttons show their accessible name as a tooltip when
    // reached by Tab — the keyboard counterpart of the hover tooltip their
    // owning views show — so a sighted keyboard user can tell them apart.
    // label_ is deliberately not used: on icon-only buttons it's often a
    // raw glyph test hook (see set_accessible_name()).
    std::string focus_tooltip_text() const override
    {
        const bool icon_only = variant_ == Variant::Icon ||
                               (!icon_svg_.empty() && !icon_leading_);
        return icon_only ? accessible_name_ : std::string{};
    }

protected:
    // Subclasses that paint their own content over the button's fill (e.g.
    // BusyButton's spinner) return false to make paint() stop after the
    // fill, skipping the icon and label.
    virtual bool paints_content() const
    {
        return true;
    }
    // The colour paint() uses for the icon / label in the current state.
    Color content_color(const Theme& theme) const;

private:
    void invalidate_cache()
    {
        cached_.reset();
    }

    std::string label_;
    std::string accessible_name_; // see set_accessible_name()
    Variant variant_ = Variant::Primary;
    Size min_size_{0, 32};
    std::optional<float> corner_radius_;
    std::optional<FillOverride> fill_override_;

    std::span<const std::uint8_t> icon_svg_;
    float icon_logical_px_ = 20.0f;
    bool icon_leading_ = false; // set_leading_icon(): icon + label, not icon-only
    std::optional<Color> icon_color_override_;
    IconCache icon_cache_;

    CachedTextLayout cached_;
    Size cached_size_{};
};

// A Button that can show a busy state: while busy it keeps its fill (and
// stays clickable) but paints the standard 8-dot loading spinner in place of
// its icon / label, animating itself via repaint requests. Used for the
// composer's send button while a send is still being prepared.
class BusyButton : public Button
{
protected:
    using Button::Button;
    TK_WIDGET_FACTORY_FRIEND(BusyButton)

public:
    void set_busy(bool busy);
    bool busy() const
    {
        return busy_;
    }

    void paint(PaintCtx&) override;

protected:
    bool paints_content() const override
    {
        return !busy_;
    }

private:
    bool busy_ = false;
    std::chrono::steady_clock::time_point busy_start_{};
};

// A Button with a persistent on/off state, for icon toolbars (e.g. a filter or
// follow toggle). While checked it keeps a "selected" background under the
// usual hover/press fills and tints its icon with the accent colour. A user
// click (pointer, Enter/Space, or an AT default action) flips the state and
// fires on_toggled with the new value; set_checked() is the silent,
// programmatic setter. Use set_on_toggled(), not set_on_click().
class ToggleButton : public Button
{
protected:
    explicit ToggleButton(std::string label,
                          Variant variant = Variant::Icon)
        : Button(std::move(label), {}, variant)
    {
        Widget::set_on_click([this] { toggle_(); });
    }
    TK_WIDGET_FACTORY_FRIEND(ToggleButton)

public:
    ToggleButton& set_checked(bool checked);
    bool checked() const
    {
        return checked_;
    }
    // Fires with the new state on every user toggle (never from set_checked).
    ToggleButton& set_on_toggled(std::function<void(bool)> f)
    {
        on_toggled_ = std::move(f);
        return *this;
    }
    // Icon tints for each state. Unchecked defaults to the Button's ordinary
    // tint (unset); checked defaults to the theme accent (unset).
    ToggleButton& set_unchecked_icon_color(std::optional<Color> c)
    {
        unchecked_tint_ = c;
        return *this;
    }
    ToggleButton& set_checked_icon_color(std::optional<Color> c)
    {
        checked_tint_ = c;
        return *this;
    }
    // The click handler is owned by the toggle logic.
    Button& set_on_click(std::function<void()>) = delete;

    void paint(PaintCtx&) override;

    AccessState access_state() const override
    {
        AccessState s;
        s.checked = checked_;
        return s;
    }

private:
    void toggle_();

    bool checked_ = false;
    std::function<void(bool)> on_toggled_;
    std::optional<Color> unchecked_tint_;
    std::optional<Color> checked_tint_;
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
    // Gated on has_focus() — see Button::on_key_down's comment above; same
    // root-broadcast exposure applies here.
    bool on_key_down(const KeyEvent& e) override
    {
        if (!enabled_ || !has_focus()) return false;
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

    Role access_role() const override
    {
        return Role::CheckBox;
    }
    std::string access_name() const override
    {
        return label_;
    }
    AccessState access_state() const override
    {
        AccessState s;
        s.checked = checked_;
        return s;
    }
    // Mirrors on_key_down's Enter/Space toggle body exactly (not
    // set_checked(), which is the silent/programmatic setter used for
    // initial state restore and deliberately does not fire on_change).
    bool access_default_action() override
    {
        if (!enabled_)
            return false;
        checked_ = !checked_;
        if (on_change)
        {
            auto cb = on_change;
            cb(checked_);
        }
        return true;
    }

private:
    std::string label_;
    bool checked_ = false;
    bool hovered_ = false;
    bool pressed_ = false;

    FontRole font_role_ = FontRole::Body;

    CachedTextLayout label_layout_;
    float cached_max_w_ = -2.0f;
    Size label_size_{};
    IconCache check_icon_;
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
    // Gated on has_focus() — see Button::on_key_down's comment above; same
    // root-broadcast exposure applies here.
    bool on_key_down(const KeyEvent& e) override
    {
        if (!enabled_ || !has_focus()) return false;
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

    Role access_role() const override
    {
        return Role::Switch;
    }
    std::string access_name() const override
    {
        return label_;
    }
    AccessState access_state() const override
    {
        AccessState s;
        s.checked = checked_;
        return s;
    }
    // Mirrors on_key_down's Enter/Space toggle body exactly (not
    // set_checked(), the silent/programmatic setter).
    bool access_default_action() override
    {
        if (!enabled_)
            return false;
        checked_ = !checked_;
        if (on_change)
        {
            auto cb = on_change;
            cb(checked_);
        }
        return true;
    }

private:
    std::string                 label_;
    bool                        checked_ = false;
    bool                        hovered_ = false;
    bool                        pressed_ = false;
    CachedTextLayout cached_;
    Size                        cached_size_{};
};

// A thin, non-interactive horizontal progress indicator. Determinate mode
// fills the track proportionally to progress(); indeterminate mode
// animates a looping sweep instead, for operations with no meaningful
// total (e.g. a room-history export whose room-creation timestamp hasn't
// resolved, so there's no fraction to show). Self-animates while
// indeterminate by calling host()->request_repaint() at the end of every
// paint — the same "schedule the next frame from inside paint()" idiom
// the media viewer's loading spinner uses, just via the built-in
// Widget::host() accessor since this is an ordinary child widget rather
// than a detached overlay.
class ProgressBar : public Widget
{
protected:
    explicit ProgressBar() = default;
    TK_WIDGET_FACTORY_FRIEND(ProgressBar)

public:
    // Switches to determinate mode; `value01` is clamped to [0,1].
    void set_progress(float value01);
    // Switches to indeterminate mode (a looping sweep). This is the
    // default until set_progress() is first called.
    void set_indeterminate();
    bool indeterminate() const { return indeterminate_; }
    float progress() const { return value_; }

    // Optional caption drawn above the track (e.g. "12,431 messages —
    // back to Mar 2019"). Empty (the default) draws no caption row.
    ProgressBar& set_label(std::string text);

    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

    bool focusable() const override { return false; }

    // The caption doubles as the accessible name; an indeterminate bar
    // reports no value (screen readers then just say "busy").
    Role access_role() const override { return Role::ProgressBar; }
    std::string access_name() const override { return label_; }
    AccessState access_state() const override
    {
        AccessState s;
        s.busy = indeterminate_;
        return s;
    }
    AccessValue access_value() const override
    {
        if (indeterminate_)
            return {};
        return AccessValue{true, 0.0, 100.0, static_cast<double>(value_) * 100.0};
    }

private:
    bool  indeterminate_ = true;
    float value_ = 0.0f;
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};

    std::string                 label_;
    CachedTextLayout label_layout_;
    Size                        label_size_{};
};

} // namespace tk
