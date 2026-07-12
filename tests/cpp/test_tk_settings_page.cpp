#include <catch2/catch_test_macros.hpp>

#include "views/settings/SettingsPage.h"
#include "tk_test_surface.h"

#include <memory>

using tesseract::views::SettingsPage;

namespace
{

// Reports a fixed natural height regardless of the constraint offered, so
// tests can build a SettingsPage with a known, predictable content height.
struct FixedHeightWidget : tk::Widget
{
    explicit FixedHeightWidget(float h) : height(h)
    {
    }
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w, height};
    }
    void paint(tk::PaintCtx&) override
    {
    }
    float height;
};

struct TkSettingsPageStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("SettingsPage: wheel scrolls when content exceeds viewport",
          "[settings][page][scroll]")
{
    TkSettingsPageStage st;
    SettingsPage page;
    // 20 widgets at 80px each, comfortably exceeding a 240px viewport once
    // SettingsPage's own padding/spacing is added in.
    for (int i = 0; i < 20; ++i)
    {
        page.add_widget(std::make_unique<FixedHeightWidget>(80.0f));
    }
    st.run(page, {0.0f, 0.0f, 640.0f, 240.0f});

    INFO("content_height = " << page.content_height_for_testing()
                              << ", viewport = 240");
    REQUIRE(page.content_height_for_testing() > 240.0f);

    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, 60.0f) == true);
    st.run(page, {0.0f, 0.0f, 640.0f, 240.0f});

    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, 1e6f) == true);
    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, 60.0f) == false);

    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, -1e6f) == true);
    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, -60.0f) == false);
}

TEST_CASE("SettingsPage: wheel is a no-op when content fits the viewport",
          "[settings][page][scroll]")
{
    TkSettingsPageStage st;
    SettingsPage page;
    page.add_widget(std::make_unique<FixedHeightWidget>(40.0f));
    page.add_widget(std::make_unique<FixedHeightWidget>(40.0f));
    st.run(page, {0.0f, 0.0f, 640.0f, 800.0f});

    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, 120.0f) == false);
    REQUIRE(page.on_wheel({10.0f, 10.0f}, 0.0f, -120.0f) == false);
}
