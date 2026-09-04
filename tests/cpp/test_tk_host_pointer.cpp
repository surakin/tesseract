#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/widget.h"
#include "tk_test_host.h"

#include <memory>

// Exercises the shared Host::dispatch_pointer_* state machine + popup routing
// (Phase 4.1: the logic was previously copy-pasted across all four host
// backends, including the Phase-1.7 popup branch with no test coverage). A
// minimal concrete Host stubs the platform-specific virtuals and exposes the
// protected dispatch entry points so we can drive them directly. ProbeWidget
// relies on the base Widget tree-walk (hit_test / dispatch_*) and only fixes
// its world bounds + records the leaf callbacks it receives.

using namespace tk;

namespace
{

class ProbeWidget : public Widget
{
public:
    ProbeWidget(Rect rect, bool claim_down = false) : claim_down_(claim_down)
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
        ++down_count;
        return claim_down_;
    }
    bool focusable() const override { return focusable_; }

    // Set to make this widget a keyboard-focus target (base Widget is not).
    bool focusable_ = false;
    void on_pointer_up(Point, bool inside) override
    {
        ++up_count;
        last_up_inside = inside;
    }
    void on_pointer_drag(Point) override { ++drag_count; }
    bool on_pointer_move(Point) override
    {
        ++move_count;
        return false;
    }
    void on_pointer_leave() override { ++leave_count; }
    void on_popup_dismiss() override { ++dismiss_count; }
    bool on_right_click(Point) override
    {
        ++right_count;
        return claim_generic;
    }
    bool on_drag_hover(Point) override
    {
        ++drag_hover_count;
        return claim_generic;
    }
    bool on_file_drop(Point, FileDropPayload&) override
    {
        ++file_drop_count;
        return claim_generic;
    }

    int down_count = 0;
    int up_count = 0;
    int drag_count = 0;
    int move_count = 0;
    int leave_count = 0;
    int dismiss_count = 0;
    int right_count = 0;
    int drag_hover_count = 0;
    int file_drop_count = 0;
    bool last_up_inside = false;
    // When set, on_right_click / on_drag_hover / on_file_drop claim the event.
    bool claim_generic = false;

private:
    bool claim_down_;
};

// Mirrors CheckButton::on_pointer_up invoking `on_change` directly (no
// copy-before-invoke, unlike Button::click()/SwitchButton::on_pointer_up()):
// the callback removes this widget from its parent, and on_pointer_up keeps
// touching `this` afterward. Only safe because subtree removal defers the
// actual free — see Host::queue_for_deletion().
class SelfRemovingWidget : public Widget
{
public:
    explicit SelfRemovingWidget(Rect rect)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}
    bool on_pointer_down(Point) override { return true; }
    void on_pointer_up(Point, bool) override
    {
        if (on_change)
            on_change();
        ++after_callback_count; // would be a use-after-free write if this
                                 // widget had already been freed by on_change
    }

    std::function<void()> on_change;
    int after_callback_count = 0;
};

} // namespace

TEST_CASE("Click outside an open popup dismisses it and falls through",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_down({10, 10}); // outside popup, inside root

    REQUIRE(host.popup() == nullptr);  // popup cleared
    REQUIRE(popup.dismiss_count == 1); // dismiss fired exactly once
    REQUIRE(popup.down_count == 0);    // popup never saw the press
    REQUIRE(root.down_count == 1);     // press fell through to the tree
}

TEST_CASE("Click inside an open popup routes to it and does not dismiss",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100}, /*claim_down=*/true);
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_down({120, 120}); // inside the popup

    REQUIRE(popup.down_count == 1);
    REQUIRE(popup.dismiss_count == 0);
    REQUIRE(host.popup() == &popup);         // still open
    REQUIRE(root.down_count == 0);           // tree never saw it
    REQUIRE(host.pressed_widget_.lock().get() == &popup); // popup captured the press
}

TEST_CASE("Click on the popup's registered trigger does not dismiss it",
          "[tk][host][popup]")
{
    // Regression coverage: clicking the control that opened a popup (e.g.
    // the emoji-picker toggle button) used to unconditionally dismiss it as
    // an "outside click" before the trigger's own handler got a chance to
    // decide — causing a dismiss-then-reopen flicker. register_popup()'s
    // optional trigger widget exempts exactly this case.
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget* trigger =
        root.add_child(std::make_unique<ProbeWidget>(Rect{10, 10, 30, 30}));
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup, trigger);

    host.dispatch_pointer_down({20, 20}); // inside trigger, outside popup

    REQUIRE(popup.dismiss_count == 0); // dismiss skipped for the trigger
    REQUIRE(host.popup() == &popup);   // popup stays registered
    REQUIRE(trigger->down_count == 1); // press still reached the trigger
}

TEST_CASE("Click outside both the popup and its trigger still dismisses it",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget* trigger =
        root.add_child(std::make_unique<ProbeWidget>(Rect{10, 10, 30, 30}));
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup, trigger);

    host.dispatch_pointer_down({200, 200}); // outside both, inside root

    REQUIRE(popup.dismiss_count == 1);
    REQUIRE(host.popup() == nullptr);
}

TEST_CASE("Pointer-move inside an open popup routes hover to it, not the tree",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_move({130, 130}); // inside the popup

    REQUIRE(popup.move_count == 1);
    REQUIRE(root.move_count == 0);
    REQUIRE(host.hovered_widget_.lock().get() == &popup);
}

TEST_CASE("Captured widget gets drag on move and inside-release on up",
          "[tk][host][popup]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* child =
        root->add_child(std::make_unique<ProbeWidget>(
            Rect{50, 50, 100, 100}, /*claim_down=*/true));
    TestHost host(root.get());

    host.dispatch_pointer_down({60, 60}); // press inside the claiming child
    REQUIRE(host.pressed_widget_.lock().get() == child);

    host.dispatch_pointer_move({70, 70}); // captured → drag, no hover
    REQUIRE(child->drag_count == 1);
    REQUIRE(child->move_count == 0);

    host.dispatch_pointer_up({80, 80}); // inside child's bounds
    REQUIRE(child->up_count == 1);
    REQUIRE(child->last_up_inside == true);
    REQUIRE(host.pressed_widget_.expired());
}

TEST_CASE("Pointer-leave clears hover and synthesises an outside release",
          "[tk][host][popup]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* child =
        root->add_child(std::make_unique<ProbeWidget>(
            Rect{50, 50, 100, 100}, /*claim_down=*/true));
    TestHost host(root.get());

    host.dispatch_pointer_down({60, 60});
    REQUIRE(host.pressed_widget_.lock().get() == child);

    host.dispatch_pointer_leave();
    REQUIRE(child->up_count == 1);
    REQUIRE(child->last_up_inside == false); // synthetic outside release
    REQUIRE(host.pressed_widget_.expired());
}

TEST_CASE("clear_children()/remove_child() defer actual destruction until "
          "the host drains its queue",
          "[tk][host][deferred-deletion]")
{
    auto inner_root_owned = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    Widget* inner_root = inner_root_owned.get();
    ProbeWidget* raw_child = inner_root_owned->add_child(
        std::make_unique<ProbeWidget>(Rect{0, 0, 100, 100}));
    TestHost host(inner_root);
    auto wrapper = create_root_widget<RootWidget>(&host);
    wrapper->add_child(std::move(inner_root_owned));

    std::weak_ptr<Widget> tracked = track(static_cast<Widget*>(raw_child));
    REQUIRE_FALSE(tracked.expired());

    inner_root->clear_children();
    REQUIRE(inner_root->children().empty()); // detached immediately
    REQUIRE_FALSE(tracked.expired());        // but not yet actually destroyed

    host.fire_all_ui_tasks(); // runs the posted drain
    REQUIRE(tracked.expired()); // now actually freed
}

TEST_CASE("A widget can safely remove itself from within its own callback "
          "when destruction is deferred",
          "[tk][host][deferred-deletion]")
{
    auto inner_root_owned = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    Widget* inner_root = inner_root_owned.get();
    SelfRemovingWidget* child = inner_root_owned->add_child(
        std::make_unique<SelfRemovingWidget>(Rect{0, 0, 100, 100}));
    TestHost host(inner_root);
    auto wrapper = create_root_widget<RootWidget>(&host);
    wrapper->add_child(std::move(inner_root_owned));

    child->on_change = [inner_root, child] { inner_root->remove_child(child); };

    host.dispatch_pointer_down({10, 10});
    host.dispatch_pointer_up({10, 10}); // -> on_pointer_up -> on_change ->
                                         // remove_child(child), then
                                         // ++after_callback_count on `this`

    // If destruction weren't deferred, the increment above (and this read)
    // would be a use-after-free.
    CHECK(child->after_callback_count == 1);
    CHECK(inner_root->children().empty());

    host.fire_all_ui_tasks(); // now actually destroy it
}

// ─────────────────────────────────────────────────────────────────────────
//  Disabled widgets are opaque to input — they swallow everything that
//  lands on them and never let it reach their subtree, the z-order siblings
//  painted behind them, or their ancestors.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("A disabled widget absorbs a press instead of letting it fall "
          "through to a sibling behind it",
          "[tk][widget][disabled]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    // `behind` is added first → earlier in children_ → painted under `front`,
    // hit-tested after it. It claims every press it sees.
    ProbeWidget* behind = root->add_child(
        std::make_unique<ProbeWidget>(Rect{50, 50, 100, 100},
                                      /*claim_down=*/true));
    ProbeWidget* front = root->add_child(
        std::make_unique<ProbeWidget>(Rect{50, 50, 100, 100}));

    // Sanity: while `front` is enabled but doesn't claim the press, it falls
    // through to `behind` (the historical behaviour).
    Widget* hit = root->dispatch_pointer_down({60, 60});
    CHECK(hit == behind);
    CHECK(behind->down_count == 1);

    // Disable `front` — now it swallows the press; `behind` sees nothing more.
    front->set_enabled(false);
    const int front_downs = front->down_count;
    hit = root->dispatch_pointer_down({60, 60});
    CHECK(hit == front);
    CHECK(front->down_count == front_downs); // on_pointer_down not called again
    CHECK(behind->down_count == 1);          // unchanged — no fall-through
    CHECK(root->down_count == 0);            // parent never gets it either
}

TEST_CASE("A disabled container makes its whole subtree non-interactable",
          "[tk][widget][disabled]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* panel =
        root->add_child(std::make_unique<ProbeWidget>(Rect{0, 0, 200, 200}));
    ProbeWidget* child = panel->add_child(
        std::make_unique<ProbeWidget>(Rect{20, 20, 60, 60},
                                      /*claim_down=*/true));

    panel->set_enabled(false);

    Widget* hit = root->dispatch_pointer_down({30, 30});
    CHECK(hit == panel);
    CHECK(child->down_count == 0); // enabled child inside a disabled parent
    CHECK(panel->down_count == 0);
}

TEST_CASE("A disabled widget absorbs hover, right-click, drag-hover and "
          "file-drop",
          "[tk][widget][disabled]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* behind = root->add_child(
        std::make_unique<ProbeWidget>(Rect{50, 50, 100, 100}));
    behind->claim_generic = true; // claims right-click / drag-hover / file-drop
    ProbeWidget* front = root->add_child(
        std::make_unique<ProbeWidget>(Rect{50, 50, 100, 100}));
    front->set_enabled(false);

    bool dirty = false;
    Widget* moved = root->dispatch_pointer_move({60, 60}, &dirty);
    CHECK(moved == front);
    CHECK(behind->move_count == 0);

    CHECK(root->dispatch_right_click({60, 60}) == front);
    CHECK(behind->right_count == 0);

    CHECK(root->dispatch_drag_hover({60, 60}) == front);
    CHECK(behind->drag_hover_count == 0);

    FileDropPayload payload;
    CHECK(root->dispatch_file_drop({60, 60}, payload) == front);
    CHECK(behind->file_drop_count == 0);
}

TEST_CASE("hit_test stops at a disabled widget", "[tk][widget][disabled]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* panel =
        root->add_child(std::make_unique<ProbeWidget>(Rect{0, 0, 200, 200}));
    ProbeWidget* child =
        panel->add_child(std::make_unique<ProbeWidget>(Rect{20, 20, 60, 60}));

    CHECK(root->hit_test({30, 30}) == child); // enabled: descends to the leaf
    panel->set_enabled(false);
    CHECK(root->hit_test({30, 30}) == panel); // disabled: stops here
}

TEST_CASE("Clicking a disabled widget does not clear the focused widget",
          "[tk][host][disabled][focus]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* field = root->add_child(
        std::make_unique<ProbeWidget>(Rect{10, 10, 100, 30}));
    field->focusable_ = true;
    ProbeWidget* dead = root->add_child(
        std::make_unique<ProbeWidget>(Rect{10, 200, 100, 30}));
    dead->set_enabled(false);
    TestHost host(root.get());

    host.request_focus(field);
    REQUIRE(host.focused_widget() == field);

    host.dispatch_pointer_down({50, 210}); // press on the disabled widget
    CHECK(host.focused_widget() == field); // focus preserved, not cleared

    host.dispatch_pointer_down({300, 300}); // press on genuinely empty space
    CHECK(host.focused_widget() == nullptr); // that still clears focus
}

TEST_CASE("Tab traversal skips a disabled subtree", "[tk][widget][disabled]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* a = root->add_child(
        std::make_unique<ProbeWidget>(Rect{0, 0, 100, 20}));
    a->focusable_ = true;
    ProbeWidget* b = root->add_child(
        std::make_unique<ProbeWidget>(Rect{0, 40, 100, 20}));
    b->focusable_ = true;

    CHECK(next_focusable(root.get(), a, /*forward=*/true) == b);
    b->set_enabled(false);
    CHECK(next_focusable(root.get(), a, /*forward=*/true) == a); // b skipped
}
