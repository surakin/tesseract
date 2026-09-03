#pragma once

// tk::ComboBox — a labeled dropdown button. The dropdown is a real,
// standalone native popup surface (host()->make_popup_surface(), see
// host.h's PopupSurfaceHandle) rather than a canvas-drawn overlay — a
// canvas popup can never reliably occlude a native tk::TextField it happens
// to overlap, since a native control always composites above its own canvas
// parent's painted content regardless of paint()/paint_overlay() ordering,
// on every backend. Generalizes the same native-popup-surface pattern used
// by MentionPopup/SlashCommandPopup/ShortcodePopup/the Gif popup and (now)
// views::LanguagePicker.
//
// Still registers itself as the active Host popup (Host::register_popup)
// while open — not for painting (the dropdown draws in its own surface now)
// but purely for the "any click outside this widget dismisses it" behavior
// that mechanism already provides, including for a click that lands on some
// other widget entirely (which tk-level focus tracking alone wouldn't catch
// — see Host::dispatch_pointer_down's doc comment on why an ordinary click
// elsewhere doesn't always move/clear keyboard focus).

#include "animator.h"
#include "host.h"
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

protected:
    ComboBox() = default;
    TK_WIDGET_FACTORY_FRIEND(ComboBox)

public:
    void set_options(std::vector<Option> options);
    void set_selected_value(const std::string& value);
    const std::string& selected_value() const { return selected_value_; }

    // Optional static hover tooltip shown while the pointer rests over the
    // closed combo button (suppressed while the dropdown is expanded).
    // Empty (default) shows nothing.
    void set_tooltip(std::string text) { tooltip_text_ = std::move(text); }

    // Disabled: never expands the dropdown, drawn dimmed. Collapses the
    // dropdown if it happens to be open when disabled.
    void set_enabled(bool enabled) override;

    bool is_expanded() const { return expanded_; }
    void collapse();

    std::function<void(std::string value)> on_changed;

    Size     measure(LayoutCtx&, Size constraints) override;
    void     arrange(LayoutCtx&, Rect bounds) override;
    void     paint_before_children(PaintCtx&) override;
    void     on_theme_changed(const Theme& t) override;
    void     on_popup_dismiss() override;
    bool     on_pointer_down(Point local) override;
    void     on_pointer_up(Point local, bool inside_self) override;
    bool     on_pointer_move(Point local) override;
    void     on_pointer_leave() override;
    bool     on_wheel(Point local, float dx, float dy, bool is_touchpad = false) override;

    // Keyboard-focusable whenever enabled. Enter/Space/Up/Down opens the
    // dropdown; once open, Up/Down moves the highlighted option,
    // Enter/Space commits it, Escape cancels.
    bool focusable() const override
    {
        return enabled_;
    }
    bool on_key_down(const KeyEvent& e) override;

    Role access_role() const override
    {
        return Role::ComboBox;
    }
    // The selected option's display label, not its (possibly
    // non-human-readable) value — falls back to empty if selected_value_
    // doesn't match any current option (e.g. nothing selected yet).
    std::string access_name() const override
    {
        for (const auto& opt : options_)
            if (opt.value == selected_value_)
                return opt.label;
        return {};
    }
    AccessState access_state() const override
    {
        AccessState s;
        s.expanded = expanded_;
        return s;
    }
    // Toggle the dropdown, mirroring on_key_down's Enter/Space-when-
    // collapsed behavior (opens it) — collapse() is the natural symmetric
    // action when already open, matching the keyboard/Escape path.
    bool access_default_action() override
    {
        if (!enabled_)
            return false;
        if (expanded_)
            collapse();
        else
            set_expanded_(true);
        return true;
    }

private:
    class DropdownList; // popup surface's root widget — defined in the .cpp

    void set_expanded_(bool expanded);
    void commit_(std::size_t index);
    void set_hovered_(int index);
    void reposition_popup_();

    std::vector<Option> options_;
    std::string         selected_value_;
    std::string         tooltip_text_;

    // Button label layout only now — dropdown row layouts live in
    // DropdownList, inside the popup's own surface.
    std::unique_ptr<TextLayout> button_label_layout_;

    std::unique_ptr<PopupSurfaceHandle> popup_;
    DropdownList* dropdown_ = nullptr; // borrowed — owned by popup_ once set

    bool expanded_       = false;
    int  hovered_option_ = -1;
    bool button_hovered_ = false;
    bool button_pressed_ = false;

    Rect  button_rect_{};
    float last_w_ = -1.0f;

    FloatTween button_hover_fade_;

    static constexpr float kDropRowH = 32.0f;
};

} // namespace tk
