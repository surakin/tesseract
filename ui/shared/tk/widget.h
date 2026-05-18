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

namespace tk {

// Inset/outset on the four edges, in logical pixels. The "trbl" naming
// matches the CSS order so reading mixed sources stays unambiguous.
struct Edges {
    float top    = 0;
    float right  = 0;
    float bottom = 0;
    float left   = 0;

    static constexpr Edges all(float v)              { return { v, v, v, v }; }
    static constexpr Edges symmetric(float h, float v) { return { v, h, v, h }; }
    constexpr float horizontal() const { return left + right; }
    constexpr float vertical()   const { return top + bottom; }
};

// Flex-style axis alignment. Main = along the layout axis (vertical for
// VBox, horizontal for HBox); Cross = perpendicular to it.
enum class Main  { Start, Center, End, SpaceBetween, SpaceAround };
enum class Cross { Start, Center, End, Stretch };

// Per-widget tug on its parent's measure: when fill_main == true the
// widget asks for the leftover main-axis space; fill_cross == true asks
// to span the cross axis. Used by layout containers; ignored otherwise.
struct LayoutHints {
    bool fill_main  = false;
    bool fill_cross = false;
};

// Layout / paint contexts. Widgets never reach for global state; the
// owning tree threads these in from the platform host.
struct LayoutCtx {
    CanvasFactory& factory;
    const Theme&   theme;
};
struct PaintCtx {
    Canvas&        canvas;
    CanvasFactory& factory;
    const Theme&   theme;
};

class Widget {
public:
    virtual ~Widget() = default;

    // Compute the desired size given the maximum bounds available. The
    // returned size is the natural size at or below `constraints`.
    virtual Size measure(LayoutCtx&, Size constraints) = 0;

    // Commit the final bounds. Default sets bounds_ and arranges all
    // children at the same bounds (overridden by layout containers).
    virtual void arrange(LayoutCtx&, Rect bounds);

    // Paint into the canvas at this widget's bounds (in parent-local
    // coordinates — the parent has already translated the canvas).
    virtual void paint(PaintCtx&) = 0;

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
    virtual bool on_wheel(Point /*local*/, float /*dx*/, float /*dy*/) {
        return false;
    }

    // Pointer-down. Return true to claim ownership of the subsequent
    // pointer_up. The host remembers the claimer so a release outside
    // the widget still routes back to it (with inside_self=false).
    virtual bool on_pointer_down(Point /*local*/) { return false; }

    // Pointer-up on the widget that consumed the matching pointer_down.
    // `inside_self` tells the widget whether the release landed inside
    // its own bounds — Button uses this to fire on_click only when the
    // user released on the same control they pressed.
    virtual void on_pointer_up(Point /*local*/, bool /*inside_self*/) {}

    // Pointer-drag: the host forwards every pointer-move after a
    // pointer-down that this widget claimed, until the matching
    // pointer-up arrives. `local` is in widget-local coordinates and
    // can land outside the widget's bounds when the user drags off the
    // control. Used by ListView for scrollbar-thumb dragging.
    virtual void on_pointer_drag(Point /*local*/) {}

    // Pointer-move without a press. Called via `dispatch_pointer_move`
    // on the deepest hit widget so views can update per-element hover
    // state (reaction chips, row-hover affordances, etc.). Buttons
    // continue to handle their own hover via the host's Button-hover
    // bookkeeping — this is the generic widget-level hook.
    // Returns true if visual state changed and a repaint is needed.
    virtual bool on_pointer_move(Point /*local*/) { return false; }
    // Mirrors `on_pointer_leave` semantics from the host so widgets can
    // clear hover state when the pointer leaves the surface (or the
    // pointer-move dispatch lands on a different widget).
    virtual void on_pointer_leave() {}

    // Walk into the deepest visible child under `world`, then bubble
    // back up to find a widget whose on_pointer_down returns true.
    // Returns the claiming widget (or nullptr if none did). `world` is
    // in root-surface coordinates (the space `bounds_` is stored in).
    // The widget-local Point handed to on_pointer_down is computed by
    // subtracting the claimer's own world origin.
    Widget* dispatch_pointer_down(Point world);

    // Walk into the deepest visible child under `world` and call
    // `on_pointer_move(local)` on it. Returns the deepest widget that
    // received the event (or nullptr if `world` was outside), and sets
    // *dirty = true if the widget reported a visual change. Hosts call
    // this from their pointer-move handler when no widget holds the press.
    virtual Widget* dispatch_pointer_move(Point world, bool* dirty = nullptr);

    // Walk into the hit widget, then bubble up through parents until
    // someone consumes the wheel event. `world` is in root-surface
    // coordinates. Called by hosts on the root.
    bool dispatch_wheel(Point world, float dx, float dy);

    // Translate a point from root-surface coordinates into this widget's
    // local coordinate system. Since `bounds_` is stored in world coords
    // throughout the tree, this is just `world - this->bounds_`.
    Point world_to_local(Point world) const;

    // World-coords containment check used by the dispatch + hit-test
    // routines above. Exposed as a member so subclasses (notably
    // clip-respecting containers like ScrollView) can reuse the same
    // half-open rect test.
    bool contains_world(Point world) const;

    // Tree.
    Widget*  parent() const { return parent_; }
    Rect     bounds() const { return bounds_; }
    bool     visible() const { return visible_; }
    void     set_visible(bool v) { visible_ = v; }
    void     set_layout_hints(LayoutHints h) { hints_ = h; }
    LayoutHints layout_hints() const { return hints_; }

    // Take ownership of a child. Returns a borrowed pointer for callers
    // that want to keep wiring (e.g. on_click handlers, dynamic state).
    template <typename W>
    W* add_child(std::unique_ptr<W> w) {
        W* raw = w.get();
        w->parent_ = this;
        children_.push_back(std::move(w));
        return raw;
    }

    const std::vector<std::unique_ptr<Widget>>& children() const {
        return children_;
    }

protected:
    Rect        bounds_{};
    LayoutHints hints_{};
    bool        visible_ = true;

private:
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
};

// Convenience for widgets that simply paint a coloured background.
class FillBackground : public Widget {
public:
    explicit FillBackground(Color c) : colour_(c) {}
    Size measure(LayoutCtx&, Size constraints) override {
        return constraints;
    }
    void paint(PaintCtx& ctx) override {
        ctx.canvas.fill_rect(bounds_, colour_);
        for (auto& ch : children()) {
            if (ch->visible()) ch->paint(ctx);
        }
    }
private:
    Color colour_;
};

} // namespace tk
