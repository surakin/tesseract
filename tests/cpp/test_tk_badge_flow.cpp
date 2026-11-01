#include <catch2/catch_test_macros.hpp>

#include "tk/badge_flow.h"
#include "tk/theme.h"
#include "tk_test_surface.h"

using namespace tk;

namespace
{
std::vector<BadgeFlow::Badge> badges(std::initializer_list<const char*> labels)
{
    std::vector<BadgeFlow::Badge> out;
    for (const char* l : labels)
    {
        out.push_back({l, {}, {}});
    }
    return out;
}
} // namespace

TEST_CASE("BadgeFlow has no height when empty", "[tk][badge_flow]")
{
    auto surface = TestSurface::create(300, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};
    auto flow = create_root_widget<BadgeFlow>(nullptr);
    CHECK(flow->measure(lc, {250.0f, 0.0f}).h == 0.0f);
}

TEST_CASE("BadgeFlow wraps onto more rows as the width shrinks", "[tk][badge_flow]")
{
    auto surface = TestSurface::create(300, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};
    auto flow = create_root_widget<BadgeFlow>(nullptr);
    flow->set_badges(badges({"Encrypted", "Shared history", "English (United States)"}));

    const Size wide = flow->measure(lc, {1000.0f, 0.0f});
    const Size narrow = flow->measure(lc, {120.0f, 0.0f});
    CHECK(wide.h > 0.0f);
    CHECK(narrow.h > wide.h);
    CHECK(narrow.w <= 120.0f);
}

TEST_CASE("BadgeFlow ellipsizes a single badge wider than the row", "[tk][badge_flow]")
{
    auto surface = TestSurface::create(300, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};
    auto flow = create_root_widget<BadgeFlow>(nullptr);
    flow->set_badges(badges({"A very long bridged network name that cannot possibly fit"}));

    const Size one_row = flow->measure(lc, {1000.0f, 0.0f});
    const Size squeezed = flow->measure(lc, {100.0f, 0.0f});
    CHECK(squeezed.w <= 100.0f);
    CHECK(squeezed.h == one_row.h);
}
