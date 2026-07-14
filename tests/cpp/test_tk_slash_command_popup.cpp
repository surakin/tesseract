#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "views/SlashCommandPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"

#include <optional>
#include <string>
#include <vector>

using namespace tk;
using Catch::Approx;
using tesseract::views::SlashCommandPopup;
using tesseract::views::SlashCommandSuggestion;

namespace
{

struct TkSlashCommandPopupStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 400);
    LayoutCtx lc()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx pc()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& w, Rect bounds)
    {
        auto l = lc();
        w.measure(l, {bounds.w, bounds.h});
        w.arrange(l, bounds);
        auto p = pc();
        w.paint(p);
    }
};

std::vector<SlashCommandSuggestion> make_suggestions(int n)
{
    std::vector<SlashCommandSuggestion> out;
    for (int i = 0; i < n; ++i)
    {
        SlashCommandSuggestion s;
        s.name = "cmd" + std::to_string(i);
        s.args_hint = "";
        s.description = "description " + std::to_string(i);
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace

TEST_CASE("SlashCommandPopup:capped at 8 rows even with 11 suggestions")
{
    TkSlashCommandPopupStage st;
    SlashCommandPopup popup;
    popup.set_suggestions(make_suggestions(11));
    auto lc = st.lc();
    auto sz = popup.measure(lc, {SlashCommandPopup::kWidth, 400.0f});
    CHECK(sz.h == Approx(8.0f * SlashCommandPopup::kRowHeight));
}

TEST_CASE("SlashCommandPopup:set_selected_index reaches rows past the 8-row viewport")
{
    TkSlashCommandPopupStage st;
    SlashCommandPopup popup;
    auto suggestions = make_suggestions(11);
    popup.set_suggestions(suggestions);

    std::optional<SlashCommandSuggestion> accepted;
    popup.on_accepted = [&](SlashCommandSuggestion s) { accepted = std::move(s); };

    Rect bounds{0, 0, SlashCommandPopup::kWidth, 8.0f * SlashCommandPopup::kRowHeight};
    st.run(popup, bounds);

    // Index 10 is unreachable under the old visible_rows()-clamped nav range
    // (max index 7); it must now be selectable and scrolled into view.
    popup.set_selected_index(10);
    CHECK(popup.selected_index() == 10);

    auto p = st.pc();
    popup.paint(p); // must not crash while scrolled

    // With row 10 scrolled to the bottom of the 8-row viewport, it now paints
    // in the viewport's last slot (index 7 of 0..7).
    float row_h = SlashCommandPopup::kRowHeight;
    popup.on_pointer_down({100.0f, row_h * 7.0f + row_h * 0.5f});
    popup.on_pointer_up({100.0f, row_h * 7.0f + row_h * 0.5f}, true);

    REQUIRE(accepted.has_value());
    CHECK(accepted->name == suggestions[10].name);
}

TEST_CASE("SlashCommandPopup:wheel scrolls the viewport once content overflows")
{
    TkSlashCommandPopupStage st;
    SlashCommandPopup popup;
    auto suggestions = make_suggestions(11);
    popup.set_suggestions(suggestions);

    std::optional<SlashCommandSuggestion> accepted;
    popup.on_accepted = [&](SlashCommandSuggestion s) { accepted = std::move(s); };

    Rect bounds{0, 0, SlashCommandPopup::kWidth, 8.0f * SlashCommandPopup::kRowHeight};
    st.run(popup, bounds);

    float row_h = SlashCommandPopup::kRowHeight;
    CHECK(popup.on_wheel({0, 0}, 0.0f, row_h)); // scroll down by one row

    popup.on_pointer_down({100.0f, row_h * 0.5f});
    popup.on_pointer_up({100.0f, row_h * 0.5f}, true);

    REQUIRE(accepted.has_value());
    CHECK(accepted->name == suggestions[1].name);
}

TEST_CASE("SlashCommandPopup:wheel still moves selection when content fits the viewport")
{
    TkSlashCommandPopupStage st;
    SlashCommandPopup popup;
    auto suggestions = make_suggestions(3);
    popup.set_suggestions(suggestions);

    Rect bounds{0, 0, SlashCommandPopup::kWidth, 8.0f * SlashCommandPopup::kRowHeight};
    st.run(popup, bounds);

    CHECK(popup.selected_index() == 0);
    CHECK(popup.on_wheel({0, 0}, 0.0f, 1.0f));
    CHECK(popup.selected_index() == 1);
}
