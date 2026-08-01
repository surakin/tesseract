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
        return *this;
    }
    // Overrides the default theme/enabled-driven tint (text_primary /
    // text_muted). Unset (default) preserves the ordinary look.
    Button& set_icon_color_override(std::optional<Color> color)
    {
        icon_color_override_ = color;
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
    // Gated on has_focus(): without it, this is reachable not only as the
    // genuinely tk-focused widget but also via Host::dispatch_key_down's
    // root-wide broadcast fallback (fired whenever the ACTUALLY focused
    // widget doesn't consume a key) — any other unfocused Button in the
    // tree would silently "click" on a stray Enter/Space meant elsewhere.
    bool on_key_down(const KeyEvent& event) override
    {
        if (has_focus() && (event.key == Key::Enter || event.key == Key::Space))
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

    Role access_role() const override
    {
        return Role::Button;
    }
    std::string access_name() const override
    {
        return accessible_name_.empty() ? label_ : accessible_name_;
    }
    // click() already guards on enabled_/on_click_, matching the pointer
    // and Enter/Space activation paths — nothing extra needed here beyond
    // reporting whether it actually had something to invoke.
    bool access_default_action() override
    {
        click();
        return enabled_ && static_cast<bool>(on_click_);
    }

private:
    void invalidate_cache()
    {
        cached_.reset();
    }

    std::string label_;
    std::string accessible_name_; // see set_accessible_name()
    std::function<void()> on_click_;
    Variant variant_ = Variant::Primary;
    bool hovered_ = false;
    bool pressed_ = false;
    Size min_size_{0, 32};
    std::optional<FillOverride> fill_override_;
    // Eases the hover fill in/out instead of snapping; pressed stays an
    // instant override on top (see paint()).
    FloatTween hover_fade_;

    std::span<const std::uint8_t> icon_svg_;
    float icon_logical_px_ = 20.0f;
    std::optional<Color> icon_color_override_;
    IconCache icon_cache_;

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
    std::unique_ptr<TextLayout> cached_;
    Size                        cached_size_{};
};

// A thin, non-interactive horizontal progress indicator. Determinate mode
// fills the track proportionally to progress(); indeterminate mode
// animates a looping sweep instead, for operations with no meaningful
// total (e.g. a room-history export whose room-creation timestamp hasn't
// resolved, so there's no fraction to show). Self-animates while
// indeterminate by calling host()->request_repaint() at the end of every
// paint — the same "schedule the next frame from inside paint()" idiom
// ImageViewerOverlay's loading spinner uses, just via the built-in
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

private:
    bool  indeterminate_ = true;
    float value_ = 0.0f;
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};

    std::string                 label_;
    std::unique_ptr<TextLayout> label_layout_;
    Size                        label_size_{};
};

} // namespace tk
