#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/PinnedBanner.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <memory>
#include <string>

using tesseract::PinnedEvent;
using tesseract::views::PinnedBanner;

namespace
{

PinnedEvent make_pin(const std::string& id, std::uint64_t ts)
{
    PinnedEvent p;
    p.event_id     = id;
    p.sender_name  = "Alice";
    p.body_preview = "Important";
    p.timestamp    = ts;
    return p;
}

struct TkPinnedBannerStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 200);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    void arrange(tk::Widget& w, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }
};

} // namespace

TEST_CASE("PinnedBanner::set_pins stores the list", "[pinned_banner]")
{
    PinnedBanner b;
    b.set_pins({make_pin("$a", 100), make_pin("$b", 200)});
    REQUIRE(b.pins().size() == 2);
    CHECK(b.pins()[0].event_id == "$a");
    CHECK(b.pins()[1].event_id == "$b");
    CHECK(b.current_index() == 0);
}

TEST_CASE("PinnedBanner::set_pins clamps current_index_ when list shrinks to empty",
          "[pinned_banner]")
{
    PinnedBanner b;
    b.set_pins({make_pin("$a", 100), make_pin("$b", 200), make_pin("$c", 300)});
    // current_index_ starts at 0; shrink to empty and verify index is 0.
    b.set_pins({});
    CHECK(b.current_index() == 0);
    CHECK(b.pins().empty());
}

TEST_CASE("PinnedBanner::on_jump_to fires for the currently-displayed pin",
          "[pinned_banner]")
{
    TkPinnedBannerStage st;
    PinnedBanner b;
    b.set_pins({make_pin("$pinned", 100)});
    st.arrange(b, {0, 0, 400, PinnedBanner::kBannerH});

    std::string clicked;
    b.on_jump_to = [&](const std::string& id) { clicked = id; };

    // Body rect starts at x=0 and is wide; click near the middle.
    const tk::Point p{50.0f, PinnedBanner::kBannerH * 0.5f};
    REQUIRE(b.on_pointer_down(p));
    b.on_pointer_up(p, /*inside_self=*/true);
    CHECK(clicked == "$pinned");
}
