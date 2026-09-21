#include <catch2/catch_test_macros.hpp>

#include "tk/drag_drop.h"
#include "tk/drag_gesture.h"
#include "tk/host.h"
#include "tk/widget.h"
#include "tk_test_host.h"

#include <memory>
#include <string>

// Exercises the in-app (widget-to-widget) drag-and-drop framework: DragPayload
// typing, DragGestureTracker's click-vs-drag threshold, Widget::dispatch_drag_enter's
// claim-bubble walk, and Host::begin_drag/dispatch_pointer_move/_up/cancel_drag's
// state machine. Mirrors test_tk_host_file_drop.cpp's coverage shape for the
// unrelated OS-file-drop system (native_drag_hover/leave).

using namespace tk;

namespace
{

class DragProbeWidget : public Widget
{
public:
    DragProbeWidget(Rect rect, bool accept = false) : accept_(accept)
    {
        bounds_ = rect; // world bounds drive contains_world / the tree walk
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}

    bool on_pointer_down(Point) override
    {
        return claim_press;
    }

    bool on_drag_enter(Point local, const DragPayload&) override
    {
        ++enter_count;
        last_enter_local = local;
        return accept_;
    }
    void on_drag_over(Point local, const DragPayload&) override
    {
        ++over_count;
        last_over_local = local;
    }
    void on_drag_leave_target() override
    {
        ++leave_count;
    }
    bool on_drop(Point local, DragPayload payload) override
    {
        ++drop_count;
        last_local = local;
        last_payload_kind = payload.kind();
        return accept_;
    }

    bool claim_press = false;
    int enter_count = 0;
    int over_count = 0;
    int leave_count = 0;
    int drop_count = 0;
    Point last_enter_local{};
    Point last_over_local{};
    Point last_local{};
    std::string last_payload_kind;

private:
    bool accept_;
};

DragPayload make_payload()
{
    return DragPayload("room_id", std::string("!room:example.org"));
}

} // namespace

TEST_CASE("DragPayload carries a kind-tagged, strongly-typed value",
          "[tk][drag_drop]")
{
    DragPayload payload = make_payload();

    CHECK(payload.kind() == "room_id");
    REQUIRE(payload.get_if<std::string>("room_id") != nullptr);
    CHECK(*payload.get_if<std::string>("room_id") == "!room:example.org");

    // Wrong kind tag: nullptr even though the stored type matches.
    CHECK(payload.get_if<std::string>("other_kind") == nullptr);
    // Right kind, wrong requested type: nullptr, not a crash.
    CHECK(payload.get_if<int>("room_id") == nullptr);
}

TEST_CASE("DragGestureTracker fires exactly once when the threshold is "
          "crossed and can be rearmed",
          "[tk][drag_drop]")
{
    DragGestureTracker tracker;
    tracker.begin({0, 0});

    CHECK_FALSE(tracker.crossed_threshold({1, 1})); // below kDragThresholdPx
    CHECK(tracker.crossed_threshold({5, 0}));        // crosses it
    CHECK_FALSE(tracker.crossed_threshold({20, 0})); // already fired, disarmed

    tracker.begin({0, 0});
    tracker.cancel();
    CHECK_FALSE(tracker.crossed_threshold({100, 100})); // cancelled before firing
}

TEST_CASE("dispatch_drag_enter claims the topmost accepting descendant, "
          "reverse z-order, first claimer wins",
          "[tk][drag_drop]")
{
    DragProbeWidget root({0, 0, 400, 400});
    auto* a = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{0, 0, 100, 100}, /*accept=*/true));
    auto* b = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{200, 0, 100, 100}, /*accept=*/true));
    DragPayload payload = make_payload();

    Widget* target = root.dispatch_drag_enter({50, 50}, payload);
    REQUIRE(target == a);
    CHECK(a->enter_count == 1);
    CHECK(b->enter_count == 0);

    target = root.dispatch_drag_enter({250, 50}, payload);
    REQUIRE(target == b);
}

TEST_CASE("dispatch_drag_enter skips an invisible subtree entirely",
          "[tk][drag_drop]")
{
    DragProbeWidget root({0, 0, 400, 400});
    auto* hidden =
        root.add_child(std::make_unique<DragProbeWidget>(Rect{0, 0, 200, 200}));
    auto* grandchild = hidden->add_child(
        std::make_unique<DragProbeWidget>(Rect{50, 50, 50, 50}, /*accept=*/true));
    hidden->set_visible(false);
    DragPayload payload = make_payload();

    Widget* target = root.dispatch_drag_enter({60, 60}, payload);

    REQUIRE(target == nullptr);
    CHECK(grandchild->enter_count == 0);
    CHECK(hidden->enter_count == 0);
}

TEST_CASE("Host::begin_drag + dispatch_pointer_move retargets across "
          "widgets, firing on_drag_leave_target on change, and "
          "dispatch_pointer_up delivers on_drop to the current claimant",
          "[tk][drag_drop][host]")
{
    DragProbeWidget root({0, 0, 400, 400});
    auto* source =
        root.add_child(std::make_unique<DragProbeWidget>(Rect{0, 0, 50, 50}));
    source->claim_press = true;
    auto* a = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{100, 100, 100, 100}, /*accept=*/true));
    auto* b = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{300, 100, 100, 100}, /*accept=*/true));
    TestHost host(&root);

    host.dispatch_pointer_down({10, 10});
    REQUIRE(host.pressed_widget_.lock().get() == source);

    host.begin_drag(make_payload(), DragVisual{}, {10, 10});
    REQUIRE(host.is_dragging());

    host.dispatch_pointer_move({150, 150}); // inside a
    CHECK(a->enter_count == 1);
    CHECK(a->over_count == 1);
    CHECK(host.active_drag_->current_target.lock().get() == a);

    host.dispatch_pointer_move({350, 150}); // inside b now
    CHECK(a->leave_count == 1);             // a lost the claim
    CHECK(b->enter_count == 1);
    CHECK(b->over_count == 1);

    host.dispatch_pointer_move({350, 160}); // still inside b — no reclaim
    CHECK(b->enter_count == 1);
    CHECK(b->over_count == 2);
    CHECK(b->leave_count == 0);

    host.dispatch_pointer_up({350, 160});
    CHECK(b->drop_count == 1);
    CHECK(b->last_payload_kind == "room_id");
    CHECK_FALSE(host.is_dragging());
    CHECK(host.pressed_widget_.expired());
}

TEST_CASE("Dropping where nothing accepts ends the drag without firing "
          "on_drop",
          "[tk][drag_drop][host]")
{
    DragProbeWidget root({0, 0, 400, 400}); // accept_ = false: root itself refuses
    root.claim_press = true;
    TestHost host(&root);

    host.dispatch_pointer_down({10, 10});
    host.begin_drag(make_payload(), DragVisual{}, {10, 10});
    host.dispatch_pointer_move({200, 200});
    CHECK(host.active_drag_->current_target.expired());

    host.dispatch_pointer_up({200, 200});
    CHECK(root.drop_count == 0);
    CHECK_FALSE(host.is_dragging());
}

TEST_CASE("cancel_drag fires on_drag_leave_target, never on_drop, and is "
          "idempotent",
          "[tk][drag_drop][host]")
{
    DragProbeWidget root({0, 0, 400, 400});
    root.claim_press = true;
    auto* a = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{100, 100, 100, 100}, /*accept=*/true));
    TestHost host(&root);

    host.dispatch_pointer_down({10, 10});
    host.begin_drag(make_payload(), DragVisual{}, {10, 10});
    host.dispatch_pointer_move({150, 150});
    REQUIRE(a->enter_count == 1);

    host.cancel_drag();
    CHECK(a->leave_count == 1);
    CHECK(a->drop_count == 0);
    CHECK_FALSE(host.is_dragging());
    CHECK(host.pressed_widget_.expired());

    host.cancel_drag(); // no-op, no double leave
    CHECK(a->leave_count == 1);
}

TEST_CASE("Escape cancels an active drag ahead of other key handling",
          "[tk][drag_drop][host]")
{
    DragProbeWidget root({0, 0, 400, 400});
    root.claim_press = true;
    auto* a = root.add_child(
        std::make_unique<DragProbeWidget>(Rect{100, 100, 100, 100}, /*accept=*/true));
    TestHost host(&root);

    host.dispatch_pointer_down({10, 10});
    host.begin_drag(make_payload(), DragVisual{}, {10, 10});
    host.dispatch_pointer_move({150, 150});
    REQUIRE(a->enter_count == 1);

    KeyEvent esc;
    esc.key = Key::Escape;
    CHECK(host.dispatch_key_down(esc));

    CHECK(a->leave_count == 1);
    CHECK_FALSE(host.is_dragging());
}
