#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/widget.h"
#include "tk_test_host.h"

#include <memory>

// Exercises Host's tooltip show/hide/update-text API: the dwell-delay timer
// (generation-counter guarded, since post_delayed has no cancellation),
// popup suppression, and pointer-down dismissal. Mirrors the style of
// test_tk_host_pointer.cpp, using the same shared TestHost fake.

using namespace tk;

namespace
{

class TooltipProbeWidget : public Widget
{
public:
    explicit TooltipProbeWidget(Rect rect) { bounds_ = rect; }

    Size measure(LayoutCtx&, Size) override { return {bounds_.w, bounds_.h}; }
    void paint(PaintCtx&) override {}
};

} // namespace

TEST_CASE("show_tooltip does not become visible until the dwell delay fires",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner = 0;

    host.show_tooltip(&owner, "hello", {10, 10, 20, 20});

    CHECK(host.tooltip_owner_ == &owner);
    CHECK_FALSE(host.tooltip_visible_); // still in its dwell delay
    REQUIRE(host.pending_delays_.size() == 1);
    CHECK(host.pending_delays_[0].ms == TestHost::kTooltipShowDelayMs);

    host.fire_all_delays();

    CHECK(host.tooltip_visible_);
    CHECK(host.tooltip_text_ == "hello");
}

TEST_CASE("a second show_tooltip call from the same owner refreshes content "
          "without re-arming the delay",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner = 0;

    host.show_tooltip(&owner, "first", {0, 0, 10, 10});
    REQUIRE(host.pending_delays_.size() == 1);

    // Same owner, before the delay fires: refresh in place, no second timer.
    host.show_tooltip(&owner, "second", {5, 5, 10, 10});
    CHECK(host.pending_delays_.size() == 1);
    CHECK_FALSE(host.tooltip_visible_);

    host.fire_all_delays();
    CHECK(host.tooltip_visible_);
    CHECK(host.tooltip_text_ == "second");

    // Same owner, already visible: refresh takes effect immediately.
    host.show_tooltip(&owner, "third", {5, 5, 10, 10});
    CHECK(host.tooltip_visible_);
    CHECK(host.tooltip_text_ == "third");
}

TEST_CASE("a new owner mid-dwell invalidates the stale timer", "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner_a = 0;
    int owner_b = 0;

    host.show_tooltip(&owner_a, "a", {0, 0, 10, 10});
    REQUIRE(host.pending_delays_.size() == 1);
    auto stale_a_delay = host.pending_delays_[0].fn;

    // A different owner takes over before A's delay elapses.
    host.show_tooltip(&owner_b, "b", {0, 0, 10, 10});
    CHECK(host.tooltip_owner_ == &owner_b);

    // Firing the stale timer for A must not make the tooltip visible for A.
    stale_a_delay();
    CHECK(host.tooltip_owner_ == &owner_b);
    CHECK_FALSE(host.tooltip_visible_); // B's own delay hasn't fired yet
    CHECK(host.tooltip_text_ == "b");
}

TEST_CASE("hide_tooltip before the delay elapses cancels the pending show",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner = 0;

    host.show_tooltip(&owner, "hello", {0, 0, 10, 10});
    host.hide_tooltip(&owner);

    host.fire_all_delays();

    CHECK_FALSE(host.tooltip_visible_);
    CHECK(host.tooltip_owner_ == nullptr);
}

TEST_CASE("hide_tooltip from a non-owning caller is a no-op", "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner_a = 0;
    int owner_b = 0;

    host.show_tooltip(&owner_a, "hello", {0, 0, 10, 10});
    host.fire_all_delays();
    REQUIRE(host.tooltip_visible_);

    host.hide_tooltip(&owner_b); // not the owner — must not disturb A's tooltip

    CHECK(host.tooltip_visible_);
    CHECK(host.tooltip_owner_ == &owner_a);
    CHECK(host.tooltip_text_ == "hello");
}

TEST_CASE("an open popup suppresses show_tooltip entirely", "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TooltipProbeWidget popup({100, 100, 50, 50});
    TestHost host(&root);
    host.set_active_popup(&popup);

    int owner = 0;
    host.show_tooltip(&owner, "hello", {0, 0, 10, 10});

    CHECK(host.tooltip_owner_ == nullptr);
    CHECK(host.pending_delays_.empty());
}

TEST_CASE("dispatch_pointer_down cancels an active or pending tooltip",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner = 0;

    SECTION("while pending (still in dwell delay)")
    {
        host.show_tooltip(&owner, "hello", {0, 0, 10, 10});
        REQUIRE(host.tooltip_owner_ == &owner);

        host.dispatch_pointer_down({200, 200});

        host.fire_all_delays();
        CHECK(host.tooltip_owner_ == nullptr);
        CHECK_FALSE(host.tooltip_visible_);
    }

    SECTION("while visible")
    {
        host.show_tooltip(&owner, "hello", {0, 0, 10, 10});
        host.fire_all_delays();
        REQUIRE(host.tooltip_visible_);

        host.dispatch_pointer_down({200, 200});

        CHECK_FALSE(host.tooltip_visible_);
        CHECK(host.tooltip_owner_ == nullptr);
    }
}

TEST_CASE("update_tooltip_text adopts ownership only when nobody else owns "
          "the tooltip, and shows immediately without a delay",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    int owner_a = 0;
    int owner_b = 0;

    // Nobody owns the tooltip yet — adopt and show immediately.
    host.update_tooltip_text(&owner_a, "adopted");
    CHECK(host.tooltip_owner_ == &owner_a);
    CHECK(host.tooltip_visible_);
    CHECK(host.tooltip_text_ == "adopted");
    CHECK(host.pending_delays_.empty()); // no dwell delay for an update

    // A different owner cannot steal it.
    host.update_tooltip_text(&owner_b, "stolen?");
    CHECK(host.tooltip_owner_ == &owner_a);
    CHECK(host.tooltip_text_ == "adopted");

    // The actual owner can update its own text in place.
    host.update_tooltip_text(&owner_a, "updated");
    CHECK(host.tooltip_text_ == "updated");
}

TEST_CASE("update_tooltip_text is a no-op while a popup is open",
          "[tk][host][tooltip]")
{
    TooltipProbeWidget root({0, 0, 400, 400});
    TooltipProbeWidget popup({100, 100, 50, 50});
    TestHost host(&root);
    host.set_active_popup(&popup);

    int owner = 0;
    host.update_tooltip_text(&owner, "hello");

    CHECK(host.tooltip_owner_ == nullptr);
}
