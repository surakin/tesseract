#pragma once

// Widget tree. Pure C++, no platform dependencies. Each widget knows
// how to (a) measure itself given a width × height constraint, (b)
// paint itself into a Canvas, and (c) hit-test pointer events. Layout
// is the classic two-pass measure-then-arrange model: measure() returns
// the desired size, arrange() commits the final bounds.

#include "canvas.h"
#include "theme.h"

#include <functional>
#include <memory>
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

// Forward declaration — avoids a circular include (host.h includes widget.h).
class Host;

struct PaintCtx
{
    Canvas& canvas;
    CanvasFactory& factory;
    const Theme& theme;
    AnimDamageSink* anim_damage = nullptr;
    Host*           host        = nullptr;
};

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

class Widget
{
public:
    virtual ~Widget() = default;

    // Compute the desired size given the maximum bounds available. The
    // returned size is the natural size at or below `constraints`.
    virtual Size measure(LayoutCtx&, Size constraints) = 0;

    // Commit the final bounds. Default sets bounds_ and arranges all
    // children at the same bounds (overridden by layout containers).
    virtual void arrange(LayoutCtx&, Rect bounds);

    // Paint into the canvas at this widget's bounds. The default paints all
    // visible children in insertion order, matching hit-test / overlay order.
    // Leaf widgets override this; containers can either inherit it or call
    // paint_children() around their own chrome drawing.
    virtual void paint(PaintCtx&);

    // Second paint pass, called by the host after the entire widget tree's
    // paint() has finished. Default propagates to children so every
    // container forwards the pass automatically. Override (without calling
    // the base) to draw popup content that must render above all other widgets.
    virtual void paint_overlay(PaintCtx& ctx)
    {
        for (auto& ch : children())
            if (ch->visible()) ch->paint_overlay(ctx);
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
    virtual bool on_wheel(Point /*local*/, float /*dx*/, float /*dy*/)
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
    virtual void on_focus_gained()
    {
    }
    virtual void on_focus_lost()
    {
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
    virtual bool dispatch_wheel(Point world, float dx, float dy);

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
    bool visible() const
    {
        return visible_;
    }
    void set_visible(bool v)
    {
        visible_ = v;
    }
    bool enabled() const
    {
        return enabled_;
    }
    // Virtual so widgets that need extra bookkeeping on disable (e.g.
    // ComboBox collapsing an open dropdown) can extend it; the base just
    // stores the flag. Hover is gated on enabled_ centrally — see
    // dispatch_pointer_move() and each leaf widget's paint() — so overrides
    // don't need to handle hover themselves.
    virtual void set_enabled(bool enabled)
    {
        enabled_ = enabled;
    }
    void set_layout_hints(LayoutHints h)
    {
        hints_ = h;
    }
    LayoutHints layout_hints() const
    {
        return hints_;
    }

    // Take ownership of a child. Returns a borrowed pointer for callers
    // that want to keep wiring (e.g. on_click handlers, dynamic state).
    template <typename W>
    W* add_child(std::unique_ptr<W> w)
    {
        W* raw = w.get();
        w->parent_ = this;
        children_.push_back(std::move(w));
        return raw;
    }

    // Removes a direct child and returns ownership.
    // Borrowed pointers to the removed widget become dangling — callers must clear them.
    // Asserts that child->parent_ == this.
    std::unique_ptr<Widget> remove_child(Widget* child);

    // Drop every child widget. Any borrowed pointers returned from
    // add_child() are dangling after this call — callers must clear them.
    void clear_children()
    {
        children_.clear();
    }

    const std::vector<std::unique_ptr<Widget>>& children() const
    {
        return children_;
    }

    // Installed once by the platform Host on the surface root widget.
    // Fires in remove_child() before the child subtree is freed so the
    // Host can clear any dangling Widget pointers it holds internally.
    // Not propagated to children — only set on the root of each surface.
    void set_subtree_removing_cb(std::function<void(Widget*)> cb)
    {
        subtree_removing_cb_ = std::move(cb);
    }

protected:
    void paint_children(PaintCtx&);

    Rect bounds_{};
    LayoutHints hints_{};
    bool visible_ = true;
    bool enabled_ = true;

private:
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    std::function<void(Widget*)> subtree_removing_cb_;
};

} // namespace tk
