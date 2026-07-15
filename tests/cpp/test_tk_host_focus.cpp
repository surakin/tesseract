#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

// Exercises Host::focus_visible_ — the ":focus-visible" policy gating whether
// paint_focus_overlay() actually draws the keyboard-focus ring. Mouse clicks
// move tk-level focus but must not show the ring; only keyboard-driven focus
// changes should (Host::dispatch_key_down for any key, and Host::advance_focus()
// for the native-text-control Tab-forwarding path, which never goes through
// dispatch_key_down at all — see host.cpp's own comment on advance_focus_()).

using namespace tk;

namespace
{

class FocusProbeWidget : public Widget
{
public:
    explicit FocusProbeWidget(Rect rect)
    {
        bounds_ = rect; // world bounds drive contains_world / hit_test descent
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool on_pointer_down(Point) override
    {
        return true;
    }
    bool focusable() const override
    {
        return true;
    }
};

// Claims pointer-downs for its own reasons (mirrors e.g. MessageListView
// anchoring a text selection, or a panel's background dismiss-on-click) but
// is not itself a focus target — the default focusable() == false.
class ClaimingNonFocusableWidget : public Widget
{
public:
    explicit ClaimingNonFocusableWidget(Rect rect)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool on_pointer_down(Point) override
    {
        return true;
    }
};

// Mirrors e.g. RoomListView's inner row list: stays focusable() (so Tab
// still reaches it for keyboard row-navigation) but opts out of the
// click-focus grab via focus_on_click() == false, since its own click
// handling (row selection) is a complete action that shouldn't also steal
// focus from whatever else — e.g. a compose box — currently has it.
class FocusableNoClickWidget : public Widget
{
public:
    explicit FocusableNoClickWidget(Rect rect)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool on_pointer_down(Point) override
    {
        return true;
    }
    bool focusable() const override
    {
        return true;
    }
    bool focus_on_click() const override
    {
        return false;
    }
};

// Mirrors tk::TextField/TextArea: focusable() and reports
// holds_native_focus() == true, i.e. it owns a real native OS control that
// itself takes care of holding real keyboard focus once tk-focused.
class NativeFocusHoldingWidget : public Widget
{
public:
    explicit NativeFocusHoldingWidget(Rect rect)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool focusable() const override
    {
        return true;
    }
    bool holds_native_focus() const override
    {
        return true;
    }
};

// Overrides paint_own_focus_ring() to draw at a fixed rect/radius that's
// deliberately different from (and non-overlapping with) its own bounds()
// — mirrors ComposeBar::ComposerTextArea, which traces the whole compose
// card's outline instead of its own narrow text-column bounds.
class CustomRingWidget : public Widget
{
public:
    explicit CustomRingWidget(Rect bounds, Rect ring_rect)
        : ring_rect_(ring_rect)
    {
        bounds_ = bounds;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool focusable() const override
    {
        return true;
    }
    void paint_own_focus_ring(PaintCtx& ctx) override
    {
        paint_focus_ring(ctx, ring_rect_, 10.0f);
    }

private:
    Rect ring_rect_;
};

// Counts claim_native_focus_container_() calls instead of the default no-op,
// so tests can assert exactly when Host::request_focus() decides a backend
// needs to park real native focus on its canvas-hosting container.
class ClaimCountingHost : public TestHost
{
public:
    explicit ClaimCountingHost(Widget* root) : TestHost(root) {}

    int claims = 0;

protected:
    void claim_native_focus_container_() override
    {
        ++claims;
    }
};

// Plain container relying on the base Widget::dispatch_pointer_down tree
// walk (children topmost-first, then this widget's own on_pointer_down).
class ContainerWidget : public Widget
{
public:
    explicit ContainerWidget(Rect rect)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
};

bool nearly(Color a, Color b, int tol = 20)
{
    auto delta = [](int x, int y)
    {
        return x > y ? x - y : y - x;
    };
    return delta(a.r, b.r) <= tol && delta(a.g, b.g) <= tol &&
           delta(a.b, b.b) <= tol;
}

} // namespace

TEST_CASE("Host::dispatch_key_down sets focus_visible_ for any key",
          "[tk][host][focus]")
{
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);
    host.focus_visible_ = false;

    host.dispatch_key_down({Key::Enter});
    CHECK(host.focus_visible_);
}

TEST_CASE("Host::dispatch_key_down Tab moves focus and sets focus_visible_",
          "[tk][host][focus]")
{
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);
    host.focus_visible_ = false;

    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == &root);
    CHECK(host.focus_visible_);
}

TEST_CASE("Host::dispatch_pointer_down clears focus_visible_ even on an "
          "empty-space click",
          "[tk][host][focus]")
{
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);
    host.focus_visible_ = true;

    // Click outside root's bounds entirely — no widget claims it, but the
    // user is still unambiguously using the mouse right now.
    host.dispatch_pointer_down({500, 500});
    CHECK_FALSE(host.focus_visible_);
}

TEST_CASE("Host::dispatch_pointer_down moves focus but clears focus_visible_",
          "[tk][host][focus]")
{
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);

    host.dispatch_pointer_down({10, 10}); // inside root, focusable
    CHECK(host.focused_widget() == &root);
    CHECK_FALSE(host.focus_visible_);
}

TEST_CASE("Host::advance_focus() (native-overlay Tab-forwarding path) sets "
          "focus_visible_ without going through dispatch_key_down",
          "[tk][host][focus]")
{
    // Regression coverage for the specific gap the design closes: a native
    // text control's own Tab key handling forwards an unconsumed Tab via the
    // public advance_focus() wrapper directly (see tk::TextField's
    // set_on_popup_nav), never touching Host::dispatch_key_down. A fix that
    // only set focus_visible_ inside dispatch_key_down would fail this.
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);
    host.focus_visible_ = false;

    CHECK(host.advance_focus(/*forward=*/true));
    CHECK(host.focused_widget() == &root);
    CHECK(host.focus_visible_);
}

TEST_CASE("Tab then click ends with the ring hidden; click then Tab ends "
          "with it visible",
          "[tk][host][focus]")
{
    FocusProbeWidget root({0, 0, 200, 200});
    TestHost host(&root);

    host.dispatch_key_down({Key::Tab});
    CHECK(host.focus_visible_);
    host.dispatch_pointer_down({10, 10});
    CHECK_FALSE(host.focus_visible_);

    host.clear_focus();
    host.dispatch_pointer_down({10, 10});
    CHECK_FALSE(host.focus_visible_);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focus_visible_);
}

TEST_CASE("paint_focus_overlay draws the ring when focus_visible_ is true",
          "[tk][host][focus]")
{
    const Rect bounds{10, 10, 120, 36};
    FocusProbeWidget root(bounds);
    TestHost host(&root);
    auto surface = TestSurface::create(200, 200);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    pc.host = &host;

    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == &root);
    REQUIRE(host.focus_visible_);
    host.paint_focus_overlay(pc);

    // Sample mid-edge (away from the ring's 4px corner radius) along the
    // top stroke line — see paint_focus_ring()'s 2px-wide stroke geometry.
    const int sample_x = static_cast<int>(bounds.x + bounds.w / 2);
    const int sample_y = static_cast<int>(bounds.y + 1);
    CHECK(nearly(surface->read_pixel(sample_x, sample_y),
                Theme::light().palette.accent));
}

TEST_CASE("paint_focus_overlay skips the ring when focus_visible_ is false, "
          "even with a focused widget",
          "[tk][host][focus]")
{
    const Rect bounds{10, 10, 120, 36};
    FocusProbeWidget root(bounds);
    TestHost host(&root);
    auto surface = TestSurface::create(200, 200);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    pc.host = &host;

    // Focus via mouse click (inside root, which is focusable) — moves
    // tk-level focus but must not make the ring visible.
    host.dispatch_pointer_down({15, 15});
    REQUIRE(host.focused_widget() == &root);
    REQUIRE_FALSE(host.focus_visible_);
    host.paint_focus_overlay(pc);

    const int sample_x = static_cast<int>(bounds.x + bounds.w / 2);
    const int sample_y = static_cast<int>(bounds.y + 1);
    CHECK_FALSE(nearly(surface->read_pixel(sample_x, sample_y),
                       Theme::light().palette.accent));
}

TEST_CASE("Host::dispatch_pointer_down does not clear focus when a click "
          "lands on a non-focusable widget that claims it for its own "
          "reasons",
          "[tk][host][focus]")
{
    // Regression test: Host::dispatch_pointer_down used to clear_focus()
    // whenever the widget that claimed a pointer-down wasn't itself
    // focusable() — a blanket "click anywhere blurs the previous control"
    // rule that broke as soon as a widget claimed clicks for reasons
    // unrelated to keyboard focus (MessageListView anchoring a text
    // selection, UserProfilePanel's background, etc. — nearly every widget
    // in a real app does this). Only a click that claims *nothing at all*
    // anywhere in the tree should clear focus now.
    ContainerWidget root({0, 0, 200, 200});
    auto* target = root.add_child(
        std::make_unique<FocusProbeWidget>(Rect{0, 0, 50, 50}));
    root.add_child(
        std::make_unique<ClaimingNonFocusableWidget>(Rect{100, 0, 50, 50}));
    TestHost host(&root);

    host.dispatch_pointer_down({25, 25}); // inside target
    REQUIRE(host.focused_widget() == target);

    host.dispatch_pointer_down({125, 25}); // inside claiming, not target
    CHECK(host.focused_widget() == target); // untouched, not cleared

    host.dispatch_pointer_down({500, 500}); // genuinely unclaimed
    CHECK(host.focused_widget() == nullptr);
}

TEST_CASE("Host::dispatch_pointer_down does not move focus onto a "
          "focusable widget that opts out via focus_on_click(), but Tab "
          "still reaches it",
          "[tk][host][focus]")
{
    // Regression test for RoomListView's inner row list: it must stay
    // focusable() (real Tab/arrow-key row-navigation feature) but an
    // ordinary click on it must not steal focus from whatever else — e.g.
    // the compose box, modeled here by `target` — currently holds it.
    ContainerWidget root({0, 0, 200, 200});
    auto* target = root.add_child(
        std::make_unique<FocusProbeWidget>(Rect{0, 0, 50, 50}));
    auto* no_click_target = root.add_child(
        std::make_unique<FocusableNoClickWidget>(Rect{100, 0, 50, 50}));
    TestHost host(&root);

    host.dispatch_pointer_down({25, 25}); // inside target
    REQUIRE(host.focused_widget() == target);

    host.dispatch_pointer_down({125, 25}); // inside no_click_target
    CHECK(host.focused_widget() == target); // untouched, not stolen

    // Still genuinely focusable() via Tab traversal.
    CHECK(no_click_target->focusable());
    CHECK_FALSE(no_click_target->focus_on_click());
}

TEST_CASE("Host::request_focus claims the native focus container when the "
          "target has no native control of its own",
          "[tk][host][focus]")
{
    // Regression test for a live bug: Tab from the compose box (a native
    // text area) to the emoji button (a plain Button) left NOTHING holding
    // real OS keyboard focus — the text area released it via its own
    // on_focus_lost(), and a plain Button's on_focus_gained() is a no-op,
    // so no native control ever claimed it. The very next Tab press then
    // had no widget to deliver the key event to at all ("nothing happens"),
    // and the press after that landed wherever stray native focus ended up,
    // restarting traversal with no tk-widget considered current (observed
    // live as Tab snapping back to the very first stop). See
    // Host::claim_native_focus_container_'s doc comment.
    ContainerWidget root({0, 0, 200, 200});
    auto* plain = root.add_child(
        std::make_unique<FocusProbeWidget>(Rect{0, 0, 50, 50}));
    ClaimCountingHost host(&root);

    host.request_focus(plain);
    CHECK(host.claims == 1);

    // Idempotent re-assert (mirrors request_focus's own "always re-assert
    // focus-gained" comment) — harmless to call again on every Tab that
    // lands on another plain widget.
    host.request_focus(plain);
    CHECK(host.claims == 2);
}

TEST_CASE("Host::request_focus does not claim the native focus container "
          "when the target holds native focus itself",
          "[tk][host][focus]")
{
    ContainerWidget root({0, 0, 200, 200});
    auto* native = root.add_child(
        std::make_unique<NativeFocusHoldingWidget>(Rect{0, 0, 50, 50}));
    ClaimCountingHost host(&root);

    host.request_focus(native);
    CHECK(host.claims == 0);
}

TEST_CASE("paint_focus_overlay dispatches to a widget's own "
          "paint_own_focus_ring override, not the default bounds()-based ring",
          "[tk][host][focus]")
{
    // Regression test for ComposeBar::ComposerTextArea: a widget that
    // overrides paint_own_focus_ring() must have ITS geometry drawn, not
    // Widget::paint_own_focus_ring's default (a ring around bounds()).
    const Rect bounds{10, 10, 40, 20};
    const Rect ring_rect{100, 100, 60, 60}; // disjoint from bounds
    CustomRingWidget root(bounds, ring_rect);
    TestHost host(&root);
    auto surface = TestSurface::create(200, 200);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    pc.host = &host;

    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == &root);
    REQUIRE(host.focus_visible_);
    host.paint_focus_overlay(pc);

    // Inside ring_rect (mid-edge, away from its 10px corner radius), not
    // bounds() — proves the override's geometry was actually used.
    const int ring_x = static_cast<int>(ring_rect.x + ring_rect.w / 2);
    const int ring_y = static_cast<int>(ring_rect.y + 1);
    CHECK(nearly(surface->read_pixel(ring_x, ring_y),
                Theme::light().palette.accent));

    // bounds()'s own top edge must NOT show the ring — the override fully
    // replaced the default, it didn't draw both.
    const int bounds_x = static_cast<int>(bounds.x + bounds.w / 2);
    const int bounds_y = static_cast<int>(bounds.y + 1);
    CHECK_FALSE(nearly(surface->read_pixel(bounds_x, bounds_y),
                       Theme::light().palette.accent));
}

TEST_CASE("paint_focus_overlay uses the default bounds()-based ring for a "
          "widget with no paint_own_focus_ring override",
          "[tk][host][focus]")
{
    // Regression coverage that Widget::paint_own_focus_ring's default
    // behavior (and hence every widget that doesn't override it — e.g. the
    // compose bar's emoji/sticker/mic/send buttons) is unaffected by the
    // new virtual hook.
    const Rect bounds{10, 10, 120, 36};
    FocusProbeWidget root(bounds);
    TestHost host(&root);
    auto surface = TestSurface::create(200, 200);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    pc.host = &host;

    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == &root);
    host.paint_focus_overlay(pc);

    const int sample_x = static_cast<int>(bounds.x + bounds.w / 2);
    const int sample_y = static_cast<int>(bounds.y + 1);
    CHECK(nearly(surface->read_pixel(sample_x, sample_y),
                Theme::light().palette.accent));
}
