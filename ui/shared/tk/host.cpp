#include "host.h"

#include <memory>

#include "access_tree.h" // find_topmost_modal (modal-scoped Tab traversal)
#include "key_chord.h"
#include "scrollable_base.h" // keyboard paging fallback (scroll_by_key)
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

// Right-click — see host.h. Popup first, like dispatch_pointer_down below.
Widget* Host::dispatch_right_click(Point world)
{
    fire_user_activity_();
    cancel_tooltip_();
    if (auto p = popup_.lock(); p && p->contains_world(world))
    {
        Widget* hit = p->dispatch_right_click(world);
        request_repaint();
        return hit ? hit : p.get();
    }
    Widget* root = input_root_();
    return root ? root->dispatch_right_click(world) : nullptr;
}

// Pointer-event dispatch. We keep simple capture semantics: a pointer-down on
// a Button stamps it as the captured widget; the matching up-on-the-same-button
// fires its click. This isn't a generic capture protocol — it's the minimum
// LoginView needs.
void Host::dispatch_pointer_down(Point world)
{
    fire_user_activity_();
    // A press while the scrolled view is still coasting only stops it: the
    // content under the pointer is moving, so activating whatever slid there
    // would be an accident. The matching release is swallowed too.
    if (Widget* latched = live_wheel_latch_target_(world))
    {
        const bool coasting = latched->wheel_motion_active();
        wheel_latch_.reset();
        if (coasting)
        {
            latched->stop_wheel_motion();
            swallow_pointer_up_ = true;
            request_repaint();
            return;
        }
    }
    wheel_latch_.reset();
    cancel_tooltip_();      // any click anywhere dismisses an active/pending tooltip
    focus_visible_ = false; // mouse navigation doesn't need a focus ring
    Widget* root = input_root_();
    if (!root)
    {
        return;
    }
    if (auto p = popup_.lock())
    {
        if (p->contains_world(world))
        {
            // Route through the recursive dispatch, not the leaf-only
            // on_pointer_down — a popup with real add_child'd children
            // (e.g. TabbedGridPicker's search field / grid) needs the
            // click to reach whichever child is actually hit, exactly
            // like the normal (non-popup) tree does. Widgets with no
            // children (DatePickerView, ComboBox's dropdown) see identical
            // behavior: dispatch_pointer_down's child loop is a no-op and
            // it falls straight through to on_pointer_down.
            if (Widget* hit = p->dispatch_pointer_down(world))
            {
                pressed_widget_ = track(hit);
                request_repaint();
            }
            return;
        }
        // Click outside the popup — unless it landed on the popup's own
        // registered trigger control (see register_popup()'s doc comment):
        // that control's own click handler already knows the popup is open
        // and decides what to do, so skip the dismiss here and just fall
        // through to the normal dispatch below, which presses the trigger
        // exactly as if no popup were open. hit_test() is a pure read-only
        // query with the same traversal order as dispatch_pointer_down()'s
        // own hit-test, so it agrees on which widget would be pressed.
        Widget* trigger = popup_trigger_.lock().get();
        if (!trigger || !trigger->enabled() || root->hit_test(world) != trigger)
        {
            // Dismiss it, then let the click through. Reset popup_ BEFORE
            // calling on_popup_dismiss() — the dismiss callback commonly
            // re-focuses some other widget (e.g. RoomView::
            // hide_pickers_() calls compose_bar_->focus()), which reaches
            // request_focus() below and re-checks popup_; if it were still
            // set at that point, request_focus would try to dismiss it all
            // over again — unbounded recursion.
            popup_.reset();
            p->on_popup_dismiss();
        }
    }
    Widget* pressed = root->dispatch_pointer_down(world);
    pressed_widget_ = track(pressed);
    if (pressed)
    {
        request_repaint();
    }
    // A click on a focusable canvas widget moves tk-level keyboard focus
    // there — unless that widget opts out via focus_on_click() (see
    // Widget::focus_on_click's doc comment: e.g. RoomListView's inner row
    // list, which must stay focusable() for Tab/arrow-key row navigation
    // but whose ordinary mouse click already performs a complete action of
    // its own and shouldn't also steal focus from the compose box). A
    // click that claims nothing anywhere in the whole tree (truly empty
    // space) clears focus. A click that lands on some OTHER widget — one
    // that claims the press for its own reasons (hover state, a
    // dismiss-on-click background, a text-selection anchor, an avatar) but
    // isn't itself a focus target — deliberately does neither: almost every
    // widget in a real application claims clicks for reasons unrelated to
    // keyboard focus, and blurring the previously-focused control just
    // because some unrelated click landed elsewhere is rarely what's
    // wanted (this used to be the default, and needed ~40 scattered
    // "refocus the compose box after X" call sites across every shell to
    // work around it — those calls are now harmless/idempotent, not
    // load-bearing). Inert until some widget actually overrides
    // focusable() — see Widget::focusable()'s default in widget.h.
    if (pressed && pressed->focusable() && pressed->focus_on_click())
    {
        request_focus(pressed);
    }
    else if (!pressed)
    {
        clear_focus();
    }
}

void Host::dispatch_pointer_up(Point world)
{
    if (swallow_pointer_up_)
    {
        swallow_pointer_up_ = false;
        return;
    }
    if (active_drag_)
    {
        Widget* target = active_drag_->current_target.lock().get();
        DragPayload payload = std::move(active_drag_->payload);
        Point local{};
        if (target)
        {
            local = target->world_to_local(world);
        }
        active_drag_.reset(); // clear before callback (re-entrancy safe)
        pressed_widget_.reset();
        if (target)
        {
            target->on_drop(local, std::move(payload));
        }
        request_repaint();
        return;
    }
    auto w = pressed_widget_.lock();
    if (!w)
    {
        return;
    }
    pressed_widget_.reset(); // clear before callback (re-entrancy safe)
    Point ws = w->world_to_local(world);
    bool inside = (ws.x >= 0 && ws.y >= 0 && ws.x < w->bounds().w && ws.y < w->bounds().h);
    // Guard: if the callback destroys this host (e.g. hang-up → call_window_.reset()),
    // the weak flag expires — do not touch `this` after.
    std::weak_ptr<bool> still_alive = weak_flag();
    w->on_pointer_up(ws, inside);
    if (!still_alive.expired())
    {
        request_repaint();
    }
}

void Host::dispatch_pointer_move(Point world)
{
    if (wheel_latch_)
    {
        const float dx = world.x - wheel_latch_->anchor.x;
        const float dy = world.y - wheel_latch_->anchor.y;
        if (dx * dx + dy * dy > kWheelLatchMoveSlop * kWheelLatchMoveSlop)
        {
            wheel_latch_.reset();
        }
    }
    Widget* root = input_root_();
    if (!root)
    {
        return;
    }
    // While an in-app drag (begin_drag()) is active, moves retarget drop
    // candidates instead of reaching the widget pressed_widget_ still holds
    // (that widget stops receiving on_pointer_drag once a drag has begun).
    // This is a distinct branch from the pressed_widget_ hover-suspension
    // below, so retargeting still works even though normal hover dispatch
    // stays suspended for the whole gesture.
    if (active_drag_)
    {
        active_drag_->cursor_world = world;
        Widget* current = active_drag_->current_target.lock().get();
        // Only re-run the claim-bubble walk (which calls on_drag_enter on
        // whatever it resolves to, per its claim-testing contract) when the
        // current target no longer contains the cursor — otherwise the same
        // still-current target would see a spurious repeat on_drag_enter on
        // every move instead of the "fires once" contract its doc promises.
        if (current && current->contains_world(world))
        {
            current->on_drag_over(current->world_to_local(world), active_drag_->payload);
            request_repaint();
            return;
        }
        Widget* target = root->dispatch_drag_enter(world, active_drag_->payload);
        if (target != current)
        {
            if (current)
            {
                current->on_drag_leave_target();
            }
            active_drag_->current_target = track(target);
        }
        if (target)
        {
            target->on_drag_over(target->world_to_local(world), active_drag_->payload);
        }
        request_repaint();
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
        // The popup is above everything else: hover feedback comes from the
        // popup's own subtree, never from the tree behind it.
        bool fb_changed = false;
        const CursorShape want =
            begin_hover_feedback_(hover_target_for_(p->hit_test(world)), &fb_changed);
        // Recursive dispatch (see dispatch_pointer_down's comment above) so
        // hover reaches whichever child of the popup is actually under the
        // pointer, not just the popup widget itself.
        bool dirty = false;
        Widget* moved = p->dispatch_pointer_move(world, &dirty);
        bool widget_changed = (moved != hovered_widget_.lock().get());
        if (widget_changed)
        {
            if (auto hw = hovered_widget_.lock())
            {
                hw->on_pointer_leave();
            }
            hovered_widget_ = track(moved);
        }
        end_hover_feedback_(want);
        if (fb_changed || widget_changed || dirty)
        {
            request_repaint();
        }
        return;
    }

    bool fb_changed = false;
    const CursorShape want =
        begin_hover_feedback_(hover_target_for_(root->hit_test(world)), &fb_changed);
    // Widget-level hover/move dispatch (views that track their own regions).
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
    end_hover_feedback_(want);
    if (fb_changed || widget_changed || dirty)
    {
        request_repaint();
    }
}

// The widget that should show hover for a pointer over `hit`: the nearest
// hoverable ancestor-or-self. A disabled hit widget is opaque to input (see
// Widget::hit_test), so it shows no hover and neither do its ancestors.
Widget* Host::hover_target_for_(Widget* hit)
{
    if (!hit || !hit->enabled())
    {
        return nullptr;
    }
    for (Widget* w = hit; w; w = w->parent())
    {
        if (w->hoverable())
        {
            return w->enabled() ? w : nullptr;
        }
    }
    return nullptr;
}

// First half of the hover-feedback update, run BEFORE the widget-level move
// dispatch: moves hover to `target` and releases the cursor if the new target
// wants none, so a view that sets its own (link) cursor during the dispatch
// that follows is not overwritten. Returns the cursor `target` wants, to be
// applied by end_hover_feedback_() AFTER the dispatch (and after the previous
// hovered widget's on_pointer_leave, which may clear link cursors).
CursorShape Host::begin_hover_feedback_(Widget* target, bool* changed)
{
    if (target != hover_feedback_.lock().get())
    {
        if (changed)
        {
            *changed = true;
        }
        if (auto old = hover_feedback_.lock())
        {
            old->set_hovered(false);
        }
        hover_feedback_ = track(target);
        if (target)
        {
            target->set_hovered(true);
        }
    }
    const CursorShape want = target ? target->cursor() : CursorShape::Default;
    if (want == CursorShape::Default && applied_cursor_ != CursorShape::Default)
    {
        applied_cursor_ = CursorShape::Default;
        apply_cursor_(CursorShape::Default);
    }
    return want;
}

void Host::revalidate_hover_feedback_()
{
    if (hover_feedback_.expired())
    {
        return;
    }
    auto target = hover_feedback_.lock();
    if (!target)
    {
        return;
    }
    bool inert = !target->visible();
    for (Widget* w = target.get(); w && !inert; w = w->parent())
    {
        inert = !w->enabled();
    }
    if (!inert)
    {
        return;
    }
    // Reset before notifying: on_hover_leave may itself hide/disable widgets
    // and re-enter here.
    hover_feedback_.reset();
    target->set_hovered(false);
    if (applied_cursor_ != CursorShape::Default)
    {
        applied_cursor_ = CursorShape::Default;
        apply_cursor_(CursorShape::Default);
    }
    request_repaint();
}

void Host::end_hover_feedback_(CursorShape want)
{
    if (want != CursorShape::Default && want != applied_cursor_)
    {
        applied_cursor_ = want;
        apply_cursor_(want);
    }
}

Widget* Host::live_wheel_latch_target_(Point world)
{
    if (!wheel_latch_)
    {
        return nullptr;
    }
    Widget* t = wheel_latch_->target.lock().get();
    bool live = t && t->visible() && t->enabled();
    if (live)
    {
        // A popup under the pointer owns the wheel; a latch from outside it
        // must not reach through.
        if (auto p = popup_.lock(); p && p->contains_world(world))
        {
            live = false;
            for (Widget* w = t; w; w = w->parent())
            {
                if (w == p.get())
                {
                    live = true;
                    break;
                }
            }
        }
    }
    if (!live)
    {
        wheel_latch_.reset();
        return nullptr;
    }
    return t;
}

bool Host::dispatch_wheel(Point world, float dx, float dy, bool is_touchpad)
{
    Widget* root = input_root_();
    if (!root)
    {
        return false;
    }
    const auto now = wheel_clock_now_();

    // Mid-gesture: the widget that took the gesture keeps it, even when it
    // has nothing left to give (a scroller pinned at its bound must not hand
    // the rest of the gesture to whatever slid under the pointer).
    if (Widget* latched = live_wheel_latch_target_(world))
    {
        if (now - wheel_latch_->last < kWheelLatchQuiet || latched->wheel_motion_active())
        {
            wheel_latch_->anchor = world;
            wheel_latch_->last = now;
            latched->on_wheel(latched->world_to_local(world), dx, dy, is_touchpad);
            return true;
        }
        wheel_latch_.reset();
    }

    wheel_consumer_ = nullptr;
    bool consumed = false;
    if (auto p = popup_.lock(); p && p->contains_world(world))
    {
        // Recursive dispatch (see dispatch_pointer_down's comment above) so
        // a scroll over e.g. TabbedGridPicker's grid reaches the grid's own
        // wheel handling, not just the popup's own on_wheel.
        consumed = p->dispatch_wheel(world, dx, dy, is_touchpad);
    }
    else
    {
        consumed = root->dispatch_wheel(world, dx, dy, is_touchpad);
    }

    if (consumed && wheel_consumer_)
    {
        wheel_latch_ = WheelLatch{track(wheel_consumer_), world, now};
    }
    else
    {
        wheel_latch_.reset();
    }
    wheel_consumer_ = nullptr;
    return consumed;
}

Widget* Host::dispatch_file_drop(Point world, FileDropPayload& payload)
{
    Widget* root = input_root_();
    if (!root)
    {
        return nullptr;
    }
    Widget* target = root->dispatch_file_drop(world, payload);
    if (target)
    {
        request_repaint();
    }
    return target;
}

Widget* Host::dispatch_native_drag_hover(Point world)
{
    Widget* root = input_root_();
    if (!root)
    {
        return nullptr;
    }
    Widget* target = root->dispatch_native_drag_hover(world);
    bool changed = (target != native_drag_hovered_widget_.lock().get());
    if (changed)
    {
        if (auto d = native_drag_hovered_widget_.lock())
        {
            d->on_native_drag_leave();
        }
        native_drag_hovered_widget_ = track(target);
    }
    if (changed || target)
    {
        request_repaint();
    }
    return target;
}

void Host::dispatch_native_drag_leave()
{
    if (auto d = native_drag_hovered_widget_.lock())
    {
        d->on_native_drag_leave();
        native_drag_hovered_widget_.reset();
        request_repaint();
    }
}

void Host::begin_drag(DragPayload payload, DragVisual visual, Point source_local)
{
    // Convert the caller-local point to world space using whatever widget
    // is still holding pointer capture (pressed_widget_) — begin_drag() is
    // only ever called from within that widget's own on_pointer_down/
    // on_pointer_drag, so it is always the caller here.
    Point world = source_local;
    if (auto p = pressed_widget_.lock())
    {
        Rect b = p->bounds();
        world = Point{b.x + source_local.x, b.y + source_local.y};
    }
    active_drag_ =
        std::make_unique<ActiveDrag>(ActiveDrag{std::move(payload), std::move(visual), world, {}});
    request_repaint();
}

void Host::cancel_drag()
{
    if (!active_drag_)
    {
        return;
    }
    if (auto t = active_drag_->current_target.lock())
    {
        t->on_drag_leave_target();
    }
    active_drag_.reset();
    pressed_widget_.reset();
    request_repaint();
}

void Host::paint_drag_overlay(PaintCtx& ctx, Rect /*surface_bounds*/)
{
    if (!active_drag_ || !active_drag_->visual.image)
    {
        return;
    }
    const DragVisual& visual = active_drag_->visual;
    Point cursor = active_drag_->cursor_world;
    // Image::width()/height() are device pixels (the visual is rendered at the
    // surface's scale factor, see CanvasFactory::create_offscreen), while
    // `dst` and the hotspot are logical: divide the scale back out, or the
    // ghost is drawn scale-times too big on a HiDPI display.
    const float scale = ctx.canvas.scale_factor() > 0.0f ? ctx.canvas.scale_factor() : 1.0f;
    Rect dst{cursor.x - visual.hotspot.x, cursor.y - visual.hotspot.y,
             static_cast<float>(visual.image->width()) / scale,
             static_cast<float>(visual.image->height()) / scale};
    OpacityScope fade(ctx.canvas, visual.opacity);
    ctx.canvas.draw_image(*visual.image, dst);
}

void Host::dispatch_pointer_leave()
{
    wheel_latch_.reset();
    swallow_pointer_up_ = false;
    // A drag in progress is cancelled by the pointer leaving the surface
    // entirely (no OS-level drag to hand off to, per design). cancel_drag()
    // already resets pressed_widget_, so the pressed-widget cleanup below
    // becomes a no-op afterward.
    cancel_drag();
    if (auto hf = hover_feedback_.lock())
    {
        hf->set_hovered(false);
        hover_feedback_.reset();
    }
    if (applied_cursor_ != CursorShape::Default)
    {
        applied_cursor_ = CursorShape::Default;
        apply_cursor_(CursorShape::Default);
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

// ── Canvas-level keyboard focus ─────────────────────────────────────────────

void Host::request_focus(Widget* w)
{
    if (!w || !w->focusable() || !w->enabled() || !w->visible_in_tree())
    {
        return;
    }
    // Focus landing outside the popup dismisses it, exactly like an outside
    // click would (dispatch_pointer_down, above). This is the only place
    // that catches a click on a native text field/area while a popup is
    // open: that click bypasses canvas hit-testing entirely (the native
    // overlay eats it at the OS level — see TextArea/TextField's
    // set_on_focus_changed sync) and never reaches dispatch_pointer_down's
    // own popup-contains check. Walk up from `w` rather than comparing
    // directly, since a hit inside the popup is usually one of its real
    // children (e.g. TabbedGridPicker's search field), not the popup widget
    // itself.
    if (auto p = popup_.lock(); p && popup_keep_open_depth_ == 0)
    {
        bool inside_popup = false;
        for (Widget* a = w; a; a = a->parent())
        {
            if (a == p.get())
            {
                inside_popup = true;
                break;
            }
        }
        // Also exempt the popup's own registered trigger (see
        // register_popup()'s doc comment) — a click on it lands here too:
        // dispatch_pointer_down() skips its own dismiss for the trigger, but
        // Button::focusable()/focus_on_click() default to true, so the
        // click's own request_focus(pressed) call below reaches this same
        // popup-outside check independently, on the same click, moments
        // later. Without this, that second check would dismiss the popup
        // anyway right before the trigger's own on_click handler runs.
        Widget* trigger = popup_trigger_.lock().get();
        if (!inside_popup && (!trigger || w != trigger))
        {
            // Reset before calling — the dismiss callback commonly
            // re-focuses some other widget (e.g. RoomView::hide_pickers_()
            // calls compose_bar_->focus()), which reenters request_focus();
            // if popup_ were still set at that point this branch would fire
            // again on every reentrant call, recursing without end.
            popup_.reset();
            p->on_popup_dismiss();
        }
    }
    Widget* old = focused_widget_.lock().get();
    if (old != w)
    {
        if (old)
        {
            old->set_focused_(false);
            old->on_focus_lost();
        }
        focused_widget_ = track(w);
        w->set_focused_(true);
        // Focus is landing on a genuinely different widget. Only Tab/
        // Shift-Tab traversal (advance_focus_(), which reasserts this right
        // after calling us) should show a ring for that new widget — every
        // other path here (a real mouse click, or a programmatic focus()
        // call like ComposeBar::focus() after switching rooms via the quick
        // switcher) must not inherit a `true` left over from an unrelated
        // earlier keypress.
        focus_visible_ = false;
        on_focus_changed_(old, w);
    }
    // Always re-assert focus-gained, even when `w` was already the tracked
    // tk-level focus target: real native/OS keyboard focus can drift away
    // independently of this bookkeeping — e.g. a platform surface widget
    // unconditionally grabbing native focus for itself as part of its own
    // default mouse-down handling (Qt's Surface::mousePressEvent calls
    // setFocus() on itself before our own dispatch even runs), without
    // ever going through clear_focus(). Early-returning here used to leave
    // tk-level state "correct" while real keyboard input silently went
    // nowhere. Safe to call repeatedly: TextField/TextArea's
    // on_focus_gained() already guards its own native set_focused() call
    // against a reentrant native echo via syncing_from_native_, and is
    // otherwise idempotent.
    w->on_focus_gained();
    // `w` has no native OS control of its own (holds_native_focus() ==
    // false) to hold real keyboard focus — e.g. a plain Button reached via
    // Tab from a native text field, which just released real OS focus via
    // its own on_focus_lost() above. Ask the backend to park real focus on
    // its canvas-hosting container so native key events (the very next
    // Tab, Enter, ...) keep reaching Host::dispatch_key_down instead of
    // dangling nowhere. See claim_native_focus_container_()'s doc comment.
    if (!w->holds_native_focus())
    {
        claim_native_focus_container_();
    }
    scroll_widget_into_view(w);
    request_repaint();
}

void Host::clear_focus()
{
    if (auto w = focused_widget_.lock())
    {
        w->set_focused_(false);
        w->on_focus_lost();
        focused_widget_.reset();
        request_repaint();
        on_focus_changed_(w.get(), nullptr);
    }
}

void Host::release_focus_to_canvas()
{
    if (auto w = focused_widget_.lock())
    {
        w->set_focused_(false);
        w->on_focus_lost();
        focused_widget_.reset();
        request_repaint();
        on_focus_changed_(w.get(), nullptr);
    }
    claim_native_focus_container_();
}

bool Host::advance_focus_(bool forward)
{
    // The one true choke point for every Tab-driven focus change — reached
    // both from dispatch_key_down's own Tab branch below and from a native
    // text control forwarding an unconsumed Tab via the public
    // advance_focus() wrapper (which never goes through dispatch_key_down).
    Widget* root = input_root_();
    if (!root)
    {
        return false;
    }
    // Narrowing order: an explicit focus scope wins — its owner (RoomView,
    // MainAppWidget) already picks whichever overlay is on top, including a
    // non-modal one stacked over a modal. With no scope set, the topmost
    // open modal dialog (access_modal()) keeps Tab inside it — the fallback
    // for surfaces whose owners set no scope (Settings, login, pop-outs). A
    // registered popup that scopes focus beats both, since it paints above
    // everything.
    if (Widget* scope = focus_scope_.lock().get(); scope && scope->visible_in_tree())
    {
        root = scope;
    }
    else if (Widget* modal = find_topmost_modal(root))
    {
        root = modal;
    }
    if (auto p = popup_.lock(); p && p->popup_scopes_focus() && p->visible_in_tree())
    {
        root = p.get();
    }
    Widget* next = next_focusable(root, focused_widget_.lock().get(), forward);
    if (!next)
    {
        return false;
    }
    request_focus(next);
    // Set after request_focus(), which clears focus_visible_ whenever focus
    // actually lands on a new widget — Tab/Shift-Tab traversal is the one
    // case among request_focus()'s callers that should show the ring.
    focus_visible_ = true;
    if (std::string tip = next->focus_tooltip_text(); !tip.empty())
    {
        show_tooltip(next, std::move(tip), next->bounds());
        tooltip_anchor_follows_focus_ = true;
    }
    else
    {
        cancel_tooltip_();
    }
    return true;
}

bool Host::dispatch_key_down(const KeyEvent& event)
{
    fire_user_activity_();
    if (key_prefilter_ && key_prefilter_(event))
    {
        request_repaint();
        return true;
    }
    wheel_latch_.reset(); // keyboard input starts a fresh gesture
    // Any key other than the Tab that may re-show one (advance_focus_)
    // dismisses a tooltip — e.g. Enter activating a Tab-focused icon
    // button shouldn't leave its focus tooltip floating over what opens.
    if (event.key != Key::Tab && event.key != Key::Backtab)
    {
        cancel_tooltip_();
    }
    // A drag in progress is the most "modal" transient state a surface can
    // be in — Escape cancels it ahead of every other Escape-consumer below
    // (popup dismiss, etc.), which would be reaching for state that no
    // longer means anything mid-drag.
    if (active_drag_ && event.key == Key::Escape)
    {
        cancel_drag();
        return true;
    }
    // Any key (not just Tab) reasserts keyboard modality — e.g. pressing
    // Enter/Space to activate an already-Tab-focused widget should keep the
    // ring visible. Skipped when the currently focused widget holds its own
    // native OS control (TextField/TextArea): a key event reaching this far
    // while one of those has focus is not a deliberate Tab-driven keyboard
    // interaction — it's either unreachable in practice (a real native
    // control fully consumes its own keys, e.g. Qt never even routes them
    // to this Host) or incidental bubble-propagation noise after the
    // control's own native submit signal (GTK's GtkEntry "activate") has
    // already handled it. Without this guard, pressing Enter to submit a
    // focused text field flips on a focus ring around it even though the
    // user never Tab-navigated anywhere.
    if (auto f = focused_widget_.lock(); !f || !f->holds_native_focus())
    {
        focus_visible_ = true;
    }
    if (auto p = popup_.lock(); p && p->dispatch_key_down(event))
    {
        request_repaint();
        return true;
    }
    bool context_menu_key = false;
    for (const KeyChord& chord : context_menu_chords())
    {
        context_menu_key = context_menu_key || chord_matches(chord, event);
    }
    if (context_menu_key)
    {
        if (auto f = focused_widget_.lock())
        {
            for (Widget* w = f.get(); w; w = w->parent())
            {
                if (w->on_context_menu_key())
                {
                    request_repaint();
                    return true;
                }
            }
            // No custom handler — deliver the right-click a mouse user would
            // make on the focused element, along the focus chain only (see
            // Widget::on_context_menu_key's doc comment).
            f->reveal_context_anchor();
            const Rect a = f->context_anchor_rect();
            const Point c{a.x + a.w * 0.5f, a.y + a.h * 0.5f};
            for (Widget* w = f.get(); w; w = w->parent())
            {
                const Rect b = w->bounds();
                if (w->on_right_click({c.x - b.x, c.y - b.y}))
                {
                    request_repaint();
                    return true;
                }
            }
        }
    }
    else if (event.key == Key::Tab || event.key == Key::Backtab)
    {
        // Checked independent of whether a widget is currently focused —
        // next_focusable(..., nullptr, ...) picks the first/last candidate
        // when nothing is, so Tab works from a cold start too, not just to
        // move between two already-focused widgets.
        if (advance_focus_(event.key == Key::Tab))
        {
            request_repaint();
            return true;
        }
        // No focusable widget at all — fall through to the root broadcast
        // below rather than swallowing the key.
    }
    else if (auto f = focused_widget_.lock())
    {
        if (f->dispatch_key_down(event))
        {
            request_repaint();
            return true;
        }
        // PageUp/PageDown/Home/End the focused widget didn't use scroll
        // the nearest scrollable region around it — e.g. a Settings page
        // while one of its buttons has focus.
        for (Widget* w = f.get(); w; w = w->parent())
        {
            if (auto* region = dynamic_cast<ScrollableBase*>(w);
                region && region->scroll_by_key(event))
            {
                request_repaint();
                return true;
            }
        }
    }
    Widget* root = input_root_();
    if (root && root->dispatch_key_down(event))
    {
        request_repaint();
        return true;
    }
    return false;
}

void Host::paint_focus_overlay(PaintCtx& ctx)
{
    auto w = focused_widget_.lock();
    if (!w)
    {
        return;
    }
    // The focused widget (or an ancestor — e.g. its owning panel/overlay
    // was dismissed) is no longer visible/enabled. Clear focus outright
    // rather than just skipping the ring: leaving focused_widget_ pointing
    // at a hidden widget would let a stray Enter/Tab still reach it via
    // Host::dispatch_key_down.
    if (!w->enabled() || !w->visible_in_tree())
    {
        clear_focus();
        return;
    }
    if (focus_visible_)
    {
        w->paint_own_focus_ring(ctx);
    }
}

void Host::queue_for_deletion(std::unique_ptr<Widget> subtree)
{
    pending_deletions_.push_back(std::move(subtree));
    if (drain_scheduled_)
    {
        return;
    }
    drain_scheduled_ = true;
    // Guard against the Host itself being destroyed before this posted
    // closure runs.
    post_to_ui(guarded(
        [this]
        {
            drain_deferred_deletions_();
        }));
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

void Host::show_tooltip(const void* owner, std::string text, Rect anchor_world, bool from_popup)
{
    tooltip_anchor_follows_focus_ = false;
    // A real popup is open — tooltips are suppressed entirely, unless this
    // request comes from the popup's own content (see from_popup's doc
    // comment in host.h).
    if (!popup_.expired() && !from_popup)
    {
        return;
    }
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
        const bool changed = text != tooltip_text_ || anchor_world.x != tooltip_anchor_.x ||
                             anchor_world.y != tooltip_anchor_.y ||
                             anchor_world.w != tooltip_anchor_.w ||
                             anchor_world.h != tooltip_anchor_.h;
        tooltip_text_ = std::move(text);
        tooltip_anchor_ = anchor_world;
        if (tooltip_visible_ && changed)
        {
            request_repaint();
        }
        return;
    }
    // New owner — reset and re-arm the dwell delay.
    tooltip_owner_ = owner;
    tooltip_text_ = std::move(text);
    tooltip_anchor_ = anchor_world;
    tooltip_visible_ = false;
    const auto gen = ++tooltip_gen_;
    post_delayed(kTooltipShowDelayMs, guarded(
                                          [this, gen, owner]
                                          {
                                              if (gen != tooltip_gen_ || owner != tooltip_owner_)
                                              {
                                                  return; // superseded/cancelled
                                              }
                                              if (tooltip_anchor_follows_focus_)
                                              {
                                                  if (auto f = focused_widget_.lock();
                                                      f && f.get() == owner)
                                                  {
                                                      tooltip_anchor_ = f->bounds();
                                                  }
                                              }
                                              tooltip_visible_ = true;
                                              tooltip_reveal_pending_ = true;
                                              request_repaint();
                                          }));
}

void Host::hide_tooltip(const void* owner)
{
    if (owner != tooltip_owner_)
    {
        return;
    }
    cancel_tooltip_();
}

void Host::update_tooltip_text(const void* owner, std::string text)
{
    if (!popup_.expired())
    {
        return;
    }
    if (owner != tooltip_owner_)
    {
        if (tooltip_owner_ != nullptr)
        {
            return; // someone else owns it — not ours to steal
        }
        tooltip_owner_ = owner; // adopt: caller asserts it's genuinely hovered right now
    }
    tooltip_text_ = std::move(text);
    if (!tooltip_visible_)
    {
        tooltip_reveal_pending_ = true;
    }
    tooltip_visible_ = true;
    request_repaint();
}

void Host::cancel_tooltip_()
{
    if (tooltip_owner_ == nullptr && !tooltip_visible_ && tooltip_text_.empty())
    {
        return; // nothing active/pending — avoid a spurious repaint request
    }
    ++tooltip_gen_; // invalidates any in-flight show timer
    tooltip_owner_ = nullptr;
    tooltip_visible_ = false;
    tooltip_reveal_pending_ = false;
    tooltip_text_.clear();
    request_repaint();
}

void Host::paint_tooltip_overlay(PaintCtx& ctx, Rect surface_bounds)
{
    if (!tooltip_visible_ || tooltip_text_.empty())
    {
        return;
    }
    if (!tooltip_widget_)
    {
        tooltip_widget_ = std::make_unique<Tooltip>();
    }
    if (tooltip_reveal_pending_)
    {
        tooltip_widget_->reset_reveal();
        tooltip_reveal_pending_ = false;
    }
    tooltip_widget_->set_content(tooltip_text_, tooltip_anchor_);
    tooltip_widget_->paint_overlay(ctx, surface_bounds);
    if (tooltip_widget_->still_revealing())
    {
        request_repaint();
    }
}

void Host::show_toast(std::string message)
{
    announce(message);
    toast_message_ = std::move(message);
    toast_visible_ = true;
    const auto gen = ++toast_gen_;
    request_repaint();
    post_delayed(kToastDurationMs, guarded(
                                       [this, gen]
                                       {
                                           if (gen != toast_gen_)
                                           {
                                               return; // superseded by a newer show_toast()
                                           }
                                           toast_visible_ = false;
                                           request_repaint();
                                       }));
}

void Host::paint_toast_overlay(PaintCtx& ctx, Rect surface_bounds)
{
    if (!toast_visible_ || toast_message_.empty())
    {
        return;
    }
    if (!toast_widget_)
    {
        toast_widget_ = std::make_unique<Toast>();
    }
    toast_widget_->set_message(toast_message_);
    toast_widget_->paint_overlay(ctx, surface_bounds);
    if (toast_widget_->still_revealing())
    {
        request_repaint();
    }
}

} // namespace tk
