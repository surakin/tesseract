#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace tk;

namespace
{

constexpr std::string_view kSolidSquareSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">)"
    R"(<rect x="0" y="0" width="24" height="24" fill="#000"/></svg>)";

std::span<const std::uint8_t> solid_square_svg()
{
    return {reinterpret_cast<const std::uint8_t*>(kSolidSquareSvg.data()), kSolidSquareSvg.size()};
}

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 40);

    LayoutCtx layout_ctx() const
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }

    // Opaque solid-colour 16x16 image.
    std::unique_ptr<Image> solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) const
    {
        std::vector<std::uint8_t> px(16 * 16 * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = 255;
        }
        return surface->factory().create_image_rgba(px.data(), 16, 16);
    }

    void run(Widget& root, Rect bounds) const
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

std::unique_ptr<Button> make_button()
{
    return tk::create_root_widget<Button>(nullptr, "Continue", std::function<void()>{},
                                          Button::Variant::Subtle);
}

// Centre x of the leading slot when the button fills [0, 300).
int slot_cx(Stage& st, Button& btn)
{
    auto lc = st.layout_ctx();
    const float content_w = btn.measure(lc, {-1, -1}).w - 2 * 16.0f;
    return static_cast<int>((300.0f - content_w) * 0.5f + 8.0f);
}

int dist(Color a, Color b)
{
    auto d = [](int x, int y)
    {
        return std::abs(x - y);
    };
    return d(a.r, b.r) + d(a.g, b.g) + d(a.b, b.b);
}

} // namespace

TEST_CASE("Button leading image widens measure like a leading icon", "[tk][button][leading]")
{
    Stage st;
    auto img = st.solid(255, 0, 0);
    REQUIRE(img);
    auto lc = st.layout_ctx();

    auto with_icon = make_button();
    with_icon->set_leading_icon(solid_square_svg(), 16.0f);
    auto with_image = make_button();
    with_image->set_leading_image(img.get(), 16.0f);
    auto plain = make_button();

    const Size a = with_icon->measure(lc, {-1, -1});
    const Size b = with_image->measure(lc, {-1, -1});
    CHECK(b.w == a.w);
    CHECK(b.h == a.h);
    CHECK(b.w > plain->measure(lc, {-1, -1}).w);
    CHECK(with_image->access_name() == "Continue");
}

TEST_CASE("Button leading image takes precedence over monogram", "[tk][button][leading]")
{
    Stage st;
    auto img = st.solid(255, 0, 0);
    REQUIRE(img);
    auto btn = make_button();
    btn->set_leading_monogram("G", 16.0f);
    btn->set_leading_image(img.get(), 16.0f);

    const int cx = slot_cx(st, *btn);
    st.run(*btn, {0, 0, 300, 36});
    const Color px = st.surface->read_pixel(cx, 18);
    CHECK(dist(px, Color{255, 0, 0, 255}) < 40);
}

TEST_CASE("Button monogram paints in the icon slot", "[tk][button][leading]")
{
    Stage st;
    auto plain = make_button();
    st.run(*plain, {0, 0, 300, 36});

    auto btn = make_button();
    btn->set_leading_monogram("G", 16.0f);
    const int cx = slot_cx(st, *btn);
    // Repaint over a clean surface-state comparison: sample the circle edge
    // region left of the glyph centre, which the plain button leaves as fill.
    Stage st2;
    st2.run(*btn, {0, 0, 300, 36});
    const Color bg = st.surface->read_pixel(cx - 6, 18);
    const Color px = st2.surface->read_pixel(cx - 6, 18);
    CHECK(dist(px, bg) > 20);
}

TEST_CASE("Button cleared leading image returns to plain width", "[tk][button][leading]")
{
    Stage st;
    auto img = st.solid(0, 255, 0);
    REQUIRE(img);
    auto lc = st.layout_ctx();
    auto plain = make_button();
    auto btn = make_button();
    btn->set_leading_image(img.get(), 16.0f);
    CHECK(btn->measure(lc, {-1, -1}).w > plain->measure(lc, {-1, -1}).w);
    btn->set_leading_image(nullptr);
    CHECK(btn->measure(lc, {-1, -1}).w == plain->measure(lc, {-1, -1}).w);
}
