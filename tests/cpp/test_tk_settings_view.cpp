#include <catch2/catch_test_macros.hpp>

#include "views/SettingsView.h"
#include "views/settings/UserPackEditor.h"
#include "tk/side_tab_view.h"
#include "tk/widget.h"
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

// The "Emojis & Stickers" tab is index 7 in tab-registration order:
// Account=0, Sessions=1, Appearance=2, Notifications=3, Media=4, Privacy=5,
// Server=6, Emojis & Stickers=7.
constexpr int kUserPackTabIdx = 7;

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

TEST_CASE("SettingsView: file drop reaches the personal pack editor only "
          "when its tab is selected",
          "[settings-view]")
{
    TkSettingsViewStage st;
    SettingsView view;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    auto* editor = view.user_pack_editor();
    REQUIRE(editor);

    // Default-selected tab (Account) — the personal pack grid is arranged
    // (SideTabView lays out every tab's content each pass) but is not
    // visible, so tree dispatch must skip it (Widget::dispatch_file_drop
    // bails on !visible_) and leave the payload untouched.
    CHECK(tabs->selected_idx() != kUserPackTabIdx);
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == nullptr);
        CHECK_FALSE(payload.bytes.empty());
    }

    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == editor);
    }

    tabs->select(0);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == nullptr);
    }
}

TEST_CASE("SettingsView: drag-hover claims the personal pack editor only "
          "when its tab is selected",
          "[settings-view]")
{
    TkSettingsViewStage st;
    SettingsView view;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    auto* editor = view.user_pack_editor();
    REQUIRE(editor);

    CHECK(tabs->selected_idx() != kUserPackTabIdx);
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        CHECK(view.dispatch_drag_hover(p) == nullptr);
        CHECK_FALSE(editor->drag_hover());
    }

    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        CHECK(view.dispatch_drag_hover(p) == editor);
        CHECK(editor->drag_hover());

        // No Host in this fixture to drive the leave transition — call the
        // leaf directly, mirroring what Host::dispatch_drag_hover/
        // dispatch_drag_leave would do on the previous claimant.
        editor->on_drag_leave();
        CHECK_FALSE(editor->drag_hover());
    }
}

TEST_CASE("SettingsView: file drop stages an image into the personal "
          "pack editor",
          "[settings-view]")
{
    TkSettingsViewStage st;
    SettingsView view;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});

    auto* editor = view.user_pack_editor();
    REQUIRE(editor);
    CHECK(editor->images().empty());
    CHECK_FALSE(editor->has_changes());

    const tk::Rect r = editor->list_rect();
    tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
    CHECK(view.dispatch_file_drop({r.x + 5, r.y + 5}, payload) == editor);

    CHECK(editor->images().size() == 1);
    CHECK(editor->has_changes());
}
