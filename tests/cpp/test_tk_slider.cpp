#include <catch2/catch_test_macros.hpp>

#include "tk/slider.h"
#include "access_test_util.h"
#include "view_test_util.h"

#include <cmath>
#include <vector>

using namespace tk;

namespace
{

struct SliderStage : vt::Stage
{
    std::unique_ptr<Slider> slider = create_root_widget<Slider>(&host, 0.0, 100.0, 1.0);
    std::vector<double> changes;
    std::vector<double> commits;

    SliderStage()
    {
        slider->on_changed = [this](double v)
        {
            changes.push_back(v);
        };
        slider->on_committed = [this](double v)
        {
            commits.push_back(v);
        };
        mount(*slider, {0, 0, 200, 24});
    }
    static Point mid()
    {
        return {100.0f, 12.0f};
    }
};

} // namespace

TEST_CASE("Slider set_value clamps to range and limit and is silent", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_limit(40.0);
    s.slider->set_value(90.0);
    CHECK(s.slider->value() == 40.0);
    s.slider->set_value(-5.0);
    CHECK(s.slider->value() == 0.0);
    CHECK(s.changes.empty());
    CHECK(s.commits.empty());
}

TEST_CASE("Slider pointer press jumps and drag tracks with capture", "[tk][slider]")
{
    SliderStage s;
    s.host.dispatch_pointer_down(s.mid());
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes.back() == 50.0);
    CHECK(s.slider->dragging());

    s.slider->on_pointer_drag({-50.0f, 12.0f});
    CHECK(s.changes.back() == 0.0);
    s.slider->set_limit(80.0);
    s.slider->on_pointer_drag({999.0f, 12.0f});
    CHECK(s.changes.back() == 80.0);

    CHECK(s.commits.empty());
    s.host.dispatch_pointer_up({999.0f, 12.0f});
    REQUIRE(s.commits.size() == 1);
    CHECK(s.commits.back() == 80.0);
    CHECK_FALSE(s.slider->dragging());
}

TEST_CASE("Slider keys step, page, and jump to ends when focused", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_value(50.0);
    s.slider->set_limit(90.0);
    CHECK_FALSE(s.slider->on_key_down(vt::key(Key::Right))); // unfocused

    s.host.request_focus(s.slider.get());
    REQUIRE(s.slider->has_focus());
    CHECK(s.slider->on_key_down(vt::key(Key::Right)));
    CHECK(s.slider->value() == 51.0);
    CHECK(s.slider->on_key_down(vt::key(Key::Left)));
    CHECK(s.slider->value() == 50.0);
    CHECK(s.slider->on_key_down(vt::key(Key::PageUp)));
    CHECK(s.slider->value() == 60.0);
    CHECK(s.slider->on_key_down(vt::key(Key::End)));
    CHECK(s.slider->value() == 90.0);
    CHECK(s.slider->on_key_down(vt::key(Key::Home)));
    CHECK(s.slider->value() == 0.0);
    CHECK(s.changes.size() == 5);
    CHECK(s.commits.size() == 5);
}

TEST_CASE("Slider without keyboard focus is not a tab stop but still takes clicks", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_keyboard_focusable(false);
    CHECK_FALSE(s.slider->focusable());
    s.host.dispatch_pointer_down(s.mid());
    s.host.dispatch_pointer_up(s.mid());
    CHECK(s.slider->value() == 50.0);
}

TEST_CASE("Slider survives degenerate ranges", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_range(5.0, 5.0);
    s.slider->set_limit(1.0);
    s.slider->set_buffered(3.0);
    s.relayout_paint(*s.slider, {0, 0, 200, 24});
    CHECK(std::isfinite(s.slider->value()));
    s.host.dispatch_pointer_down(s.mid());
    s.host.dispatch_pointer_up(s.mid());
    CHECK(std::isfinite(s.slider->value()));

    s.relayout_paint(*s.slider, {0, 0, 0, 0}); // zero width
    s.slider->on_pointer_drag({10.0f, 1.0f});
    CHECK(std::isfinite(s.slider->value()));
}

TEST_CASE("Slider exposes slider a11y role, value, and settable value", "[tk][slider][a11y]")
{
    SliderStage s;
    s.slider->set_accessible_name("Width");
    s.slider->set_value(30.0);
    CHECK(s.slider->access_role() == Role::Slider);
    CHECK(s.slider->access_name() == "Width");
    const AccessValue v = s.slider->access_value();
    CHECK(v.present);
    CHECK(v.min == 0.0);
    CHECK(v.max == 100.0);
    CHECK(v.now == 30.0);

    CHECK(s.slider->access_set_value(75.0));
    CHECK(s.slider->value() == 75.0);
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes.back() == 75.0);
    REQUIRE(s.commits.size() == 1);
}

TEST_CASE("Disabled Slider ignores input", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_enabled(false);
    s.host.dispatch_pointer_down(s.mid());
    s.host.dispatch_pointer_up(s.mid());
    CHECK(s.slider->value() == 0.0);
    CHECK(s.changes.empty());
    CHECK_FALSE(s.slider->access_set_value(10.0));
}

TEST_CASE("Slider node in the access tree carries its value and accepts a set",
          "[tk][slider][a11y]")
{
    SliderStage s;
    s.slider->set_accessible_name("Timeline width");
    s.slider->set_value(30.0);
    const AccessNode root = build_access_tree(s.slider.get());
    const AccessNode* node = access_test::find_named(root, "Timeline width");
    REQUIRE(node != nullptr);
    CHECK(node->role == Role::Slider);
    CHECK(node->value.present);
    CHECK(node->value.now == 30.0);

    CHECK(set_node_value(*node, 60.0));
    CHECK(s.slider->value() == 60.0);

    // A node with no settable widget (a plain label) refuses.
    AccessNode bare;
    CHECK_FALSE(set_node_value(bare, 1.0));
}

TEST_CASE("Slider drops a drag when it is hidden or disabled mid-drag", "[tk][slider]")
{
    SliderStage s;
    s.host.dispatch_pointer_down(s.mid());
    REQUIRE(s.slider->dragging());
    s.slider->set_visible(false);
    CHECK_FALSE(s.slider->dragging());

    s.slider->set_visible(true);
    s.host.dispatch_pointer_down(s.mid());
    REQUIRE(s.slider->dragging());
    s.slider->set_enabled(false);
    CHECK_FALSE(s.slider->dragging());
}

TEST_CASE("Slider set_range widens an unset limit to the new maximum", "[tk][slider]")
{
    SliderStage s;
    s.slider->set_range(0.0, 1000.0);
    s.slider->set_value(900.0);
    CHECK(s.slider->value() == 900.0);

    s.slider->set_limit(500.0);
    s.slider->set_range(0.0, 2000.0); // an explicit limit is kept
    s.slider->set_value(900.0);
    CHECK(s.slider->value() == 500.0);
}
