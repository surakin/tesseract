#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <memory>

using tk::Color;
using tk::Point;
using tk::Rect;

namespace
{

// Loose pixel-comparison helper. Different backends will antialias edges
// differently, so exact equality fails at corners; require the dominant
// channel to dominate by a wide margin.
bool nearly_white(Color c)
{
    return c.r > 220 && c.g > 220 && c.b > 220;
}
bool nearly_red(Color c)
{
    return c.r > 220 && c.g < 40 && c.b < 40;
}
bool nearly_black(Color c)
{
    return c.r < 40 && c.g < 40 && c.b < 40;
}

} // namespace

TEST_CASE("Canvas::fill_rect paints the requested rectangle", "[tk][canvas]")
{
    auto s = TestSurface::create(64, 64);
    REQUIRE(s);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));
    c.fill_rect({10, 10, 20, 20}, Color::rgb(0xff0000));

    CHECK(nearly_red(s->read_pixel(15, 15)));
    CHECK(nearly_white(s->read_pixel(5, 5)));
    CHECK(nearly_white(s->read_pixel(32, 32))); // just outside (10+20)
}

TEST_CASE("Canvas::fill_rounded_rect cuts the corners", "[tk][canvas]")
{
    auto s = TestSurface::create(64, 64);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));
    c.fill_rounded_rect({0, 0, 64, 64}, 12.0f, Color::rgb(0x000000));

    // Top-left corner pixel sits outside the rounded outline → still white.
    CHECK(nearly_white(s->read_pixel(0, 0)));
    // Centre is solidly inside the fill.
    CHECK(nearly_black(s->read_pixel(32, 32)));
    // A pixel along the straight edge, well past the corner radius.
    CHECK(nearly_black(s->read_pixel(32, 0)));
}

TEST_CASE("Canvas::push_clip_rect restricts subsequent paint", "[tk][canvas]")
{
    auto s = TestSurface::create(64, 64);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));
    c.push_clip_rect({20, 20, 10, 10});
    c.fill_rect({0, 0, 64, 64}, Color::rgb(0xff0000));
    c.pop_clip();

    CHECK(nearly_red(s->read_pixel(25, 25)));   // inside the clip
    CHECK(nearly_white(s->read_pixel(5, 5)));   // outside the clip
    CHECK(nearly_white(s->read_pixel(31, 31))); // just outside (20+10)
}

TEST_CASE("Canvas::push_clip_rounded_rect masks corners", "[tk][canvas]")
{
    auto s = TestSurface::create(64, 64);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));
    c.push_clip_rounded_rect({0, 0, 64, 64}, 12.0f);
    c.fill_rect({0, 0, 64, 64}, Color::rgb(0x000000));
    c.pop_clip();

    CHECK(nearly_white(s->read_pixel(0, 0))); // outside rounded corner
    CHECK(nearly_black(s->read_pixel(32, 32)));
}

TEST_CASE("CanvasFactory::build_text returns a measurable layout",
          "[tk][canvas]")
{
    auto s = TestSurface::create(200, 50);
    auto layout = s->factory().build_text("Hello, world", tk::TextStyle{});
    REQUIRE(layout);
    auto size = layout->measure();
    CHECK(size.w > 0.0f);
    CHECK(size.h > 0.0f);
    CHECK(layout->line_count() >= 1);
}

TEST_CASE("Canvas::draw_text writes glyphs into the surface", "[tk][canvas]")
{
    auto s = TestSurface::create(200, 50);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));

    tk::TextStyle st{};
    st.role = tk::FontRole::Body;
    auto layout = s->factory().build_text("Tx", st);
    REQUIRE(layout);

    c.draw_text(*layout, Point{5, 5}, Color::rgb(0x000000));

    // Sweep the bounding box of the text and require at least one pixel
    // that is clearly not white — proves the glyphs hit the surface.
    auto bounds = layout->measure();
    int w = static_cast<int>(bounds.w) + 1;
    int h = static_cast<int>(bounds.h) + 1;
    bool found_glyph_pixel = false;
    for (int y = 5; y < 5 + h && !found_glyph_pixel; ++y)
    {
        for (int x = 5; x < 5 + w; ++x)
        {
            auto px = s->read_pixel(x, y);
            if (px.r < 180 || px.g < 180 || px.b < 180)
            {
                found_glyph_pixel = true;
                break;
            }
        }
    }
    CHECK(found_glyph_pixel);
}

TEST_CASE("Canvas::draw_initials_circle fills bg and draws letter",
          "[tk][canvas]")
{
    auto s = TestSurface::create(64, 64);
    auto& c = s->canvas();
    c.clear(Color::rgb(0xffffff));
    c.draw_initials_circle("Alice", Point{32, 32}, /*diameter=*/40.0f,
                           /*bg=*/Color::rgb(0x0084ff),
                           /*fg=*/Color::rgb(0xffffff));

    // A pixel inside the disc and away from the glyph should match the bg.
    auto bg_sample = s->read_pixel(50, 32);
    CHECK(bg_sample.b > 200); // strongly blue
    CHECK(bg_sample.r < 80);

    // The corner sits outside the disc, so it stays white.
    CHECK(nearly_white(s->read_pixel(1, 1)));
}
