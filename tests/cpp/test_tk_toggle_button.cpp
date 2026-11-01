#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <memory>
#include <vector>

using namespace tk;

namespace
{

struct ToggleStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(100, 100);
    std::unique_ptr<ToggleButton> btn = create_widget<ToggleButton>(nullptr, std::string{});

    ToggleStage()
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        btn->measure(lc, {100, 100});
        btn->arrange(lc, {10, 10, 32, 32});
    }

    void click()
    {
        const Point p{26, 26};
        Widget* claimer = btn->dispatch_pointer_down(p);
        REQUIRE(claimer != nullptr);
        const Rect cb = claimer->bounds();
        claimer->on_pointer_up({p.x - cb.x, p.y - cb.y}, /*inside_self=*/true);
    }
};

} // namespace

TEST_CASE("ToggleButton click flips state and reports the new value", "[toggle_button]")
{
    ToggleStage st;
    std::vector<bool> seen;
    st.btn->set_on_toggled(
        [&](bool on)
        {
            seen.push_back(on);
        });

    CHECK_FALSE(st.btn->checked());
    st.click();
    CHECK(st.btn->checked());
    st.click();
    CHECK_FALSE(st.btn->checked());

    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == true);
    CHECK(seen[1] == false);
}

TEST_CASE("ToggleButton::set_checked is silent", "[toggle_button]")
{
    ToggleStage st;
    int calls = 0;
    st.btn->set_on_toggled(
        [&](bool)
        {
            ++calls;
        });

    st.btn->set_checked(true);
    CHECK(st.btn->checked());
    st.btn->set_checked(false);
    CHECK_FALSE(st.btn->checked());
    CHECK(calls == 0);
}

TEST_CASE("ToggleButton reports its checked state to accessibility", "[toggle_button]")
{
    ToggleStage st;
    CHECK_FALSE(st.btn->access_state().checked);
    st.btn->set_checked(true);
    CHECK(st.btn->access_state().checked);
    CHECK(st.btn->access_role() == Role::Button);
}
