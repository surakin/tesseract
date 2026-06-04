#include <catch2/catch_test_macros.hpp>

#include "views/GifPopup.h"

#include <tesseract/types.h>

using tesseract::GifResult;
using tesseract::views::GifPopup;

namespace
{
std::vector<GifResult> make_results(int n)
{
    std::vector<GifResult> v;
    for (int i = 0; i < n; ++i)
    {
        GifResult g;
        g.id = "id" + std::to_string(i);
        g.preview_url = "https://cdn/" + std::to_string(i) + ".jpg";
        g.image_url = "https://cdn/" + std::to_string(i) + ".webp";
        g.image_w = 100;
        g.image_h = 80;
        g.image_mime = "image/webp";
        v.push_back(std::move(g));
    }
    return v;
}
} // namespace

TEST_CASE("GifPopup preselects first result", "[view][gif][popup]")
{
    GifPopup p;
    CHECK(p.selected_index() == -1);
    p.set_results(make_results(3));
    CHECK(p.visible_count() == 3);
    CHECK(p.selected_index() == 0);
    REQUIRE(p.selected() != nullptr);
    CHECK(p.selected()->id == "id0");
}

TEST_CASE("GifPopup move_selection clamps to range", "[view][gif][popup]")
{
    GifPopup p;
    p.set_results(make_results(3));

    CHECK(p.move_selection(+1));
    CHECK(p.selected_index() == 1);
    p.move_selection(+1);
    p.move_selection(+1); // clamps at last
    CHECK(p.selected_index() == 2);
    p.move_selection(-5); // clamps at first
    CHECK(p.selected_index() == 0);
}

TEST_CASE("GifPopup caps visible_count at kMaxCells", "[view][gif][popup]")
{
    GifPopup p;
    p.set_results(make_results(GifPopup::kMaxCells + 5));
    CHECK(p.visible_count() == GifPopup::kMaxCells);
}

TEST_CASE("GifPopup empty results clears selection", "[view][gif][popup]")
{
    GifPopup p;
    p.set_results(make_results(2));
    p.set_results({});
    CHECK(p.visible_count() == 0);
    CHECK(p.selected_index() == -1);
    CHECK(p.selected() == nullptr);
    CHECK_FALSE(p.move_selection(+1));
}

TEST_CASE("GifPopup content_size clamps the strip to the window width",
          "[view][gif][popup]")
{
    GifPopup p;
    p.set_results(make_results(GifPopup::kMaxCells)); // far wider than a window

    const float full = 2.0f * GifPopup::kPad +
                       float(GifPopup::kMaxCells) * GifPopup::kCellW +
                       float(GifPopup::kMaxCells - 1) * GifPopup::kGap;

    // Unbounded (max_width <= 0) returns the full content width.
    CHECK(p.content_size(0.0f).w == full);

    // A narrow window clamps the width but never the cell height.
    const float narrow = 400.0f;
    tk::Size sz = p.content_size(narrow);
    CHECK(sz.w == narrow);
    CHECK(sz.h == 2.0f * GifPopup::kPad + GifPopup::kCellH + GifPopup::kAttribH);

    // A window wider than the content does not stretch the strip.
    CHECK(p.content_size(full + 500.0f).w == full);
}

TEST_CASE("GifPopup status mode sizes a compact row and shows no cells",
          "[view][gif][popup]")
{
    GifPopup p;
    p.set_results(make_results(3));
    p.set_status("No GIF API key configured");

    CHECK(p.has_status());
    CHECK(p.visible_count() == 0);  // results cleared
    CHECK(p.selected() == nullptr); // nothing selectable

    tk::Size sz = p.content_size(800.0f);
    CHECK(sz.w >= GifPopup::kStatusMinW);
    CHECK(sz.w <= 800.0f);
    CHECK(sz.h == 2.0f * GifPopup::kPad + GifPopup::kStatusH);

    // Setting results again clears the status.
    p.set_results(make_results(2));
    CHECK_FALSE(p.has_status());
}

TEST_CASE("GifPopup with no results and no status has zero size",
          "[view][gif][popup]")
{
    GifPopup p;
    CHECK(p.content_size(800.0f).w == 0.0f);
    CHECK(p.content_size(800.0f).h == 0.0f);
}
