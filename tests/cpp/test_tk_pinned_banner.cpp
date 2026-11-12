#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
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
    p.event_id = id;
    p.sender_name = "Alice";
    p.body_preview = "Important";
    p.timestamp = ts;
    return p;
}

struct TkPinnedBannerStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 200);
    tk::LayoutCtx layout_ctx() const
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    void arrange(tk::Widget& w, tk::Rect bounds) const
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

TEST_CASE("PinnedBanner::on_jump_to fires for the currently-displayed pin", "[pinned_banner]")
{
    TkPinnedBannerStage st;
    PinnedBanner b;
    b.set_pins({make_pin("$pinned", 100)});
    st.arrange(b, {0, 0, 400, PinnedBanner::kBannerH});

    std::string clicked;
    b.on_jump_to = [&](const std::string& id)
    {
        clicked = id;
    };

    // The body is a real button filling the banner's left part; press and
    // release near its middle (button-local coordinates).
    tk::Button* body = b.body_button();
    REQUIRE(body != nullptr);
    REQUIRE(body->visible());
    const tk::Point p{50.0f, PinnedBanner::kBannerH * 0.5f};
    REQUIRE(body->on_pointer_down(p));
    body->on_pointer_up(p, /*inside_self=*/true);
    CHECK(clicked == "$pinned");
}

TEST_CASE("PinnedBanner chevrons step through pins and are named for AT",
          "[pinned_banner][accessibility]")
{
    TkPinnedBannerStage st;
    PinnedBanner b;
    b.set_pins({make_pin("$a", 100), make_pin("$b", 200)});
    st.arrange(b, {0, 0, 400, PinnedBanner::kBannerH});

    CHECK(b.access_role() == tk::Role::Group);
    CHECK(b.access_name() == "Pinned messages");
    CHECK(b.access_description() == "1 of 2");
    REQUIRE(b.previous_button()->visible());
    CHECK_FALSE(b.previous_button()->enabled());
    CHECK(b.next_button()->access_name() == "Next pinned message");
    CHECK(b.body_button()->access_name() == "Jump to pinned message: Alice: Important");

    CHECK(b.next_button()->access_default_action());
    CHECK(b.current_index() == 1);
    CHECK(b.access_description() == "2 of 2");
    CHECK(b.previous_button()->enabled());
    CHECK_FALSE(b.next_button()->enabled());

    b.set_pins({make_pin("$only", 1)});
    CHECK_FALSE(b.previous_button()->visible());
}
