#pragma once

// Widget tree. Pure C++, no platform dependencies. Each widget knows
// how to (a) measure itself given a width × height constraint, (b)
// paint itself into a Canvas, and (c) hit-test pointer events. Layout
// is the classic two-pass measure-then-arrange model: measure() returns
// the desired size, arrange() commits the final bounds.

#include "canvas.h"
#include "theme.h"
#include "weak_self.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace tk
{

// Inset/outset on the four edges, in logical pixels. The "trbl" naming
// matches the CSS order so reading mixed sources stays unambiguous.
struct Edges
{
    float top = 0;
    float right = 0;
    float bottom = 0;
    float left = 0;

    static constexpr Edges all(float v)
    {
        return {v, v, v, v};
    }
    static constexpr Edges symmetric(float h, float v)
    {
        return {v, h, v, h};
    }
    constexpr float horizontal() const
    {
        return left + right;
    }
    constexpr float vertical() const
    {
        return top + bottom;
    }
};

// Flex-style axis alignment. Main = along the layout axis (vertical for
// VBox, horizontal for HBox); Cross = perpendicular to it.
enum class Main
{
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround
};
enum class Cross
{
    Start,
    Center,
    End,
    Stretch
};

// Per-widget tug on its parent's measure: when fill_main == true the
// widget asks for the leftover main-axis space; fill_cross == true asks
// to span the cross axis. Used by layout containers; ignored otherwise.
struct LayoutHints
{
    bool fill_main = false;
    bool fill_cross = false;
};

// Layout / paint contexts. Widgets never reach for global state; the
// owning tree threads these in from the platform host.
struct LayoutCtx
{
    CanvasFactory& factory;
    const Theme& theme;
};
// Optional per-paint sink for animated-image damage tracking. Views call
// note_image() with the on-screen (surface-coordinate) rect of every animated
// image they draw; the host collects these so the animation timer can repaint
// just those regions instead of the whole surface. Null when the backing host
// does not (yet) support partial animation repaints.
struct AnimDamageSink
{
    virtual ~AnimDamageSink() = default;
    virtual void note_image(const std::string& key, Rect world) = 0;
};

// Forward declarations — avoids a circular include (host.h includes widget.h).
class Host;
class Widget;
class RootWidget;

// The widget-construction factory. Every Widget subclass is constructed
// exclusively through create_widget()/create_root_widget() (defined below,
// after RootWidget), which push the real Host* onto a thread-local "pending
// host" stack immediately before invoking the real constructor and pop it
// after — Widget's own constructor (widget.cpp) reads the top of that stack
// via ordinary member-initialization, so host() is valid from the first
// line of any derived constructor's body, with no parameter threading
// required.
//
// An earlier version of this mechanism instead pre-poked Widget::host_
// directly into the not-yet-constructed object's raw memory before calling
// the real constructor. That is undefined behavior per [basic.life] (writing
// to a subobject of an object whose lifetime hasn't started), and GCC -O3
// proved it in practice: interprocedural scalar-replacement determined the
// store was unobservable, deleted it and the parameter carrying it, leaving
// every widget's host() reading uninitialized memory in release builds. The
// thread-local design has no equivalent hazard — nothing here is a dead
// store from the optimizer's point of view.
namespace detail
{
template <typename T, typename... Args>
std::unique_ptr<T> create_impl(Host* host, Args&&... args);

// Thread-local stack of "the Host currently being constructed under" —
// pushed by create_widget()/create_root_widget() immediately before
// invoking a widget's real constructor, popped after (even on exception).
// Defined in widget.cpp; the thread_local lives inside that one function
// definition, so it's a single instance process-wide regardless of how many
// translation units call this.
std::vector<Host*>& pending_host_stack();
} // namespace detail

template <typename T, typename... Args>
std::unique_ptr<T> create_widget(Widget* parent, Args&&... args);

template <typename T, typename... Args>
std::unique_ptr<T> create_root_widget(Host* host, Args&&... args);

// Every Widget subclass's constructor must be protected (or private) and
// invoke this macro once, so create_widget()/create_root_widget() are the
// only way to construct it. This isn't load-bearing for host() correctness
// anymore (an unmigrated class with a public constructor now just gets
// host() from whatever ambient pending_host_stack() entry is on top —
// nullptr if none, the real Host* if nested inside a create_*() call —
// never garbage), but it keeps every widget's construction path uniform and
// catches accidental direct construction at compile time. The actual
// `new (mem) T(...)` construction happens inside detail::create_impl() (see
// below, after RootWidget) — create_widget()/create_root_widget() just
// forward to it, so that is what needs the friendship. Friendship is
// per-class in standard C++ (befriending a base doesn't extend to derived
// classes), hence the macro rather than a single blanket friend declaration.
#define TK_WIDGET_FACTORY_FRIEND(ClassName)                                                     \
    template <typename T, typename... Args>                                                     \
    friend std::unique_ptr<T> tk::detail::create_impl(tk::Host*, Args&&...);

struct PaintCtx
{
    Canvas& canvas;
    CanvasFactory& factory;
    const Theme& theme;
    AnimDamageSink* anim_damage = nullptr;
    Host*           host        = nullptr;
};

// Paints a localized "this is a valid drop target" highlight (translucent
// accent fill + accent-colored border, inset slightly within `rect`) — the
// per-widget replacement for the old whole-surface "Drop to attach" overlay.
// Call from paint() while the widget is claiming on_drag_hover.
void paint_drag_hover_highlight(PaintCtx& ctx, Rect rect);

// Paints a keyboard-focus ring (accent-colored stroke, no fill) around
// `rect` at the given corner radius. Driven by Host::paint_focus_overlay()
// via Widget::paint_own_focus_ring() for whichever widget currently holds
// tk-level keyboard focus — see Widget::has_focus().
void paint_focus_ring(PaintCtx& ctx, Rect rect, float radius = 4.0f);

enum class Key
{
    Unknown,
    Escape,
    Enter,
    Space,
    Tab,
    Backtab,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Backspace,
    Delete,
    Character
};

struct KeyEvent
{
    Key key = Key::Unknown;
    // Populated when key == Character. Stored as UTF-8 so platform hosts do
    // not need to expose native string types to shared widgets.
    std::string text;
    bool ctrl   = false;
    bool shift  = false;
    bool alt    = false;
    bool meta   = false;
    bool repeat = false;
};

// Payload for a dropped file, threaded through on_file_drop/dispatch_file_drop.
// A widget that rejects a drop must leave every field untouched; a widget
// that accepts one moves out of them as its last action before returning
// true from on_file_drop.
struct FileDropPayload
{
    std::vector<std::uint8_t> bytes;
    std::string                mime;
    std::string                filename;
};

// Semantic role exposed to assistive technology. Grows incrementally as
// widgets are mapped (see the accessibility plan) — Role::None is the
// default and means "excluded from the accessibility tree", so adding this
// enum requires no immediate opt-in from any existing Widget subclass.
enum class Role
{
    None,
    Button,
    CheckBox,
    Switch,
    RadioButton,
    ComboBox,
    TextInput,
    StaticText,
    Image,
    Link,
    List,
    ListItem,
    Grid,
    GridCell,
    Tab,
    TabList,
    TabPanel,
    Dialog,
    MenuItem,
    Group,
};

// Dynamic accessible state, read fresh each time the tree is walked (see
// access_tree.h) — deliberately separate from Role, which is static per
// widget instance. enabled/focused already exist on Widget itself
// (enabled_/has_focus_); the rest defaults to false and is opted into per
// widget (e.g. CheckButton overriding access_state() to report checked).
struct AccessState
{
    bool checked  = false;
    bool expanded = false;
    bool selected = false;
    bool busy     = false;
};

class Widget : public EnableWeakSelf<Widget>
{
public:
    // Reads the top of detail::pending_host_stack() (widget.cpp) — nullptr
    // if empty (e.g. a widget whose own constructor was never migrated onto
    // create_widget()/create_root_widget(), invoked with no ambient host in
    // scope). See the comment on pending_host_stack() above for why this
    // replaced an earlier, UB-laden pre-poke design.
    Widget();

    // The Host that owns this widget's tree. Valid from the very first line
    // of any derived constructor's body, since this base subobject always
    // finishes constructing before the derived class body runs.
    Host* host() const
    {
        return host_;
    }

    virtual ~Widget()
    {
        // Invalidate first so any outstanding weak_ptr taken via track()
        // reports expired() for the remainder of destruction, before
        // base/member teardown runs.
        invalidate_weak_self();
    }

    // Compute the desired size given the maximum bounds available. The
    // returned size is the natural size at or below `constraints`.
    virtual Size measure(LayoutCtx&, Size constraints) = 0;

    // Commit the final bounds. Default sets bounds_ and arranges all
    // children at the same bounds (overridden by layout containers).
    virtual void arrange(LayoutCtx&, Rect bounds);

    // Paint into the canvas at this widget's bounds. The default is
    // paint_before_children() + paint_children() + paint_after_children() —
    // see those three below. Override paint() itself only when a widget
    // genuinely needs to interleave custom drawing *between* specific
    // children (true z-order splicing); everything else should override
    // paint_before_children()/paint_after_children() instead, so children
    // are never at risk of silently going unpainted.
    virtual void paint(PaintCtx&);

    // Called once, immediately before paint_children() — for chrome that
    // must sit strictly *behind* every child (backgrounds, card fills).
    // Default: no-op.
    virtual void paint_before_children(PaintCtx&)
    {
    }

    // Called once, immediately after paint_children() — for chrome that
    // must sit strictly *on top of* every child (badges, "paint this
    // widget's own decoration last" needs). Default: no-op.
    virtual void paint_after_children(PaintCtx&)
    {
    }

    // Second paint pass, called by the host after the entire widget tree's
    // paint() has finished. Default propagates to children so every
    // container forwards the pass automatically. Override (without calling
    // the base) to draw popup content that must render above all other widgets.
    virtual void paint_overlay(PaintCtx& ctx)
    {
        for (auto& ch : children())
            if (ch->visible()) ch->paint_overlay(ctx);
    }

    // Called on this widget when the active tk::Theme changes. Override to
    // push fresh colors onto any native (non-canvas) overlay control this
    // widget positions via a *_rect() getter — a tk::NativeTextField/
    // NativeTextArea the host has mounted over this widget's bounds. Unlike
    // this widget's own paint(), which is handed a fresh theme every frame
    // via PaintCtx, a native control caches its own render state and goes
    // stale until explicitly told otherwise. Default: no-op.
    virtual void on_theme_changed(const Theme&) {}

    // Push `theme` through this widget's on_theme_changed(), then recurse
    // into every child unconditionally — including invisible ones, since a
    // hidden native field still needs correct colors queued for when it
    // next shows (unlike paint_overlay(), which only walks visible children
    // because there's nothing to paint for a hidden widget). Not virtual:
    // no override should need custom recursion order, and keeping it
    // non-virtual removes "override forgot to recurse into children" as a
    // failure mode entirely.
    void apply_theme(const Theme& theme)
    {
        on_theme_changed(theme);
        for (auto& ch : children())
            ch->apply_theme(theme);
    }

    // Called on this widget when the canvas's device-pixel scale factor
    // changes (window dragged to a monitor with a different DPI, or an
    // OS-level display-scaling setting changed) — independent of
    // on_theme_changed() above and unrelated to ordinary canvas repainting,
    // since Canvas::scale_factor() is already read live on every paint()
    // for anything drawn directly on the canvas. This exists only for a
    // native (non-canvas) overlay control a widget positions via a
    // *_rect() getter — a tk::NativeTextField/NativeTextArea — whose
    // rendered_image() capture happens outside the paint cycle (on
    // content/focus/selection/set_rect() signals) and so goes stale until
    // explicitly told otherwise. Default: no-op.
    virtual void on_scale_changed(float scale_factor) {}

    // Push `scale_factor` through this widget's on_scale_changed(), then
    // recurse into every child unconditionally — including invisible ones,
    // same rationale as apply_theme() above. Not virtual, for the same
    // reason apply_theme() isn't.
    void apply_scale_change(float scale_factor)
    {
        on_scale_changed(scale_factor);
        for (auto& ch : children())
            ch->apply_scale_change(scale_factor);
    }

    // Called by the host when a click lands outside an active popup widget.
    // ComboBox overrides this to collapse the dropdown.
    virtual void on_popup_dismiss() {}

    // Hit-test against this subtree. The input `Point` is in world (root-
    // surface) coordinates — the same space `bounds_` is stored in.
    // Returns the deepest visible widget whose bounds contain the point,
    // or this widget if none of the children claim it, or null if the
    // point is outside this widget's own bounds.
    virtual Widget* hit_test(Point world);

    // Pointer-wheel input. Positive dy = scroll content forward (down).
    // Return true to consume; otherwise the dispatcher walks up the
    // parent chain so containers can handle wheel events that bubble
    // out of inert children (typical for ScrollView / ListView).
    // `is_touchpad` is true for a continuous trackpad delta, false for a
    // discrete physical mouse-wheel notch — only the former ever arms
    // momentum/kinetic scrolling (see ScrollableBase::on_wheel_scroll).
    virtual bool on_wheel(Point /*local*/, float /*dx*/, float /*dy*/,
                          bool /*is_touchpad*/ = false)
    {
        return false;
    }

    // Pointer-down. Return true to claim ownership of the subsequent
    // pointer_up. The host remembers the claimer so a release outside
    // the widget still routes back to it (with inside_self=false).
    virtual bool on_pointer_down(Point /*local*/)
    {
        return false;
    }

    // Pointer-up on the widget that consumed the matching pointer_down.
    // `inside_self` tells the widget whether the release landed inside
    // its own bounds — Button uses this to fire on_click only when the
    // user released on the same control they pressed.
    virtual void on_pointer_up(Point /*local*/, bool /*inside_self*/)
    {
    }

    // Pointer-drag: the host forwards every pointer-move after a
    // pointer-down that this widget claimed, until the matching
    // pointer-up arrives. `local` is in widget-local coordinates and
    // can land outside the widget's bounds when the user drags off the
    // control. Used by ListView for scrollbar-thumb dragging.
    virtual void on_pointer_drag(Point /*local*/)
    {
    }

    // Pointer-move without a press. Called via `dispatch_pointer_move`
    // on the deepest hit widget so views can update per-element hover
    // state (reaction chips, row-hover affordances, etc.). Buttons
    // continue to handle their own hover via the host's Button-hover
    // bookkeeping — this is the generic widget-level hook.
    // Returns true if visual state changed and a repaint is needed.
    virtual bool on_pointer_move(Point /*local*/)
    {
        return false;
    }
    // Mirrors `on_pointer_leave` semantics from the host so widgets can
    // clear hover state when the pointer leaves the surface (or the
    // pointer-move dispatch lands on a different widget).
    virtual void on_pointer_leave()
    {
    }

    // Secondary (right) mouse button — analogue of on_pointer_down for
    // right-click. Return true to consume the event; false to bubble.
    virtual bool on_right_click(Point /*local*/)
    {
        return false;
    }

    // Dropped-file input. `local` is in widget-local coordinates, mirroring
    // on_pointer_down. Reject (return false) WITHOUT moving out of
    // `payload`'s fields, so an unclaimed drop stays intact for the next
    // candidate tried by dispatch_file_drop. Accept by moving out of them
    // as the last action before returning true.
    virtual bool on_file_drop(Point /*local*/, FileDropPayload& /*payload*/)
    {
        return false;
    }

    // Drag-hover feedback while a drag is over this widget but hasn't been
    // dropped yet. `local` is widget-local coordinates. Return true to claim
    // the hover — this both selects this widget as the drag's current
    // target (for on_drag_leave purposes) and requests a repaint, so a
    // claiming widget can paint its own localized highlight instead of a
    // generic whole-surface indicator. Return false to let dispatch_drag_hover
    // try an ancestor instead (this is claim/reject like on_file_drop, NOT
    // unconditional-deepest-leaf like on_pointer_move — an ancestor such as
    // RoomView has no drop-target descendant of its own and must still be
    // reachable). Default: not interested.
    virtual bool on_drag_hover(Point /*local*/)
    {
        return false;
    }

    // Called on the widget that last claimed on_drag_hover when the drag
    // moves to a different claimant, leaves the surface, or the drag ends.
    // Mirrors on_pointer_leave.
    virtual void on_drag_leave()
    {
    }

    // Keyboard input. Platform surfaces translate native key events into this
    // shared shape, then dispatch from the root or focused widget. Return true
    // to consume. The default dispatcher gives visible children first refusal
    // in reverse paint order, then calls on_key_down() on this widget.
    virtual bool on_key_down(const KeyEvent&)
    {
        return false;
    }
    virtual bool dispatch_key_down(const KeyEvent&);

    // Focus hooks for keyboard-capable widgets. Surface-level focus ownership
    // will call these as platform key routing is migrated into tk.
    virtual bool focusable() const
    {
        return false;
    }

    // Whether an ordinary mouse click that this widget claims (i.e. it's
    // the Widget* returned by dispatch_pointer_down) should also move
    // tk-level keyboard focus onto it. Defaults to true, matching
    // focusable() itself for most widgets (text fields, buttons: clicking
    // them is exactly how a user focuses them).
    //
    // Override to false for a widget that must stay focusable() — so Tab
    // still reaches it, e.g. for keyboard row-navigation — but whose
    // ordinary mouse click already performs a complete, separate action
    // (e.g. RoomListView's row selection) and shouldn't additionally steal
    // keyboard focus away from wherever the user was actually typing (the
    // compose box). Host::dispatch_pointer_down consults this in addition
    // to focusable() before calling request_focus().
    virtual bool focus_on_click() const
    {
        return true;
    }

    virtual void on_focus_gained()
    {
    }
    virtual void on_focus_lost()
    {
    }

    // Accessibility hooks — see ui/shared/tk/access_tree.h for the tree
    // walker that consumes these. Role::None (the default) excludes this
    // widget from the accessibility tree entirely; its accessible
    // descendants (if any) attach directly to the nearest accessible
    // ancestor instead, so purely-structural layout widgets (VBox, Stack,
    // ...) don't need to opt in just to avoid breaking the tree.
    virtual Role access_role() const
    {
        return Role::None;
    }
    // Accessible name (screen-reader-announced label). Must be produced via
    // tk::tr()/trn()/trf() at the call site that sets whatever backs this,
    // per the project's i18n policy — this getter just returns the already-
    // localized string.
    virtual std::string access_name() const
    {
        return {};
    }
    // Default reflects the state Widget already tracks; override to add
    // checked/expanded/selected/busy where applicable (e.g. CheckButton).
    virtual AccessState access_state() const
    {
        return AccessState{};
    }

    // Invoke this widget's "default action" — what an assistive-technology
    // client fires when the user activates the node (AT-SPI's "click"/
    // UIA's "Invoke" pattern, Qt's QAccessibleActionInterface press
    // action). Distinct from on_key_down's Enter/Space handling: that path
    // requires this widget to hold tk-level keyboard focus first (see e.g.
    // Button::on_key_down's has_focus() guard), which an AT client
    // invoking a node out of band has no reason to have claimed. Default:
    // no action available (e.g. Label, ImageView). Returns true if an
    // action was actually performed, so a caller can distinguish "did
    // something" from "this node has no default action."
    virtual bool access_default_action()
    {
        return false;
    }

    // True for a widget that owns and manages a real native OS text-input
    // overlay (tk::TextField/TextArea) — i.e. on_focus_gained() already
    // asserts genuine native keyboard focus itself when this widget becomes
    // tk-focused. Each backend's Surface must consult this after
    // dispatching a click: grabbing native/OS focus onto the surface
    // itself unconditionally on every click (as every platform's
    // mousePressEvent-equivalent otherwise would, so Tab/keys still route
    // somewhere after a click on a plain canvas widget with no native
    // control of its own) would immediately undo that native focus this
    // widget just correctly claimed.
    virtual bool holds_native_focus() const
    {
        return false;
    }

    // Paints this widget's keyboard-focus ring. Called by
    // Host::paint_focus_overlay for whichever widget currently holds
    // tk-level keyboard focus, gated by focus_visible_ (keyboard-only —
    // see that flag's own doc comment). Default: the shared accent-colored
    // ring around bounds() at the standard radius (paint_focus_ring(),
    // declared above). Override to trace a different shape entirely — e.g.
    // ComposeBar's ComposerTextArea (ui/shared/views/ComposeBar.cpp) traces
    // the whole compose card's rounded-rect outline instead of its own
    // narrow text-column bounds, so the card reads as one focused unit.
    virtual void paint_own_focus_ring(PaintCtx& ctx)
    {
        paint_focus_ring(ctx, bounds());
    }

    // Walk into the deepest visible child under `world`, then bubble
    // back up to find a widget whose on_pointer_down returns true.
    // Returns the claiming widget (or nullptr if none did). `world` is
    // in root-surface coordinates (the space `bounds_` is stored in).
    // The widget-local Point handed to on_pointer_down is computed by
    // subtracting the claimer's own world origin.
    virtual Widget* dispatch_pointer_down(Point world);

    // Analogue of dispatch_pointer_down for right-click. Walks the
    // widget tree depth-first; calls on_right_click on the deepest
    // widget under `world` that returns true. Returns the consuming
    // widget or nullptr.
    virtual Widget* dispatch_right_click(Point world);

    // Walk into the deepest visible child under `world` and call
    // `on_pointer_move(local)` on it. Returns the deepest widget that
    // received the event (or nullptr if `world` was outside), and sets
    // *dirty = true if the widget reported a visual change. Hosts call
    // this from their pointer-move handler when no widget holds the press.
    virtual Widget* dispatch_pointer_move(Point world, bool* dirty = nullptr);

    // Walk into the hit widget, then bubble up through parents until
    // someone consumes the wheel event. `world` is in root-surface
    // coordinates. Called by hosts on the root.
    virtual bool dispatch_wheel(Point world, float dx, float dy, bool is_touchpad = false);

    // Analogue of dispatch_pointer_down for a dropped file. `world` is in
    // root-surface coordinates. Walks into the deepest visible child under
    // `world` first (topmost paint order); if none claims it, calls this
    // widget's own on_file_drop. Returns the accepting widget, or nullptr
    // if none did — `payload` is left untouched in that case.
    virtual Widget* dispatch_file_drop(Point world, FileDropPayload& payload);

    // Analogue of dispatch_file_drop for drag-hover feedback (no payload —
    // just "is anyone interested in showing feedback for a drag at this
    // point"). Same claim-based shape: children topmost-first, self last.
    // Returns the claiming widget or nullptr.
    virtual Widget* dispatch_drag_hover(Point world);

    // Translate a point from root-surface coordinates into this widget's
    // local coordinate system. Since `bounds_` is stored in world coords
    // throughout the tree, this is just `world - this->bounds_`.
    Point world_to_local(Point world) const;

    // World-coords containment check used by the dispatch + hit-test
    // routines above. Virtual so widgets with overflow content (e.g. a
    // compose bar whose image preview floats above its layout bounds) can
    // extend the hit region without changing their reported size.
    virtual bool contains_world(Point world) const;

    // Tree.
    Widget* parent() const
    {
        return parent_;
    }
    Rect bounds() const
    {
        return bounds_;
    }
    // Effective visibility: this widget's own flag AND every ancestor's.
    // Visibility cascades — a widget under a hidden parent is not visible,
    // even with its own flag set. `parent_visible_` caches the ancestor half
    // (kept current by set_visible()'s downward propagation and by
    // add_child()), so this stays O(1) despite being called from every
    // per-paint / per-pointer tree walk.
    bool visible() const
    {
        return visible_ && parent_visible_;
    }

    // The widget's own visibility flag, ignoring ancestors. Rarely needed —
    // set_visible()'s same-value guard and any code that must distinguish
    // "I was hidden" from "an ancestor hid me".
    bool own_visible() const
    {
        return visible_;
    }

    void set_visible(bool v)
    {
        if (v == visible_)
            return;
        visible_ = v;
        const bool eff = visible();
        for (auto& ch : children_)
            ch->apply_parent_visible_(eff);
        mark_relayout_needed_();
    }

    // Retained as a synonym for visible() now that visibility cascades — the
    // ~30 existing `set_visible(visible_in_tree() && cond)` call sites stay
    // correct (and can be simplified to visible() later).
    bool visible_in_tree() const
    {
        return visible();
    }

    // Declare the solid color this widget paints immediately behind its
    // own content, so descendants that need to know their effective
    // backdrop (see background_color() below) can find it without every
    // intermediate widget in between having to redeclare it. Callers pass
    // the exact color already used for their own fill — e.g. ComposeBar
    // declares its compose-card fill color once and every field nested
    // inside it inherits that value.
    void set_background_color(Color c)
    {
        background_color_ = c;
    }

    // This widget's own background_color() if set, otherwise the nearest
    // ancestor's — mirrors visible_in_tree()'s parent-chain walk.
    // std::nullopt means no widget from here up to the root ever declared
    // one. Primary consumer: NativeTextField/NativeTextArea's offscreen
    // capture, which needs an opaque, correctly-colored buffer to get
    // subpixel text antialiasing (see host.h's set_background_color() doc
    // comment) instead of guessing or reading back live pixels.
    std::optional<Color> background_color() const
    {
        for (const Widget* w = this; w; w = w->parent())
            if (w->background_color_.has_value())
                return w->background_color_;
        return std::nullopt;
    }
    // A disabled widget is opaque to input: every tree-walking input
    // dispatcher (dispatch_pointer_down / _move / _right_click / _file_drop /
    // _drag_hover, hit_test, dispatch_key_down) stops at it and does not
    // descend, so a disabled widget — leaf or container — swallows anything
    // that lands on it and its whole subtree is inert. It is also skipped by
    // Tab traversal (collect_focus_order) and refused by Host::request_focus.
    bool enabled() const
    {
        return enabled_;
    }
    // Virtual so widgets that need extra bookkeeping on disable (e.g.
    // ComboBox collapsing an open dropdown) can extend it; the base just
    // stores the flag. Hover/click are gated on enabled_ centrally in the
    // dispatch_* walkers (see widget.cpp) — overrides don't need to re-check.
    //
    // Requests a repaint of this widget's own screen area when the flag
    // actually flips: enabled/disabled is usually a paint-only change (dimmed
    // fill/text), and the caller that flips it may not be in the middle of a
    // pointer/click dispatch that would otherwise repaint for it — e.g. a
    // compose bar's send button toggled from a text-changed event, which on
    // some backends (Qt6) only issues a *scoped* repaint of the text field's
    // own rect (see NativeTextArea::set_on_repaint_needed), leaving the
    // button's stale pixels on screen until an unrelated full repaint
    // happens to touch them. Defined out-of-line in widget.cpp: Host is only
    // forward-declared here, and request_repaint_rect() needs its full type.
    virtual void set_enabled(bool enabled);

    // Whether this widget instance currently holds tk-level keyboard focus.
    // Host is the sole writer (via set_focused_(), below); widgets only
    // read it — typically from paint() to draw a focus ring, or from
    // on_key_down() to decide whether Enter/Space means "activate me".
    bool has_focus() const
    {
        return has_focus_;
    }
    void set_layout_hints(LayoutHints h)
    {
        hints_ = h;
        mark_relayout_needed_();
    }
    LayoutHints layout_hints() const
    {
        return hints_;
    }

    // Sibling paint/hit-test ordering, ascending — a widget with a higher
    // z_order() paints later (on top) and is hit-tested first (topmost).
    // Defaults to 0, meaning "ordinary insertion order" (children_ is kept
    // sorted by z_order with std::stable_sort, so equal-z_order siblings
    // never change relative order). Setting this on a specific child is the
    // preferred way to make it paint last/hit-test first — e.g. a modal
    // overlay child — instead of relying on add_child() call order.
    int z_order() const
    {
        return z_order_;
    }
    void set_z_order(int z)
    {
        if (z_order_ == z) return;
        z_order_ = z;
        if (parent_) parent_->resort_children_();
    }

    // Take ownership of a child. Returns a borrowed pointer for callers
    // that want to keep wiring (e.g. on_click handlers, dynamic state).
    template <typename W>
    W* add_child(std::unique_ptr<W> w)
    {
        W* raw = w.get();
        raw->parent_ = this;
        // Inherit this container's effective visibility — covers a subtree
        // built detached (all parent_visible_ == true) and then added under a
        // hidden parent.
        raw->apply_parent_visible_(visible());
        children_.push_back(std::move(w));
        resort_children_();
        mark_relayout_needed_();
        return raw;
    }

    // Removes a direct child. If this tree is rooted in a RootWidget (see
    // get_root_widget()), ownership passes to it (RootWidget::queue_for_deletion)
    // instead of being destroyed inline — otherwise the child is destroyed
    // synchronously, as it always was. Borrowed pointers to the removed
    // widget become dangling either way — callers must clear them. Asserts
    // that child->parent_ == this.
    void remove_child(Widget* child);

    // Drop every child widget. Any borrowed pointers returned from
    // add_child() are dangling after this call — callers must clear them.
    // Routes each child through the tree's RootWidget exactly like
    // remove_child(), one at a time.
    void clear_children();

    const std::vector<std::unique_ptr<Widget>>& children() const
    {
        return children_;
    }

    // Walks to the top of this widget's tree and returns it as a RootWidget
    // (every surface's actual top node is one — see Host::set_root()).
    // Returns nullptr for a detached/host-less tree (e.g. a bare Widget tree
    // built directly by a unit test) — remove_child()/clear_children() then
    // fall back to destroying immediately.
    RootWidget* get_root_widget();

protected:
    void paint_children(PaintCtx&);

    Rect bounds_{};
    LayoutHints hints_{};
    bool visible_ = true;
    // Cached "every ancestor is visible" — see visible(). Maintained by
    // set_visible()'s downward walk and by add_child(). A widget with no
    // parent (a detached subtree, a root) has no hidden ancestor, so true.
    bool parent_visible_ = true;
    bool enabled_ = true;

    // Fired on this widget when its *effective* visibility (visible())
    // actually flips because an ancestor was shown/hidden — NOT on a change
    // to its own flag (a container that hides itself is simply not painted;
    // its own set_visible() shadow, if any, handles its native control).
    // Default: no-op. tk::TextField/TextArea override it to show/hide the
    // native OS control they own: tk visibility is not painted for a hidden
    // subtree, but a native QLineEdit/GtkEntry has its own OS-level
    // visibility that nothing else would update.
    virtual void on_effective_visibility_changed_(bool /*now_visible*/) {}

    // Recompute parent_visible_ from `pv` (the direct parent's visible()) and,
    // if that flips this widget's effective visibility, notify it and recurse.
    // Stops early on any branch whose effective visibility is unchanged.
    void apply_parent_visible_(bool pv)
    {
        const bool was = visible();
        parent_visible_ = pv;
        const bool now = visible();
        if (was == now)
            return;
        on_effective_visibility_changed_(now);
        for (auto& ch : children_)
            ch->apply_parent_visible_(now);
    }

private:
    // Set once, in Widget's own constructor body, from
    // detail::pending_host_stack() — see the comment there and on host()
    // above. No default initializer needed: the constructor always runs
    // before anything can observe this member.
    Host* host_;
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    bool has_focus_ = false;
    int z_order_ = 0;
    std::optional<Color> background_color_;

    // Re-establishes the "children_ is sorted by ascending z_order(), ties
    // broken by prior relative order" invariant. std::stable_sort keeps
    // any equal-z_order (the common default-0) children in their existing
    // relative order, so this is a no-op reorder until something opts in.
    // Called whenever a child's z_order changes, and once more at the end
    // of add_child() (covers a child whose z_order was set before it was
    // ever added, while parent_ was still null and set_z_order() couldn't
    // reach a parent to resort). Every existing traversal of children_
    // (paint_children(), snapshot_children_rev(), hit_test()'s inline
    // reverse iteration) reads children_ directly and needs no changes —
    // they simply see it pre-sorted.
    void resort_children_()
    {
        std::stable_sort(children_.begin(), children_.end(),
                         [](const std::unique_ptr<Widget>& a, const std::unique_ptr<Widget>& b)
                         { return a->z_order() < b->z_order(); });
    }

    // Out-of-line so widget.h doesn't need Host's complete type (host.h
    // includes widget.h, not the reverse) — defined in widget.cpp, which
    // does include host.h. Called by set_visible()/add_child()/
    // set_layout_hints() here, and by remove_child()/clear_children() in
    // widget.cpp directly. No-op when host_ is null (a detached/popup-
    // mounted tree — see host()'s own doc comment above).
    void mark_relayout_needed_();

    template <typename T>
    friend std::weak_ptr<T> track(T* w);

    // Only Host may flip has_focus_ — it's the sole authority on which
    // widget currently holds tk-level keyboard focus.
    friend class Host;
    void set_focused_(bool focused)
    {
        has_focus_ = focused;
    }
};

// The literal top of every surface's widget tree, inserted by Host::set_root()
// to wrap whatever widget the shell constructs. Its only job is knowing which
// Host owns this tree, so remove_child()/clear_children() can hand a removed
// subtree straight to Host::queue_for_deletion() — no per-widget callback
// needed. Transparent pass-through: exactly one child (the shell's real
// widget); every method except measure() inherits Widget's default, which
// already just applies to every child.
class RootWidget : public Widget
{
public:
    Size measure(LayoutCtx& ctx, Size constraints) override
    {
        return children().empty() ? Size{} : children().front()->measure(ctx, constraints);
    }

    // Every Surface::root() across all four backends is a RootWidget, and
    // every shell's apply_theme_ui_() calls root()->apply_theme(theme) for
    // every window (main app, settings, account picker, branding,
    // popouts) — so this establishes the page-level background exactly
    // once, generically, for the whole tree. It matches the color the
    // backend's own canvas->clear() paints this window with every frame
    // (e.g. host_qt.cpp's Surface::paint()), so any nested field with no
    // closer ancestor override (see Widget::background_color()) still gets
    // a correct, opaque backdrop for its offscreen text capture instead of
    // an unset/transparent one.
    void on_theme_changed(const Theme& theme) override
    {
        set_background_color(theme.palette.bg);
    }

    void queue_for_deletion(std::unique_ptr<Widget> subtree);

protected:
    RootWidget() = default;
    TK_WIDGET_FACTORY_FRIEND(RootWidget)
};

// ── Widget-construction factory (definitions) ───────────────────────────
//
// Construction goes through `::operator new` + placement-new directly in
// create_impl()'s own body, not std::make_unique<T>(): the `new T(...)`
// expression has to lexically appear inside create_impl() itself for
// TK_WIDGET_FACTORY_FRIEND's per-function friendship (granted to
// create_impl specifically) to apply — std::make_unique performs its own
// `new` expression from within <memory>'s own code, which gets no such
// friendship, so it can't construct a T whose constructor is protected.
namespace detail
{

template <typename T, typename... Args>
std::unique_ptr<T> create_impl(Host* host, Args&&... args)
{
    auto& stack = pending_host_stack();
    stack.push_back(host);
    struct PopGuard
    {
        std::vector<Host*>& s;
        ~PopGuard() { s.pop_back(); }
    } pop{stack};

    void* mem = ::operator new(sizeof(T));
    T* obj;
    try
    {
        obj = ::new (mem) T(std::forward<Args>(args)...);
    }
    catch (...)
    {
        ::operator delete(mem);
        throw;
    }

    assert(obj->host() == host &&
           "pending-host stack mismatch — possible reentrant create_widget bug");
    return std::unique_ptr<T>(obj);
}

} // namespace detail

// Constructs a widget nested inside another widget's own constructor body
// (the common case). Reuses whatever Host is already resolved on `parent`
// — if `parent` has itself been constructed via create_widget()/
// create_root_widget(), its host() is already valid by the time its own
// constructor body runs (base subobjects finish constructing before the
// derived constructor body executes), so this is safe to call from within
// that body with parent == this.
template <typename T, typename... Args>
std::unique_ptr<T> create_widget(Widget* parent, Args&&... args)
{
    return detail::create_impl<T>(parent ? parent->host() : nullptr, std::forward<Args>(args)...);
}

// Constructs the top of a fresh subtree, for shell code that already holds
// a live Host* (a Surface constructs its Host before any widget exists).
template <typename T, typename... Args>
std::unique_ptr<T> create_root_widget(Host* host, Args&&... args)
{
    return detail::create_impl<T>(host, std::forward<Args>(args)...);
}

// Takes a weak_ptr to any Widget subtype without granting ownership —
// children_ (via unique_ptr) remains the sole owner. Use in Host to track
// "the widget currently under the pointer" etc. without risking a dangling
// raw pointer: .lock() returns null once the widget is actually destroyed,
// regardless of whether that happens synchronously or via a deferred
// deletion queue.
template <typename T>
std::weak_ptr<T> track(T* w)
{
    if (!w)
        return {};
    return w->template weak_self<T>();
}

// Depth-first walk of `root`'s subtree in reading order — top-to-bottom
// rows, left-to-right within a row, computed from each widget's own
// bounds() rather than add_child() order (see collect_focus_order's own
// comment in widget.cpp for the row/x-vs-y comparator) — used to compute
// Tab/Shift-Tab traversal among tk-focusable widgets. This makes Stack/
// rect-positioned children (grids, pickers) traverse sensibly too, not just
// VBox/HBox ones. Skips invisible subtrees entirely; candidates are
// filtered to visible() && enabled() && focusable(). `current == nullptr`
// returns the first (forward) or last (!forward) candidate — used when
// nothing is focused yet. Wraps around at the ends; returns nullptr only
// when `root`'s subtree contains no focusable widget at all.
Widget* next_focusable(Widget* root, Widget* current, bool forward);

// Reading-order comparator shared by next_focusable()'s traversal and
// access_tree.h's — kept as one definition so accessibility reading order
// and Tab order can never silently drift apart (see reading_order_less's
// own doc comment in widget.cpp for the row/x-vs-y logic).
bool reading_order_less(const Rect& a, const Rect& b);


// Walks `w`'s ancestor chain, calling scroll_into_view() on every
// tk::ScrollableBase found (keeps walking past the first match in case of
// nested regions — none exist today, but it costs nothing extra). See
// ScrollableBase::scroll_into_view (scrollable_base.h) for the mechanism —
// every scrollable widget in the codebase is a ScrollableBase.
void scroll_widget_into_view(Widget* w);

} // namespace tk
