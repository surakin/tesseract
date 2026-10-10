#include <catch2/catch_test_macros.hpp>

#include "tk/theme.h"
#include "tk/tooltip.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <string>

using namespace tk;

namespace
{
struct TipStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 300);
    PaintCtx pc()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
};
} // namespace

TEST_CASE("Tooltip paint is a no-op without content", "[tk][tooltip]")
{
    TipStage s;
    Tooltip t;
    auto pc = s.pc();
    t.paint_overlay(pc, {0, 0, 400, 300});
    SUCCEED();
}

TEST_CASE("Tooltip reveal restarts on reset_reveal and paints while easing",
          "[tk][tooltip]")
{
    TipStage s;
    Tooltip t;
    t.set_content("hello", {100, 100, 40, 20});
    t.reset_reveal();
    CHECK(t.still_revealing());
    auto pc = s.pc();
    t.paint_overlay(pc, {0, 0, 400, 300}); // easing: drawn under an opacity push
    t.reset_reveal();
    CHECK(t.still_revealing());
}

TEST_CASE("Tooltip paints for anchors at every edge, long text and tiny "
          "surfaces without throwing",
          "[tk][tooltip]")
{
    TipStage s;
    Tooltip t;
    auto pc = s.pc();
    const std::string longtext(600, 'w');
    const Rect anchors[] = {{0, 0, 10, 10},    {390, 0, 10, 10},
                            {0, 290, 10, 10},  {390, 290, 10, 10},
                            {150, 150, 20, 20}};
    for (const Rect& a : anchors)
    {
        t.set_content("short", a);
        t.paint_overlay(pc, {0, 0, 400, 300});
        t.set_content(longtext, a); // rebuilds layout (text changed)
        t.paint_overlay(pc, {0, 0, 400, 300});
        t.set_content(longtext, a);
        t.paint_overlay(pc, {0, 0, 120, 80}); // width changed -> rebuild
    }
    t.set_content("x", {0, 0, 1, 1});
    t.paint_overlay(pc, {0, 0, 4, 4}); // surface narrower than padding
    SUCCEED();
}
