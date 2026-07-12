#include <catch2/catch_test_macros.hpp>

#include "views/SettingsView.h"
#include "tk/side_tab_view.h"
#include "tk_test_surface.h"

using tesseract::views::SettingsView;

namespace
{

struct TkSettingsViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(900, 700);

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

// The hidden Advanced tab is index 10 in tab-registration order (see
// SettingsView::kAdvancedTabIdx: 9 top tabs [0-8], bottom tabs About=9,
// Advanced=10).
constexpr int kAdvancedTabIdx = 10;

tk::SideTabView* find_tabs(SettingsView& view)
{
    for (auto& c : view.children())
    {
        if (auto* t = dynamic_cast<tk::SideTabView*>(c.get()))
            return t;
    }
    return nullptr;
}

} // namespace

TEST_CASE("SettingsView: Advanced tab is hidden by default", "[settings-view]")
{
    TkSettingsViewStage st;
    SettingsView view;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    CHECK_FALSE(tabs->tab_visible(kAdvancedTabIdx));
}

TEST_CASE("SettingsView: Advanced tab hides again after navigating away",
          "[settings-view]")
{
    TkSettingsViewStage st;
    SettingsView view;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);

    // Reveal it (mirrors AboutSection's "Advanced" button handler) and
    // navigate to it.
    tabs->set_tab_visible(kAdvancedTabIdx, true);
    tabs->select(kAdvancedTabIdx);
    REQUIRE(tabs->tab_visible(kAdvancedTabIdx));
    REQUIRE(tabs->selected_idx() == kAdvancedTabIdx);

    // Navigate to a different tab — Advanced must hide again.
    tabs->select(0);
    CHECK_FALSE(tabs->tab_visible(kAdvancedTabIdx));
}
