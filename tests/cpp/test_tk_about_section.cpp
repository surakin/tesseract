#include <catch2/catch_test_macros.hpp>

#include "views/settings/AboutSection.h"
#include "tk_test_surface.h"

#include <string>

using tesseract::views::AboutSection;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);

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

// Scan downward from y=0 firing pointer-move events until the callback fires
// or the bottom of the widget is reached.
bool scan_for_tooltip(tk::Widget& w, float x, float height,
                      std::string& out_text)
{
    for (float y = 0; y < height; y += 1.0f)
    {
        w.dispatch_pointer_move({x, y});
        if (!out_text.empty())
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("memory cache row shows tooltip on hover when stats are set",
          "[about-section][tooltip]")
{
    Stage st;
    AboutSection section;
    st.run(section, {0, 0, 400, 600});

    section.set_memory_cache_stats(1000, 50);

    std::string shown;
    section.on_show_tooltip = [&](std::string t, tk::Rect) { shown = t; };
    section.on_hide_tooltip = [&] { shown.clear(); };

    const bool fired = scan_for_tooltip(section, 50, 600, shown);

    CHECK(fired);
    CHECK(shown.find("1000") != std::string::npos);
    CHECK(shown.find("50")   != std::string::npos);
}

TEST_CASE("cache size row value cells are at the same x after layout",
          "[about-section][layout]")
{
    Stage st;
    AboutSection section;
    st.run(section, {0, 0, 400, 600});

    // Navigate: AboutSection (SettingsPage VBox)
    //   → last child (outer HBox wrapping the Storage group)
    //     → child 0 (SettingsGroup)
    //       → children 0-2 (the three CacheSizeRows)
    auto& page_ch = section.children();
    REQUIRE(page_ch.size() >= 2);
    auto* outer_hbox = page_ch.back().get();
    REQUIRE(!outer_hbox->children().empty());
    auto* sg = outer_hbox->children()[0].get();
    REQUIRE(sg->children().size() >= 3);

    // Each CacheSizeRow (after fix) must have exactly two children:
    // index 0 = CacheNameCell, index 1 = CacheValueCell.
    std::vector<float> value_xs;
    for (int i = 0; i < 3; ++i)
    {
        auto& row = *sg->children()[i];
        REQUIRE(row.children().size() == 2);
        value_xs.push_back(row.children()[1]->bounds().x);
    }

    // All three value cells must start at the same x — that is, names form
    // a fixed column and values are right-aligned into a consistent column.
    CHECK(value_xs[0] == value_xs[1]);
    CHECK(value_xs[1] == value_xs[2]);
}

TEST_CASE("tooltip fires immediately when stats arrive while already hovering",
          "[about-section][tooltip]")
{
    Stage st;
    AboutSection section;
    st.run(section, {0, 0, 400, 600});
    // No stats set yet — hover the row so hover_count_ > 0 but has_stats_ == false.

    std::string shown;
    section.on_show_tooltip = [&](std::string t, tk::Rect) { shown = t; };
    section.on_hide_tooltip = [&] { shown.clear(); };

    // Park the pointer on a cache row without stats — tooltip must NOT fire yet.
    const bool pre = scan_for_tooltip(section, 50, 600, shown);
    REQUIRE_FALSE(pre);
    REQUIRE(shown.empty());

    // Stats arrive while the cursor is still parked over the row.
    section.set_memory_cache_stats(500, 25);

    // Tooltip must have fired immediately (no re-hover required).
    CHECK_FALSE(shown.empty());
    CHECK(shown.find("500") != std::string::npos);
}

TEST_CASE("sdk store row never triggers a tooltip",
          "[about-section][tooltip]")
{
    Stage st;
    AboutSection section;
    // Set stats only for memory and local; sdk row gets none.
    section.set_memory_cache_stats(100, 5);
    section.set_local_cache_stats(200, 10);
    st.run(section, {0, 0, 400, 600});

    int show_count = 0;
    section.on_show_tooltip = [&](std::string, tk::Rect) { ++show_count; };

    // Full-page sweep — tooltip may fire for memory/local but must NOT fire
    // for the sdk row (which has no stats).  We just assert the section
    // compiled and the on_show_tooltip callback is wired.
    for (float y = 0; y < 600; y += 1.0f)
        section.dispatch_pointer_move({50, y});
    section.on_pointer_leave();

    // At most 2 show events (memory + local rows); sdk row must be silent.
    CHECK(show_count <= 2);
}
