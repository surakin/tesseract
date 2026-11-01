#pragma once

// KeyboardTarget — an invisible, focusable stand-in for a hand-painted
// clickable area (a member row, a "Media" row, an avatar, a room-header
// title, ...), so keyboard users can Tab to it and activate it with
// Enter/Space without rewriting the owner's painting or mouse handling.
//
// The owner add_child()s one, arranges it over the painted area each
// layout pass, and wires on_activate to the exact callback its click path
// fires. The target paints nothing but its focus ring, and is transparent
// to every pointer event (contains_world() is always false), so the owner's
// existing hover/press/right-click handling keeps working unchanged —
// including the keyboard context-menu key, whose synthesised right-click at
// the target's centre falls through to the owner.
//
// Optional link cycling: set_links() makes Left/Right step through the
// links of a painted rich-text block (a topic) while the target has focus,
// announcing the current one in a tooltip; Enter then opens that link
// instead of activating. Escape returns to the plain activation.

#include "widget.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tk
{

struct TextSpan;

class KeyboardTarget : public Widget
{
protected:
    KeyboardTarget() = default;
    TK_WIDGET_FACTORY_FRIEND(KeyboardTarget)

public:
    // Fired on Enter/Space (or an AT default action). Unset = not focusable.
    std::function<void()> on_activate;
    // Fired for the keyboard context-menu key, when set; otherwise the key
    // falls through to the owner's on_right_click (see class comment).
    std::function<bool()> on_context_menu;
    // Fired when Enter opens a cycled link (set_links()).
    std::function<void(const std::string& url)> on_link_activated;

    void set_accessible_name(std::string name)
    {
        name_ = std::move(name);
    }
    void set_accessible_description(std::string d)
    {
        description_ = std::move(d);
    }
    void set_role(Role r)
    {
        role_ = r;
    }
    void set_focus_ring_radius(float r)
    {
        ring_radius_ = r;
    }
    // Places the target without a LayoutCtx — for owners that only learn
    // the painted rect during paint() (an empty rect = not focusable).
    void set_target_rect(Rect r)
    {
        bounds_ = r;
    }
    // Extra keys the owner wants while the target has focus (e.g. Delete on
    // a row). Return true to consume. Runs before the built-in handling.
    std::function<bool(const KeyEvent&)> on_key;

    // Links cycled by Left/Right (label, url). Resets the cycle position
    // when the list actually changes.
    void set_links(std::vector<std::pair<std::string, std::string>> links);
    // Convenience: every span carrying a url.
    void set_links_from_spans(const std::vector<TextSpan>& spans);
    // Index of the link currently selected by Left/Right, or -1.
    int link_index() const
    {
        return link_index_;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {};
    }
    void arrange(LayoutCtx&, Rect bounds) override
    {
        bounds_ = bounds;
    }
    void paint(PaintCtx&) override
    {
    }
    void paint_own_focus_ring(PaintCtx& ctx) override;

    // Pointer-transparent: no pointer dispatch ever lands here.
    bool contains_world(Point) const override
    {
        return false;
    }

    // Focusable with an activation, or with links to cycle (a topic block
    // whose only interactive content is its links).
    bool focusable() const override
    {
        return enabled_ && (static_cast<bool>(on_activate) || !links_.empty()) &&
               bounds_.w > 0.0f && bounds_.h > 0.0f;
    }
    bool focus_on_click() const override
    {
        return false;
    }
    bool on_key_down(const KeyEvent& e) override;
    bool on_context_menu_key() override
    {
        return on_context_menu && on_context_menu();
    }
    void on_focus_lost() override
    {
        link_index_ = -1;
    }

    Role access_role() const override
    {
        return role_;
    }
    std::string access_name() const override
    {
        return name_;
    }
    std::string access_description() const override
    {
        return description_;
    }
    bool access_default_action() override;

private:
    void step_link_(int dir);

    std::string name_;
    std::string description_;
    Role role_ = Role::Button;
    float ring_radius_ = 4.0f;
    std::vector<std::pair<std::string, std::string>> links_;
    int link_index_ = -1;
};

} // namespace tk
