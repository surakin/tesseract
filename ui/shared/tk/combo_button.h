#pragma once

// tk::ComboButton — a split button: a main click zone that immediately
// performs the currently selected action, plus a narrow chevron zone that
// opens a small dropdown to pick a different action (which both performs it
// now and becomes the new default for the next plain click). Modeled on
// tk::ComboBox (combobox.h) for the dropdown mechanics — same real,
// standalone native popup surface (host()->make_popup_surface(), see
// host.h's PopupSurfaceHandle) rather than a canvas-drawn overlay, for the
// same reason ComboBox uses one: a native control elsewhere on screen always
// composites above its own canvas parent regardless of paint() ordering, so
// a canvas-drawn popup can never reliably occlude it.
//
// Unlike ComboBox, the main body is a live action button, not just a way to
// open the dropdown — clicking it fires on_activate(selected_value())
// directly, the way tk::Button::click() would. Picking a dropdown row only
// changes which option is selected (fires on_selection_changed) — it does
// not itself activate. That split matters for a caller like CreateRoomView:
// the combo picks a *mode* ("Create Room" vs "Create Space") for a form the
// user still has to fill in and submit, so choosing "Create Space" from the
// menu must not fire the action before they've had a chance to edit
// anything else.

#include "animator.h"
#include "controls.h"
#include "host.h"
#include "widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

class ComboButton : public Widget
{
public:
    struct Option
    {
        std::string label;
        std::string value;
    };

protected:
    ComboButton() = default;
    TK_WIDGET_FACTORY_FRIEND(ComboButton)

public:
    void set_options(std::vector<Option> options);
    void set_selected_value(const std::string& value);
    const std::string& selected_value() const { return selected_value_; }

    void set_variant(Button::Variant v)
    {
        variant_ = v;
        invalidate_cache_();
    }

    // Disabled: never activates or expands the dropdown, drawn dimmed.
    // Collapses the dropdown if it happens to be open when disabled.
    void set_enabled(bool enabled) override;

    bool is_expanded() const { return expanded_; }
    void collapse();

    // Fires with selected_value() when the main zone is clicked (or
    // Enter/Space activates it while collapsed) — the live "do it" action.
    std::function<void(std::string value)> on_activate;
    // Fires with the new selected_value() when a dropdown row is picked
    // (mouse or keyboard) — a pure mode change, not an activation.
    std::function<void(std::string value)> on_selection_changed;

    Size     measure(LayoutCtx&, Size constraints) override;
    void     arrange(LayoutCtx&, Rect bounds) override;
    void     paint_before_children(PaintCtx&) override;
    void     on_theme_changed(const Theme& t) override;
    void     on_popup_dismiss() override;
    bool     on_pointer_down(Point local) override;
    void     on_pointer_up(Point local, bool inside_self) override;
    bool     on_pointer_move(Point local) override;
    void     on_pointer_leave() override;

    // Keyboard-focusable whenever enabled. Enter/Space activates the main
    // (selected) action; Down opens the dropdown; once open, Up/Down moves
    // the highlighted option, Enter/Space commits it, Escape cancels.
    bool focusable() const override
    {
        return enabled_;
    }
    bool on_key_down(const KeyEvent& e) override;

    Role access_role() const override
    {
        return Role::Button;
    }
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
    bool access_default_action() override
    {
        if (!enabled_)
            return false;
        activate_();
        return true;
    }

private:
    class DropdownList; // popup surface's root widget — defined in the .cpp

    void invalidate_cache_()
    {
        main_label_layout_.reset();
    }

    void activate_();
    void set_expanded_(bool expanded);
    void commit_(std::size_t index);
    void set_hovered_(int index);
    void reposition_popup_();

    // Which zone (if any) world point `w` falls in.
    bool in_main_zone_(Point w) const;
    bool in_chevron_zone_(Point w) const;

    std::vector<Option> options_;
    std::string         selected_value_;
    Button::Variant      variant_ = Button::Variant::Primary;

    std::unique_ptr<TextLayout> main_label_layout_;

    std::unique_ptr<PopupSurfaceHandle> popup_;
    DropdownList* dropdown_ = nullptr; // borrowed — owned by popup_ once set

    bool expanded_ = false;
    int  hovered_option_ = -1;

    bool main_hovered_ = false;
    bool main_pressed_ = false;
    bool chevron_hovered_ = false;
    bool chevron_pressed_ = false;

    Rect  main_rect_{};
    Rect  chevron_rect_{};
    float last_w_ = -1.0f;

    FloatTween main_hover_fade_;
    FloatTween chevron_hover_fade_;

    static constexpr float kDropRowH = 32.0f;
};

} // namespace tk
