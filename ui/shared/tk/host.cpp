#include "host.h"

#include "controls.h" // tk::Button (dynamic_cast + set_hovered in hover tracking)
#include "widget.h"

// Shared pointer-dispatch state machine + popup input/hover routing for every
// tk::Host backend. Each native host (host_qt.cpp / host_gtk.cpp /
// host_win32.cpp / host_macos.mm) translates its platform event to a
// world-space Point — and performs any native capture/grab step
// (Win32 SetCapture/ReleaseCapture) — then calls into here. Keeping the logic
// in one place means drag capture, hover, and popup dismiss/routing behave
// identically on all four backends.

namespace tk
{

// Pointer-event dispatch. We keep simple capture semantics: a pointer-down on
// a Button stamps it as the captured widget; the matching up-on-the-same-button
// fires its click. This isn't a generic capture protocol — it's the minimum
// LoginView needs.
void Host::dispatch_pointer_down(Point world)
{
    fire_user_activity_();
    Widget* root = input_root_();
    if (!root)
    {
        return;
    }
    if (popup_)
    {
        if (popup_->contains_world(world))
        {
            if (popup_->on_pointer_down(popup_->world_to_local(world)))
            {
                pressed_widget_ = popup_;
                request_repaint();
            }
            return;
        }
        // Click outside the popup: dismiss it, then let the click through.
        popup_->on_popup_dismiss();
        popup_ = nullptr;
    }
    pressed_widget_ = root->dispatch_pointer_down(world);
    if (pressed_widget_)
    {
        request_repaint();
    }
}

void Host::dispatch_pointer_up(Point world)
{
    if (!pressed_widget_)
        return;
    Widget* w = pressed_widget_;
    pressed_widget_ = nullptr;  // clear before callback (re-entrancy safe)
    Point ws = w->world_to_local(world);
    bool inside = (ws.x >= 0 && ws.y >= 0 &&
                   ws.x < w->bounds().w && ws.y < w->bounds().h);
    // Guard: if the callback destroys this host (e.g. hang-up → call_window_.reset()),
    // dispatch_alive_ is freed and the weak_ptr expires — do not touch `this` after.
    std::weak_ptr<bool> still_alive = dispatch_alive_;
    w->on_pointer_up(ws, inside);
    if (!still_alive.expired())
        request_repaint();
}

void Host::dispatch_pointer_move(Point world)
{
    Widget* root = input_root_();
    if (!root)
    {
        return;
    }
    // If a widget claimed the last pointer-down, all subsequent moves go to it
    // as a drag (this is how ListView's scrollbar thumb tracks a held mouse).
    // Hover updates are suspended for the duration of the drag.
    if (pressed_widget_)
    {
        Point ws = pressed_widget_->world_to_local(world);
        pressed_widget_->on_pointer_drag(ws);
        request_repaint();
        return;
    }
    // When a popup is open, route hover into it while the pointer is inside it;
    // the normal tree handles everything outside.
    if (popup_ && popup_->contains_world(world))
    {
        // Clear any Button hover state — popup is above buttons.
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(false);
            hovered_btn_ = nullptr;
        }
        bool widget_changed = (popup_ != hovered_widget_);
        if (widget_changed)
        {
            if (hovered_widget_)
                hovered_widget_->on_pointer_leave();
            hovered_widget_ = popup_;
        }
        bool dirty = popup_->on_pointer_move(popup_->world_to_local(world));
        if (widget_changed || dirty)
            request_repaint();
        return;
    }

    Widget* hit = root->hit_test(world);
    Button* hovered = dynamic_cast<Button*>(hit);
    bool btn_changed = (hovered != hovered_btn_);
    if (btn_changed)
    {
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(false);
        }
        hovered_btn_ = hovered;
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(true);
        }
    }
    // Non-Button widget-level hover dispatch (chip hover, etc.).
    bool dirty = false;
    Widget* moved = root->dispatch_pointer_move(world, &dirty);
    bool widget_changed = (moved != hovered_widget_);
    if (widget_changed)
    {
        if (hovered_widget_)
        {
            hovered_widget_->on_pointer_leave();
        }
        hovered_widget_ = moved;
    }
    if (btn_changed || widget_changed || dirty)
    {
        request_repaint();
    }
}

bool Host::dispatch_wheel(Point world, float dx, float dy)
{
    Widget* root = input_root_();
    if (!root)
        return false;
    if (popup_ && popup_->contains_world(world))
        return popup_->on_wheel(popup_->world_to_local(world), dx, dy);
    return root->dispatch_wheel(world, dx, dy);
}

void Host::dispatch_pointer_leave()
{
    if (hovered_btn_)
    {
        hovered_btn_->set_hovered(false);
        hovered_btn_ = nullptr;
    }
    if (hovered_widget_)
    {
        hovered_widget_->on_pointer_leave();
        hovered_widget_ = nullptr;
    }
    if (pressed_widget_)
    {
        // Synthetic pointer-up outside any widget so the captured widget gets a
        // chance to clean up its pressed state.
        pressed_widget_->on_pointer_up({-1, -1}, false);
        pressed_widget_ = nullptr;
    }
    request_repaint();
}

void Host::on_subtree_removing(Widget* subtree)
{
    // Walk up from a tracked widget to see if it lives inside the subtree
    // being removed. Called before the subtree is freed so the check is safe.
    auto in_subtree = [&](Widget* w) -> bool {
        while (w) {
            if (w == subtree) return true;
            w = w->parent();
        }
        return false;
    };
    if (pressed_widget_ && in_subtree(pressed_widget_)) {
        pressed_widget_->on_pointer_up({-1, -1}, false);
        pressed_widget_ = nullptr;
    }
    if (hovered_btn_ && in_subtree(hovered_btn_)) {
        hovered_btn_->set_hovered(false);
        hovered_btn_ = nullptr;
    }
    if (hovered_widget_ && in_subtree(hovered_widget_)) {
        // Skip on_pointer_leave(): the call overlay's child widgets may already
        // be partially torn down when this fires during unmount.
        hovered_widget_ = nullptr;
    }
}

} // namespace tk
