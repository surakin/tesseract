#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

// Characterization of tk::Button's hover / press / click behaviour through the
// shared Host pointer state machine. These pin down today's behaviour so the
// hover feature can move from Button into Widget (and the host's
// hit_test + dynamic_cast<Button*> track can be generalised) without any
// observable change for buttons.

using namespace tk;

namespace
{

class HoverBox : public Widget
{
public:
    explicit HoverBox(Rect r)
    {
        bounds_ = r;
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void arrange(LayoutCtx&, Rect) override
    {
    }
    void paint(PaintCtx&) override
    {
    }
};

// Mirrors RoomHeader / ComposeBar: claims pointer-move hover for itself even
// when a child (the button) is the deepest widget under the cursor.
class HoverClaimingBox : public HoverBox
{
public:
    using HoverBox::HoverBox;
    Widget* dispatch_pointer_move(Point world, bool* dirty) override
    {
        if (!visible_ || !contains_world(world))
        {
            return nullptr;
        }
        Widget::dispatch_pointer_move(world, dirty);
        return this;
    }
};

// Mirrors MainAppWidget's overlay stacks: transparent unless a child hits.
class HoverTransparentStack : public HoverBox
{
public:
    using HoverBox::HoverBox;
    Widget* dispatch_pointer_move(Point world, bool* dirty) override
    {
        if (!visible_)
        {
            return nullptr;
        }
        for (auto& ch : children())
        {
            if (ch->visible())
            {
                if (Widget* hit = ch->dispatch_pointer_move(world, dirty))
                {
                    return hit;
                }
            }
        }
        return nullptr;
    }
    Widget* hit_test(Point world) override
    {
        if (!visible_)
        {
            return nullptr;
        }
        for (auto it = children().rbegin(); it != children().rend(); ++it)
        {
            if ((*it)->visible())
            {
                if (Widget* hit = (*it)->hit_test(world))
                {
                    return hit;
                }
            }
        }
        return nullptr;
    }
};

struct HoverFixture
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(100, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};

    Button* add_button(Widget& parent, Rect r, int* clicks = nullptr)
    {
        auto* b = parent.add_child(
            create_widget<Button>(&parent, "B", std::function<void()>{}, Button::Variant::Subtle));
        if (clicks)
        {
            b->set_on_click(
                [clicks]
                {
                    ++*clicks;
                });
        }
        b->arrange(lc, r);
        return b;
    }
};

} // namespace

TEST_CASE("Button hover follows the pointer in and out", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    TestHost host(&root);

    CHECK_FALSE(b->hovered());
    host.dispatch_pointer_move({20, 20});
    CHECK(b->hovered());
    host.dispatch_pointer_move({200, 200});
    CHECK_FALSE(b->hovered());
}

TEST_CASE("Button hover is cleared when the pointer leaves the surface", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    REQUIRE(b->hovered());
    host.dispatch_pointer_leave();
    CHECK_FALSE(b->hovered());
}

TEST_CASE("on_hover_enter / on_hover_leave fire once per transition", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    int enter = 0, leave = 0;
    b->set_on_hover_enter(
        [&]
        {
            ++enter;
        });
    b->set_on_hover_leave(
        [&]
        {
            ++leave;
        });
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    host.dispatch_pointer_move({25, 22}); // still inside — no new transition
    CHECK(enter == 1);
    CHECK(leave == 0);
    host.dispatch_pointer_move({200, 200});
    CHECK(enter == 1);
    CHECK(leave == 1);
}

TEST_CASE("A disabled Button is never hovered", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    b->set_enabled(false);
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK_FALSE(b->hovered());
}

TEST_CASE("Button still hovers under a parent that claims pointer-move", "[tk][button][hover]")
{
    HoverFixture f;
    HoverClaimingBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK(b->hovered());
    CHECK(host.hovered_widget_.lock().get() == &root); // routing unchanged
    host.dispatch_pointer_move({200, 200});
    CHECK_FALSE(b->hovered());
}

TEST_CASE("Button hovers beneath a transparent overlay stack", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    // A full-window stack painted above the button that has no children.
    root.add_child(create_widget<HoverTransparentStack>(&root, Rect{0, 0, 400, 400}));
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK(b->hovered());
}

TEST_CASE("Of two overlapping Buttons only the topmost is hovered", "[tk][button][hover]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* below = f.add_button(root, {10, 10, 60, 30});
    Button* above = f.add_button(root, {40, 10, 60, 30}); // added later = on top
    TestHost host(&root);

    host.dispatch_pointer_move({50, 20}); // inside both
    CHECK(above->hovered());
    CHECK_FALSE(below->hovered());
    host.dispatch_pointer_move({20, 20}); // only the lower one
    CHECK(below->hovered());
    CHECK_FALSE(above->hovered());
}

TEST_CASE("A Button inside the open popup is not hovered by the tree walk",
          "[tk][button][hover][popup]")
{
    // Today the popup branch of Host::dispatch_pointer_move clears Button
    // hover and routes pointer-move to the popup only; a Button inside the
    // popup is not hover-tracked there. Pinned so the generalisation makes a
    // deliberate change, not an accidental one.
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* behind = f.add_button(root, {110, 110, 50, 30});
    HoverBox popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_move({20, 20});   // elsewhere: hover nothing
    host.dispatch_pointer_move({120, 120}); // inside popup, over `behind`
    CHECK_FALSE(behind->hovered());
}

TEST_CASE("Button click fires only for an inside release", "[tk][button][click]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    int clicks = 0;
    Button* b = f.add_button(root, {10, 10, 50, 30}, &clicks);
    (void)b;
    TestHost host(&root);

    host.dispatch_pointer_down({20, 20});
    host.dispatch_pointer_up({20, 20});
    CHECK(clicks == 1);

    host.dispatch_pointer_down({20, 20});
    host.dispatch_pointer_up({300, 300}); // released outside
    CHECK(clicks == 1);
}

TEST_CASE("A disabled Button does not click", "[tk][button][click]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    int clicks = 0;
    Button* b = f.add_button(root, {10, 10, 50, 30}, &clicks);
    b->set_enabled(false);
    TestHost host(&root);

    host.dispatch_pointer_down({20, 20});
    host.dispatch_pointer_up({20, 20});
    CHECK(clicks == 0);
}

TEST_CASE("Button on_click may destroy the button", "[tk][button][click]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    Button* b = f.add_button(root, {10, 10, 50, 30});
    b->set_on_click(
        [&root, b]
        {
            root.remove_child(b);
        });
    TestHost host(&root);

    host.dispatch_pointer_down({20, 20});
    host.dispatch_pointer_up({20, 20}); // must not crash / use-after-free
    SUCCEED();
}

TEST_CASE("Button activates on Enter/Space only while focused", "[tk][button][keyboard]")
{
    HoverFixture f;
    HoverBox root({0, 0, 400, 400});
    int clicks_a = 0, clicks_b = 0;
    Button* a = f.add_button(root, {10, 10, 50, 30}, &clicks_a);
    Button* b = f.add_button(root, {80, 10, 50, 30}, &clicks_b);
    TestHost host(&root);

    host.dispatch_key_down({Key::Tab}); // focuses the first button
    REQUIRE(host.focused_widget() == a);
    host.dispatch_key_down({Key::Enter});
    CHECK(clicks_a == 1);
    CHECK(clicks_b == 0); // the unfocused button never fires
    (void)b;
}
