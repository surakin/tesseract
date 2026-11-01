#include <catch2/catch_test_macros.hpp>

#include "tk/radio.h"
#include "view_test_util.h"

#include <string>
#include <vector>

using namespace tk;

namespace
{

struct RadioStage : vt::Stage
{
    std::unique_ptr<RadioGroup> group =
        create_root_widget<RadioGroup>(&host, FlexBox::Axis::Vertical);
    std::vector<std::string> changes;

    RadioStage()
    {
        group->set_options({{"One", "1"}, {"Two", "2"}, {"Three", "3"}});
        group->on_changed = [this](std::string v)
        {
            changes.push_back(v);
        };
        mount(*group, {0, 0, 300, 300});
    }
    std::vector<RadioButton*> buttons()
    {
        std::vector<RadioButton*> out;
        vt::collect<RadioButton>(*group, out);
        return out;
    }
};

} // namespace

TEST_CASE("RadioGroup set_selected_value is silent and checks exactly one", "[tk][radio]")
{
    RadioStage s;
    s.group->set_selected_value("2");
    auto b = s.buttons();
    REQUIRE(b.size() == 3);
    CHECK_FALSE(b[0]->checked());
    CHECK(b[1]->checked());
    CHECK_FALSE(b[2]->checked());
    CHECK(s.group->selected_value() == "2");
    CHECK(s.changes.empty());
}

TEST_CASE("RadioGroup click selects and fires only on change", "[tk][radio]")
{
    RadioStage s;
    s.group->set_selected_value("1");
    s.relayout_paint(*s.group, {0, 0, 300, 300});
    auto b = s.buttons();
    const Rect r = b[2]->bounds();
    const Point p{r.x + 5, r.y + r.h * 0.5f};
    s.host.dispatch_pointer_down(p);
    s.host.dispatch_pointer_up(p);
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0] == "3");
    CHECK(b[2]->checked());
    CHECK_FALSE(b[0]->checked());
    // Clicking the already-selected button does not fire again.
    s.host.dispatch_pointer_down(p);
    s.host.dispatch_pointer_up(p);
    CHECK(s.changes.size() == 1);
}

TEST_CASE("RadioGroup is a single Tab stop; arrows move and wrap", "[tk][radio]")
{
    RadioStage s;
    s.group->set_selected_value("1");
    auto b = s.buttons();
    CHECK(b[0]->focusable());
    CHECK_FALSE(b[1]->focusable());
    CHECK_FALSE(b[2]->focusable());

    s.host.request_focus(b[0]);
    REQUIRE(b[0]->has_focus());
    CHECK(b[0]->on_key_down(vt::key(Key::Left))); // wraps to last
    CHECK(s.group->selected_value() == "3");
    CHECK(b[2]->has_focus());
    CHECK(b[2]->on_key_down(vt::key(Key::Right))); // wraps to first
    CHECK(s.group->selected_value() == "1");
    CHECK(b[0]->on_key_down(vt::key(Key::Down)));
    CHECK(s.group->selected_value() == "2");
    CHECK(s.changes == std::vector<std::string>{"3", "1", "2"});
}

TEST_CASE("RadioButton exposes radio a11y role and checked state", "[tk][radio][a11y]")
{
    RadioStage s;
    s.group->set_selected_value("2");
    auto b = s.buttons();
    CHECK(s.group->access_role() == Role::Group);
    CHECK(b[1]->access_role() == Role::RadioButton);
    CHECK(b[1]->access_name() == "Two");
    CHECK(b[1]->access_state().checked);
    CHECK_FALSE(b[0]->access_state().checked);
    CHECK(b[0]->access_default_action());
    CHECK(s.group->selected_value() == "1");
}

TEST_CASE("Disabled RadioButton ignores input", "[tk][radio]")
{
    RadioStage s;
    s.group->set_selected_value("1");
    auto b = s.buttons();
    b[1]->set_enabled(false);
    CHECK_FALSE(b[1]->access_default_action());
    // Arrow navigation skips the disabled button.
    s.host.request_focus(b[0]);
    b[0]->on_key_down(vt::key(Key::Right));
    CHECK(s.group->selected_value() == "3");
}

TEST_CASE("Horizontal RadioGroup buttons size to their full label", "[tk][radio]")
{
    vt::Stage st;
    auto g = create_root_widget<RadioGroup>(&st.host, FlexBox::Axis::Horizontal);
    g->set_options({{"Light", "l"}, {"Dark", "d"}, {"System preference", "s"}});
    st.mount(*g, {0, 0, 600, 60});
    std::vector<RadioButton*> bs;
    vt::collect<RadioButton>(*g, bs);
    REQUIRE(bs.size() == 3);
    // The longest label gets a wider button than the shortest, with no clipping.
    CHECK(bs[2]->bounds().w > bs[0]->bounds().w);
    auto l = st.lc();
    CHECK(bs[2]->measure(l, {40, 40}).w == bs[2]->bounds().w);
}
