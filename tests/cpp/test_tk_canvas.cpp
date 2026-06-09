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

TEST_CASE("build_text layout char_index_at returns valid offset for plain text",
          "[tk][canvas][selection]")
{
    auto s = TestSurface::create(400, 60);
    tk::TextStyle st{};
    st.role      = tk::FontRole::Body;
    st.wrap      = true;
    st.max_width = 380.0f;
    auto layout  = s->factory().build_text("Hello world", st);
    REQUIRE(layout);

    auto sz  = layout->measure();
    tk::Point mid{sz.w * 0.5f, sz.h * 0.5f};
    int idx = layout->char_index_at(mid);
    CHECK(idx >= 0);
    CHECK(idx <= static_cast<int>(std::string("Hello world").size()));
}

TEST_CASE("build_text layout selection_rects returns non-empty rects",
          "[tk][canvas][selection]")
{
    auto s = TestSurface::create(400, 60);
    tk::TextStyle st{};
    st.role      = tk::FontRole::Body;
    st.wrap      = true;
    st.max_width = 380.0f;
    auto layout  = s->factory().build_text("Hello world", st);
    REQUIRE(layout);

    auto rects = layout->selection_rects(0, 5); // "Hello"
    CHECK(!rects.empty());
    for (const auto& r : rects)
    {
        CHECK(r.w > 0.0f);
        CHECK(r.h > 0.0f);
    }
}

TEST_CASE("build_text layout text_range extracts correct UTF-8 substring",
          "[tk][canvas][selection]")
{
    auto s = TestSurface::create(400, 60);
    tk::TextStyle st{};
    st.role      = tk::FontRole::Body;
    st.wrap      = true;
    st.max_width = 380.0f;
    auto layout  = s->factory().build_text("Hello world", st);
    REQUIRE(layout);

    CHECK(layout->text_range(0, 5)  == "Hello");
    CHECK(layout->text_range(6, 11) == "world");
    CHECK(layout->text_range(3, 3).empty());
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

TEST_CASE("tk::initials_of applies the shared word-split policy",
          "[tk][canvas][initials]")
{
    using tk::initials_of;

    // Single word → first grapheme only.
    CHECK(initials_of("Alice") == "A");
    // Two words → first grapheme of each.
    CHECK(initials_of("Ada Lovelace") == "AL");
    // Three+ words → still only the first two.
    CHECK(initials_of("Jean Luc Picard") == "JL");
    // Empty / whitespace-only → sentinel.
    CHECK(initials_of("") == "?");
    CHECK(initials_of("   ") == "?");
    // Leading @ / # are content, not separators: they ARE the initial.
    CHECK(initials_of("@neo") == "@");
    // Shared policy preserves source case (backends uppercase natively), so
    // "#room name" → '#' (first word's first grapheme) + 'n' (second word's),
    // unchanged case.
    CHECK(initials_of("#room name") == "#n");

    // The shared policy preserves source case (backends uppercase natively).
    // Lowercase input therefore stays lowercase here.
    CHECK(initials_of("bob smith") == "bs");

    // UTF-8 multibyte: must not split mid-codepoint. "Éric Ñoño" → É + Ñ
    // (each is 2 UTF-8 bytes); verify exact bytes survive intact.
    {
        const std::string r = initials_of("\xC3\x89ric \xC3\x91o\xC3\xB1o");
        CHECK(r == "\xC3\x89\xC3\x91"); // "ÉÑ"
    }
    // A 4-byte code point (emoji) as a single-word name → that whole cluster.
    {
        const std::string r = initials_of("\xF0\x9F\x98\x80 face"); // 😀 face
        CHECK(r == "\xF0\x9F\x98\x80\x66");                          // 😀 + 'f'
    }
    // NBSP (U+00A0, C2 A0) is treated as a separator.
    CHECK(initials_of("Ada\xC2\xA0Lovelace") == "AL");
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
