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
    cancel_tooltip_(); // any click anywhere dismisses an active/pending tooltip
    Widget* root = input_root_();
    if (!root)
    {
        return;
    }
    if (auto p = popup_.lock())
    {
        if (p->contains_world(world))
        {
            if (p->on_pointer_down(p->world_to_local(world)))
            {
                pressed_widget_ = popup_;
                request_repaint();
            }
            return;
        }
        // Click outside the popup: dismiss it, then let the click through.
        p->on_popup_dismiss();
        popup_.reset();
    }
    Widget* pressed = root->dispatch_pointer_down(world);
    pressed_widget_ = track(pressed);
    if (pressed)
    {
        request_repaint();
    }
}

void Host::dispatch_pointer_up(Point world)
{
    auto w = pressed_widget_.lock();
    if (!w)
        return;
    pressed_widget_.reset();  // clear before callback (re-entrancy safe)
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
    if (auto p = pressed_widget_.lock())
    {
        Point ws = p->world_to_local(world);
        p->on_pointer_drag(ws);
        request_repaint();
        return;
    }
    // When a popup is open, route hover into it while the pointer is inside it;
    // the normal tree handles everything outside.
    if (auto p = popup_.lock(); p && p->contains_world(world))
    {
        // Clear any Button hover state — popup is above buttons.
        if (auto hb = hovered_btn_.lock())
        {
            hb->set_hovered(false);
            hovered_btn_.reset();
        }
        bool widget_changed = (p.get() != hovered_widget_.lock().get());
        if (widget_changed)
        {
            if (auto hw = hovered_widget_.lock())
                hw->on_pointer_leave();
            hovered_widget_ = p;
        }
        bool dirty = p->on_pointer_move(p->world_to_local(world));
        if (widget_changed || dirty)
            request_repaint();
        return;
    }

    Widget* hit = root->hit_test(world);
    Button* hovered = dynamic_cast<Button*>(hit);
    if (hovered && !hovered->enabled())
    {
        // hit_test() doesn't consider enabled_ — Button::paint() already
        // ignores hover for a disabled button, but keep hovered_btn_/
        // hovered() from ever reporting true for one too.
        hovered = nullptr;
    }
    bool btn_changed = (hovered != hovered_btn_.lock().get());
    if (btn_changed)
    {
        if (auto hb = hovered_btn_.lock())
        {
            hb->set_hovered(false);
        }
        hovered_btn_ = track(hovered);
        if (hovered)
        {
            hovered->set_hovered(true);
        }
    }
    // Non-Button widget-level hover dispatch (chip hover, etc.).
    bool dirty = false;
    Widget* moved = root->dispatch_pointer_move(world, &dirty);
    bool widget_changed = (moved != hovered_widget_.lock().get());
    if (widget_changed)
    {
        if (auto hw = hovered_widget_.lock())
        {
            hw->on_pointer_leave();
        }
        hovered_widget_ = track(moved);
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
    if (auto p = popup_.lock(); p && p->contains_world(world))
        return p->on_wheel(p->world_to_local(world), dx, dy);
    return root->dispatch_wheel(world, dx, dy);
}

Widget* Host::dispatch_file_drop(Point world, FileDropPayload& payload)
{
    Widget* root = input_root_();
    if (!root)
        return nullptr;
    Widget* target = root->dispatch_file_drop(world, payload);
    if (target)
        request_repaint();
    return target;
}

Widget* Host::dispatch_drag_hover(Point world)
{
    Widget* root = input_root_();
    if (!root)
        return nullptr;
    Widget* target = root->dispatch_drag_hover(world);
    bool changed = (target != drag_hovered_widget_.lock().get());
    if (changed)
    {
        if (auto d = drag_hovered_widget_.lock())
            d->on_drag_leave();
        drag_hovered_widget_ = track(target);
    }
    if (changed || target)
        request_repaint();
    return target;
}

void Host::dispatch_drag_leave()
{
    if (auto d = drag_hovered_widget_.lock())
    {
        d->on_drag_leave();
        drag_hovered_widget_.reset();
        request_repaint();
    }
}

void Host::dispatch_pointer_leave()
{
    if (auto hb = hovered_btn_.lock())
    {
        hb->set_hovered(false);
        hovered_btn_.reset();
    }
    if (auto hw = hovered_widget_.lock())
    {
        hw->on_pointer_leave();
        hovered_widget_.reset();
    }
    if (auto pw = pressed_widget_.lock())
    {
        // Synthetic pointer-up outside any widget so the captured widget gets a
        // chance to clean up its pressed state.
        pw->on_pointer_up({-1, -1}, false);
        pressed_widget_.reset();
    }
    request_repaint();
}

void Host::queue_for_deletion(std::unique_ptr<Widget> subtree)
{
    pending_deletions_.push_back(std::move(subtree));
    if (drain_scheduled_)
        return;
    drain_scheduled_ = true;
    // Guard against the Host itself being destroyed before this posted
    // closure runs — mirrors the still_alive pattern in dispatch_pointer_up.
    std::weak_ptr<bool> alive = dispatch_alive_;
    post_to_ui([this, alive]
    {
        if (alive.expired())
            return;
        drain_deferred_deletions_();
    });
}

void Host::drain_deferred_deletions_()
{
    drain_scheduled_ = false;
    // Move out before destroying so a widget destructor that reentrantly
    // triggers another queue_for_deletion() call appends to a fresh queue
    // rather than the one currently being torn down.
    std::vector<std::unique_ptr<Widget>> doomed = std::move(pending_deletions_);
    pending_deletions_.clear();
    doomed.clear();
}

// ── Tooltip management ──────────────────────────────────────────────────────

void Host::show_tooltip(const void* owner, std::string text, Rect anchor_world)
{
    if (!popup_.expired()) return; // a real popup is open — tooltips are suppressed entirely
    if (owner == tooltip_owner_)
    {
        // Same owner: refresh content/anchor. If already visible, take effect
        // immediately; if still pending, the original delay keeps running.
        // Only repaint when something actually changed — callers like
        // TabbedGridPicker re-assert the tooltip unconditionally on every
        // paint() frame (no hover-transition event of their own), so an
        // unconditional repaint here would self-sustain a paint loop for as
        // long as the pointer sits still. Harmless on toolkits that
        // coalesce redundant invalidations, but fatal on Win32: raw
        // InvalidateRect during WM_PAINT re-arms immediately and starves
        // the thread-wide WM_TIMER driving animation frame ticks.
        const bool changed = text != tooltip_text_ ||
            anchor_world.x != tooltip_anchor_.x ||
            anchor_world.y != tooltip_anchor_.y ||
            anchor_world.w != tooltip_anchor_.w ||
            anchor_world.h != tooltip_anchor_.h;
        tooltip_text_ = std::move(text);
        tooltip_anchor_ = anchor_world;
        if (tooltip_visible_ && changed) request_repaint();
        return;
    }
    // New owner — reset and re-arm the dwell delay.
    tooltip_owner_   = owner;
    tooltip_text_    = std::move(text);
    tooltip_anchor_  = anchor_world;
    tooltip_visible_ = false;
    const auto gen = ++tooltip_gen_;
    std::weak_ptr<bool> weak = tooltip_alive_;
    post_delayed(kTooltipShowDelayMs, [this, gen, owner, weak] {
        if (!weak.lock()) return;
        if (gen != tooltip_gen_ || owner != tooltip_owner_) return; // superseded/cancelled
        tooltip_visible_ = true;
        request_repaint();
    });
}

void Host::hide_tooltip(const void* owner)
{
    if (owner != tooltip_owner_) return;
    cancel_tooltip_();
}

void Host::update_tooltip_text(const void* owner, std::string text)
{
    if (!popup_.expired()) return;
    if (owner != tooltip_owner_)
    {
        if (tooltip_owner_ != nullptr) return; // someone else owns it — not ours to steal
        tooltip_owner_ = owner; // adopt: caller asserts it's genuinely hovered right now
    }
    tooltip_text_    = std::move(text);
    tooltip_visible_ = true;
    request_repaint();
}

void Host::cancel_tooltip_()
{
    if (tooltip_owner_ == nullptr && !tooltip_visible_ && tooltip_text_.empty())
        return; // nothing active/pending — avoid a spurious repaint request
    ++tooltip_gen_; // invalidates any in-flight show timer
    tooltip_owner_   = nullptr;
    tooltip_visible_ = false;
    tooltip_text_.clear();
    request_repaint();
}

void Host::paint_tooltip_overlay(PaintCtx& ctx, Rect surface_bounds)
{
    if (!tooltip_visible_ || tooltip_text_.empty()) return;
    if (!tooltip_widget_) tooltip_widget_ = std::make_unique<Tooltip>();
    tooltip_widget_->set_content(tooltip_text_, tooltip_anchor_);
    tooltip_widget_->paint_overlay(ctx, surface_bounds);
}

} // namespace tk
