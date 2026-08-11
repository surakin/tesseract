#pragma once

// tk::TextField — a canvas-tree widget that owns and positions a native
// single-line text field overlay, and participates fully in the tk-level
// keyboard-focus system (Tab/Shift-Tab traversal, click-to-focus) alongside
// ordinary canvas widgets. The fact that text editing happens in a native
// OS control is an implementation detail: callers construct one exactly
// like any other widget (via a Host&) and interact with it through the
// pass-through API below, never touching a raw NativeTextField themselves.
//
// Reserves layout space like a plain Label (draws nothing itself — the
// native control paints on top) and positions the native control directly
// from its own arrange(), replacing the old pattern of a separate
// placeholder Label whose bounds() had to be read and reapplied by an
// external "position_overlay()"-style pass.

#include "controls.h"
#include "host.h"

#include <optional>
#include <vector>

namespace tk
{

class TextField : public Label
{
protected:
    // `min_height` is the minimum vertical space to reserve (and the
    // minimum height handed to the native control) — mirrors the old
    // placeholder's set_min_size({0, height}) convention. Host comes from
    // host() (inherited, valid from the first line of this constructor's
    // body — see widget.h), not a parameter. Does NOT create the native
    // control — see ensure_native_() below.
    explicit TextField(float min_height);
    TK_WIDGET_FACTORY_FRIEND(TextField)

public:
    void set_text(std::string text);
    std::string text() const;
    void set_placeholder(std::string text);
    void set_password(bool password);
    // Reduce internal padding so the field fits inside a compact inline row
    // (e.g. an image-pack shortcode/rename field).
    void set_compact(bool compact);
    void set_text_color(Color c);
    void set_on_changed(std::function<void(const std::string&)> cb);
    void set_on_submit(std::function<void()> cb);

    // Notifies whenever this field's native OS focus changes, after the
    // internal native-sync guard logic below has already run — observes
    // the settled state, not intermediate re-entrant calls. For callers
    // that need blur-commit/blur-cancel style behavior (e.g. committing
    // an inline edit when the field loses focus).
    void set_on_focus_changed(std::function<void(bool)> cb);

    // Stackable Up/Down/Escape/Left/Right handler chain: any number of
    // independent installers can layer popup-list navigation onto this
    // field without clobbering each other. Tried most-recently-pushed
    // first, after Tab/Shift-Tab (which this widget always claims for
    // canvas-focus traversal and never exposes here). A handler returns
    // true to consume the key; the first one to return true wins.
    void push_popup_nav(std::function<bool(NavKey)> cb);
    void pop_popup_nav();

    // Win32 insets the native EDIT 1 px inside the shared rect for a snug
    // visual fit; unused (0) on every other backend.
    void set_overlay_inset(float inset)
    {
        overlay_inset_ = inset;
    }

    void set_enabled(bool enabled) override;

    // Shadows Widget::set_visible (not virtual) so this widget's own
    // visible_ flag and the native control's OS-level visibility always
    // move together — every existing call site already calls set_visible()
    // through a TextField*-typed pointer, so this resolves correctly.
    //
    // Skips the native forward entirely when it would be a same-value call
    // — redundant native show/hide calls are wasted OS-level work at best,
    // and at worst have real side effects a plain canvas Widget::set_visible()
    // doesn't: Qt's QWidget::setVisible(false) on an already-focused widget
    // silently clears that focus (fires focusOutEvent). Callers that hide
    // and reshow this field within the same frame (e.g. a naive "hide
    // everything, reshow only the active one" per-paint idiom applied
    // unconditionally to every field regardless of whether it's staying
    // active) must compute which field should actually change state and
    // only call set_visible() for those — see EncryptionSetupOverlay::paint()
    // for the pattern. This guard only elides genuinely-redundant repeats;
    // it does not (and must not) suppress an intentional hide of a
    // currently-focused field, e.g. a view hiding itself and its children on
    // close — ImagePackEditorView::set_visible(false) relies on exactly that
    // to hide its focused paste_catcher_ when the view closes.
    void set_visible(bool v);

    // Programmatic focus, routed through Host::request_focus()/clear_focus()
    // (not the native field directly) so a caller-driven focus change stays
    // in sync with tk-level focus tracking exactly like a Tab-driven one —
    // both end up calling on_focus_gained()/on_focus_lost() below.
    void set_focused(bool focused);

    void arrange(LayoutCtx& ctx, Rect bounds) override;
    void paint(PaintCtx& ctx) override;

    // Canvas-drawn-text backends (see NativeTextField::rendered_image())
    // no longer receive real OS clicks — the invisible native control has
    // no on-screen presence for the OS to route to — so canvas hit-testing
    // reaches here and forwards on. On backends still using a real overlay,
    // the native control already ate the click at the OS level before
    // canvas hit-testing ever ran (see NativeTextField::set_on_pointer_down's
    // doc comment), so these are unreachable there — safe to claim
    // unconditionally.
    bool on_pointer_down(Point local) override;
    void on_pointer_drag(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;

    // Canvas-level hover notification for backends whose real control is
    // excluded from the OS's own hover/cursor handling — see
    // NativeTextField::set_hovering()'s doc comment in host.h.
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    // Keyboard-focusable whenever enabled. Tab landing here (via
    // Host::advance_focus) moves real native OS focus to the wrapped
    // field; the field's own focus-changed notification keeps tk-level
    // focus in sync when the user clicks directly into it instead.
    bool focusable() const override
    {
        return enabled_ && field_ != nullptr;
    }
    bool holds_native_focus() const override
    {
        return field_ != nullptr;
    }
    // Only pushes set_focused(true)/(false) down to the native field when
    // the change originated from the canvas side (Tab moving focus to or
    // away from this widget) — NOT when it's a reaction to the native
    // field's own set_on_focus_changed() notification
    // (syncing_from_native_ is set for the duration of that callback in
    // the .cpp). Redundantly telling a field "you gained/lost focus" right
    // after it told us the same thing is not just a no-op: on macOS in
    // particular, calling makeFirstResponder: on a field editor that is
    // ALREADY first responder (the on_focus_gained case, reached
    // synchronously from inside AppKit's own -controlTextDidBeginEditing:
    // delivery) tears down and reattaches the field editor — observed as
    // the field selecting all of its pre-keystroke text while a keystroke
    // is in flight, i.e. exactly "selects everything but the character I
    // just typed."
    void on_focus_gained() override
    {
        if (field_ && !syncing_from_native_) field_->set_focused(true);
        if (on_focus_changed_cb_) on_focus_changed_cb_(true);
    }
    // Only pushes set_focused(false) down to the native field when this
    // loss originated from the canvas side (e.g. Tab moving focus to
    // another widget) — NOT when it's a reaction to the native field's own
    // set_on_focus_changed(false) notification (syncing_from_native_ is set
    // for the duration of that callback in the .cpp). Redundantly telling a
    // field "you lost focus" right after it told us the same thing is not
    // just a no-op: on macOS in particular it forces makeFirstResponder to
    // the surface, which can cut off an edit session the native side was
    // about to resume on its own (observed as an intermittent selection
    // glitch while typing at the end of the field's text).
    void on_focus_lost() override
    {
        if (field_ && !syncing_from_native_) field_->set_focused(false);
        if (on_focus_changed_cb_) on_focus_changed_cb_(false);
    }

private:
    // Creates field_ via host()->make_text_field() and wires its internal
    // callbacks (focus-sync, popup-nav forwarding, repaint forwarding), then
    // flushes pending_ into it. Called from two places, not the constructor:
    // set_visible(true) (so a caller that shows-then-immediately-focuses a
    // field within one function call, before the next layout pass, still
    // works) and arrange() (the reliable general-case signal — covers a
    // field that starts, and stays, visible via Widget's own default and so
    // never receives an explicit set_visible(true) transition to hook).
    // Deferring this is the entire point: on Win32 (and equivalent backends
    // elsewhere), make_text_field() creates a real native child window, and
    // a field that's part of a hidden subtree (an unopened settings tab, an
    // unselected tab within it, ...) may never need one — parents that only
    // conditionally arrange such a field already skip calling arrange() on
    // it while hidden (see ImagePackEditorView::arrange()'s fields_enabled
    // gating for the concrete precedent), so this stays lazy for that case.
    // No-op if field_ already exists. Reentrancy-guarded: on at least one
    // backend (Qt6), host()->make_text_field() synchronously pumps pending
    // widget geometry events as a side effect of constructing the native
    // control (QWidget::render() -> QWidgetPrivate::prepareToRender() ->
    // sendPendingMoveAndResizeEvents()), which can reach back into
    // Host::relayout() and re-arrange this same still-under-construction
    // field before field_ has been assigned — without the guard, that
    // recurses into ensure_native_() again indefinitely. creating_native_
    // makes the reentrant call a no-op instead; the field is simply not
    // positioned on that redundant reentrant pass, and gets positioned
    // correctly once the outer call finishes and a real arrange() runs.
    void ensure_native_();
    bool creating_native_ = false;

    std::unique_ptr<NativeTextField> field_;
    float min_height_;
    float overlay_inset_ = 0.0f;
    bool syncing_from_native_ = false;
    std::function<void(bool)> on_focus_changed_cb_;
    std::vector<std::function<bool(NavKey)>> nav_handlers_;
    // Last Widget::background_color() pushed to field_->set_background_color()
    // — avoids re-forwarding the same value (and the recapture it can
    // trigger) on every paint(). See TextField::paint()'s doc comment.
    std::optional<Color> last_bg_pushed_;

    // State set through the setters below before field_ exists, replayed by
    // ensure_native_() once it does. Every setter is a straight pass-through
    // to field_ once it exists — see the .cpp — so this only ever holds
    // state from before the first set_visible(true).
    struct PendingState
    {
        std::optional<std::string> text;
        std::optional<std::string> placeholder;
        std::optional<bool> password;
        std::optional<bool> compact;
        std::optional<Color> text_color;
        std::optional<bool> enabled;
        std::function<void(const std::string&)> on_changed;
        std::function<void()> on_submit;
    };
    PendingState pending_;
};

} // namespace tk
