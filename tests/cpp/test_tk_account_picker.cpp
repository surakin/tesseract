#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/AccountPicker.h"
#include "views/UserInfo.h"
#include "tk_test_surface.h"

#include <catch2/catch_approx.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::AccountEntry;
using tesseract::views::AccountPicker;

namespace
{

struct TkAccountPickerStage
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

std::vector<AccountEntry> n_entries(int n)
{
    std::vector<AccountEntry> out;
    for (int i = 0; i < n; ++i)
    {
        const std::string id = std::to_string(i);
        out.push_back(AccountEntry{"@user" + id + ":example.org", "User " + id,
                                   "", i == 0});
    }
    return out;
}

float one_row_height(LayoutCtx& lc)
{
    tesseract::views::UserInfo one_row;
    one_row.set_display_name("Alice");
    one_row.set_user_id("@alice:example.org");
    return one_row.measure(lc, {320.0f, 0.0f}).h;
}

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
    TkAccountPickerStage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    auto lc = st.layout_ctx();

    // Height is exactly N × one UserInfo row (status line disabled). The
    // shells size the popup window from picker.measure(), so this must track
    // UserInfo::measure() — the regression that would have caught the sidebar
    // avatar 40→44 bump silently clipping the popup's last row.
    tesseract::views::UserInfo one_row;
    one_row.set_display_name("Alice");
    one_row.set_user_id("@alice:example.org");
    const float row_h = one_row.measure(lc, {320.0f, 0.0f}).h;
    REQUIRE(row_h > 0.0f);

    auto sz = picker.measure(lc, {320.0f, 0.0f});
    CHECK(sz.h == Catch::Approx(2.0f * row_h));
    CHECK(sz.w == 320.0f);
}

TEST_CASE("AccountPicker fires on_select with the clicked row's user_id",
          "[tk][view][account_picker]")
{
    TkAccountPickerStage st;
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
    const auto& kids = picker.rows();
    REQUIRE(kids.size() == 2);
    auto row1_bounds = kids[1]->bounds();
    const tk::Point click{
        row1_bounds.x + row1_bounds.w * 0.5f,
        row1_bounds.y + row1_bounds.h * 0.5f,
    };

    Widget* claimer = picker.dispatch_pointer_down(click);
    REQUIRE(claimer == kids[1]);
    claimer->on_pointer_up({click.x - row1_bounds.x, click.y - row1_bounds.y},
                           /*inside_self=*/true);

    CHECK(got == "@bob:matrix.org");
}

TEST_CASE("AccountPicker active indicator paints on only the active row",
          "[tk][view][account_picker]")
{
    TkAccountPickerStage st;
    AccountPicker picker;
    picker.set_entries(two_entries());

    const auto& kids = picker.rows();
    REQUIRE(kids.size() == 2);

    auto* row_a = kids[0];
    auto* row_b = kids[1];

    CHECK(row_a->active_indicator()); // alice is the active entry
    CHECK_FALSE(row_b->active_indicator());
}

TEST_CASE("AccountPicker image_provider propagates to every row",
          "[tk][view][account_picker]")
{
    TkAccountPickerStage st;
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

TEST_CASE("AccountPicker picks up accounts added or removed after first build",
          "[tk][view][account_picker]")
{
    // Every shell keeps one picker alive and re-calls set_entries() on each
    // open, so a login/logout between opens must change the row count.
    TkAccountPickerStage st;
    AccountPicker picker;
    picker.set_entries(n_entries(3));
    REQUIRE(picker.rows().size() == 3);

    picker.set_entries(n_entries(4));
    REQUIRE(picker.rows().size() == 4);
    CHECK(picker.rows()[3]->user_id() == "@user3:example.org");

    std::string got;
    picker.on_select = [&](const std::string& uid) { got = uid; };
    st.run(picker, {0, 0, 320, 400});
    const auto b = picker.rows()[3]->bounds();
    const tk::Point click{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
    Widget* claimer = picker.dispatch_pointer_down(click);
    REQUIRE(claimer == picker.rows()[3]);
    claimer->on_pointer_up({click.x - b.x, click.y - b.y}, /*inside_self=*/true);
    CHECK(got == "@user3:example.org");

    picker.set_entries(n_entries(2));
    CHECK(picker.rows().size() == 2);
}

TEST_CASE("AccountPicker caps its height at kMaxVisibleRows and scrolls the rest",
          "[tk][view][account_picker]")
{
    TkAccountPickerStage st;
    auto lc = st.layout_ctx();
    const float row_h = one_row_height(lc);
    REQUIRE(row_h > 0.0f);

    AccountPicker picker;
    picker.set_entries(n_entries(AccountPicker::kMaxVisibleRows));
    CHECK(picker.measure(lc, {320.0f, 0.0f}).h ==
          Catch::Approx(AccountPicker::kMaxVisibleRows * row_h));

    const int total = static_cast<int>(AccountPicker::kMaxVisibleRows) + 3;
    picker.set_entries(n_entries(total));
    const float h = picker.measure(lc, {320.0f, 0.0f}).h;
    CHECK(h == Catch::Approx(AccountPicker::kMaxVisibleRows * row_h));

    picker.arrange(lc, {0, 0, 320, h});
    const float last_top_before = picker.rows().back()->bounds().y;
    CHECK(last_top_before >= h); // overflow row starts below the viewport

    // Wheel down: the scroll view moves the rows up on the next arrange.
    picker.dispatch_wheel({160.0f, h * 0.5f}, 0.0f, 1000.0f);
    picker.arrange(lc, {0, 0, 320, h});
    CHECK(picker.rows().back()->bounds().y < last_top_before);
    CHECK(picker.rows().back()->bounds().y + row_h ==
          Catch::Approx(h).margin(0.5f));
}
