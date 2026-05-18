#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/AccountPicker.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::AccountEntry;
using tesseract::views::AccountPicker;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

std::vector<AccountEntry> two_entries()
{
    return {
        AccountEntry{"@alice:example.org", "Alice", "mxc://x/a", true},
        AccountEntry{"@bob:matrix.org", "Bob", "mxc://x/b", false},
    };
}

} // namespace

TEST_CASE("AccountPicker stacks rows vertically with summed natural height",
          "[tk][view][account_picker]")
{
    Stage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    auto lc = st.layout_ctx();
    auto sz = picker.measure(lc, {320.0f, 0.0f});

    // Two rows, ≈48 px each.
    CHECK(sz.h >= 80.0f);
    CHECK(sz.h <= 120.0f);
    CHECK(sz.w == 320.0f);
}

TEST_CASE("AccountPicker fires on_select with the clicked row's user_id",
          "[tk][view][account_picker]")
{
    Stage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    std::string got;
    picker.on_select = [&](const std::string& uid)
    {
        got = uid;
    };

    st.run(picker, {0, 0, 320, 200});

    // The two rows are stacked top-to-bottom from y=0. Click in the middle
    // of the second row.
    const auto& kids = picker.children();
    REQUIRE(kids.size() == 2);
    auto row1_bounds = kids[1]->bounds();
    const tk::Point click{
        row1_bounds.x + row1_bounds.w * 0.5f,
        row1_bounds.y + row1_bounds.h * 0.5f,
    };

    Widget* claimer = picker.dispatch_pointer_down(click);
    REQUIRE(claimer == kids[1].get());
    claimer->on_pointer_up({click.x - row1_bounds.x, click.y - row1_bounds.y},
                           /*inside_self=*/true);

    CHECK(got == "@bob:matrix.org");
}

TEST_CASE("AccountPicker active indicator paints on only the active row",
          "[tk][view][account_picker]")
{
    Stage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    const auto& kids = picker.children();
    REQUIRE(kids.size() == 2);

    auto* row_a = dynamic_cast<tesseract::views::UserInfo*>(kids[0].get());
    auto* row_b = dynamic_cast<tesseract::views::UserInfo*>(kids[1].get());
    REQUIRE(row_a);
    REQUIRE(row_b);

    CHECK(row_a->active_indicator()); // alice is the active entry
    CHECK_FALSE(row_b->active_indicator());
}

TEST_CASE("AccountPicker image_provider propagates to every row",
          "[tk][view][account_picker]")
{
    Stage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    std::vector<std::string> requested;
    picker.set_image_provider(
        [&](const std::string& mxc) -> const tk::Image*
        {
            requested.push_back(mxc);
            return nullptr;
        });

    st.run(picker, {0, 0, 320, 200});

    // Both rows requested their avatar; order matches the entry list.
    REQUIRE(requested.size() == 2);
    CHECK(requested[0] == "mxc://x/a");
    CHECK(requested[1] == "mxc://x/b");
}
