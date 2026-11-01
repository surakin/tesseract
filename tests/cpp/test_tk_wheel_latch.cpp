#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/scroll_view.h"
#include "tk/widget.h"
#include "tk_test_host.h"

#include <chrono>
#include <memory>

// Wheel latching: once a widget consumes a wheel event, the rest of that
// gesture stays with it even when content slides a different wheel consumer
// (a combobox, an inner list) under the stationary pointer.

using namespace tk;
using namespace std::chrono_literals;

namespace
{

// Counts wheel events; optionally pretends to coast (momentum in flight).
class WheelProbe : public Widget
{
public:
    explicit WheelProbe(Rect r, bool consume = true) : consume_(consume)
    {
        bounds_ = r;
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override
    {
    }
    bool on_wheel(Point, float, float, bool) override
    {
        ++wheels;
        return consume_;
    }
    bool on_pointer_down(Point) override
    {
        ++downs;
        return true;
    }
    void on_pointer_up(Point, bool) override
    {
        ++ups;
    }
    bool wheel_motion_active() const override
    {
        return coasting;
    }
    void stop_wheel_motion() override
    {
        coasting = false;
        ++stops;
    }
    void move_to(Rect r)
    {
        bounds_ = r;
    }
    void set_consume(bool c)
    {
        consume_ = c;
    }

    int wheels = 0;
    int downs = 0;
    int ups = 0;
    int stops = 0;
    bool coasting = false;

private:
    bool consume_;
};

class LatchHost : public TestHost
{
public:
    using TestHost::TestHost;
    using tk::Host::dispatch_wheel;
    std::chrono::steady_clock::time_point fake_now = std::chrono::steady_clock::time_point{} + 1h;
    void advance(std::chrono::milliseconds d)
    {
        fake_now += d;
    }

protected:
    std::chrono::steady_clock::time_point wheel_clock_now_() const override
    {
        return fake_now;
    }
};

// root ─ scroller (whole area, consumes) ─ target (consumes, "the combobox")
struct Stage
{
    LatchHost host{nullptr};
    std::unique_ptr<WheelProbe> root_owned;
    WheelProbe* root = nullptr;
    WheelProbe* scroller = nullptr;
    WheelProbe* target = nullptr;

    Stage()
    {
        root_owned = create_root_widget<WheelProbe>(&host, Rect{0, 0, 400, 400}, false);
        root = root_owned.get();
        host.set_root(root);
        scroller = root->add_child(create_widget<WheelProbe>(root, Rect{0, 0, 400, 400}));
        // Target starts well away from the pointer.
        target = scroller->add_child(create_widget<WheelProbe>(scroller, Rect{0, 300, 400, 40}));
    }
    // Content "scrolls": the target slides under the pointer at y=100.
    void slide_target_under_pointer()
    {
        target->move_to({0, 90, 400, 40});
    }
};

constexpr Point kPointer{50, 100};

} // namespace

TEST_CASE("A scrolling gesture keeps the wheel when a consumer slides under the pointer",
          "[tk][host][wheel-latch]")
{
    Stage s;
    REQUIRE(s.host.dispatch_wheel(kPointer, 0, 10));
    CHECK(s.scroller->wheels == 1);

    s.slide_target_under_pointer();
    s.host.advance(50ms);
    CHECK(s.host.dispatch_wheel(kPointer, 0, 10));
    CHECK(s.scroller->wheels == 2);
    CHECK(s.target->wheels == 0);
}

TEST_CASE("The wheel returns to the widget under the pointer after a quiet gap",
          "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.slide_target_under_pointer();

    s.host.advance(500ms);
    CHECK(s.host.dispatch_wheel(kPointer, 0, 10));
    CHECK(s.target->wheels == 1);
}

TEST_CASE("Moving the pointer ends the latch, a tiny jitter does not", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.slide_target_under_pointer();

    s.host.dispatch_pointer_move({52, 101}); // jitter
    s.host.advance(20ms);
    s.host.dispatch_wheel({52, 101}, 0, 10);
    CHECK(s.target->wheels == 0);

    s.host.dispatch_pointer_move({70, 110}); // deliberate move
    s.host.advance(20ms);
    s.host.dispatch_wheel({70, 110}, 0, 10);
    CHECK(s.target->wheels == 1);
}

TEST_CASE("A latched widget keeps the wheel even if it declines it", "[tk][host][wheel-latch]")
{
    // A scroller pinned at its bound returns false from on_wheel; the
    // gesture must not leak to the widget now under the pointer.
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.slide_target_under_pointer();
    s.scroller->set_consume(false); // hit the bound
    s.host.advance(10ms);
    CHECK(s.host.dispatch_wheel(kPointer, 0, 10));
    CHECK(s.scroller->wheels == 2);
    CHECK(s.target->wheels == 0);
}

TEST_CASE("Momentum in flight keeps the latch past the quiet gap", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10, true);
    s.scroller->coasting = true;
    s.slide_target_under_pointer();

    s.host.advance(900ms);
    s.host.dispatch_wheel(kPointer, 0, 10, true);
    CHECK(s.scroller->wheels == 2);
    CHECK(s.target->wheels == 0);

    s.scroller->coasting = false;
    s.host.advance(900ms);
    s.host.dispatch_wheel(kPointer, 0, 10, true);
    CHECK(s.target->wheels == 1);
}

TEST_CASE("A press during a coast only stops it and is swallowed", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10, true);
    s.scroller->coasting = true;
    s.slide_target_under_pointer();

    s.host.dispatch_pointer_down(kPointer);
    s.host.dispatch_pointer_up(kPointer);
    CHECK(s.scroller->stops == 1);
    CHECK(s.target->downs == 0);
    CHECK(s.target->ups == 0);
    CHECK(s.scroller->downs == 0);

    // The next click behaves normally.
    s.host.dispatch_pointer_down(kPointer);
    s.host.dispatch_pointer_up(kPointer);
    CHECK(s.target->downs == 1);
    CHECK(s.target->ups == 1);
}

TEST_CASE("A press with nothing coasting is not swallowed", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.slide_target_under_pointer();

    s.host.dispatch_pointer_down(kPointer);
    s.host.dispatch_pointer_up(kPointer);
    CHECK(s.target->downs == 1);
}

TEST_CASE("A hidden latched widget releases the wheel", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.slide_target_under_pointer();
    s.scroller->set_visible(false);

    s.host.advance(10ms);
    s.host.dispatch_wheel(kPointer, 0, 10); // must not crash or hit the hidden one
    CHECK(s.scroller->wheels == 1);
}

TEST_CASE("A destroyed latched widget releases the wheel", "[tk][host][wheel-latch]")
{
    Stage s;
    s.host.dispatch_wheel(kPointer, 0, 10);
    s.root->clear_children();
    s.host.fire_all_ui_tasks();

    s.host.advance(10ms);
    CHECK_FALSE(s.host.dispatch_wheel(kPointer, 0, 10));
}

TEST_CASE("ScrollView reports momentum after touchpad samples and can stop it",
          "[tk][scroll-view][wheel-latch]")
{
    TestHost host(nullptr);
    auto sv = create_root_widget<ScrollView>(&host);
    CHECK_FALSE(sv->wheel_motion_active());
    // Touchpad sample arms the idle-gap watch; a mouse notch does not.
    sv->set_child(create_widget<WheelProbe>(sv.get(), Rect{0, 0, 100, 1000}));
    sv->on_wheel({1, 1}, 0, 5, true);
    CHECK(sv->wheel_motion_active());
    sv->stop_wheel_motion();
    CHECK_FALSE(sv->wheel_motion_active());
}
