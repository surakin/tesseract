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
        g.preview_url = "https://cdn/" + std::to_string(i) + ".gif";
        g.mp4_url = "https://cdn/" + std::to_string(i) + ".mp4";
        g.mp4_w = 100;
        g.mp4_h = 80;
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
