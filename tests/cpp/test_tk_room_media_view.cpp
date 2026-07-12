// Regression coverage for the room-media-gallery "spamming" bug: opening the
// gallery in a media-sparse room (few items, content shorter than the
// viewport) must not re-fire on_load_older_media on every relayout forever.
// RoomMediaView.h documents on_load_older_media as firing "at most once per
// approach to the top" — tk::ListView::arrange()'s unconditional per-arrange
// autofill (list_view.cpp) violates that unless the list opts into
// autofill_only_when_empty (as MessageListView already does).

#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/RoomMediaView.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::MessageRowData;
using tesseract::views::RoomMediaView;

namespace
{

struct TkRoomMediaViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx() { return {surface->factory(), Theme::light()}; }
    PaintCtx  paint_ctx()  { return {surface->canvas(), surface->factory(), Theme::light()}; }
};

MessageRowData make_image_row(const std::string& event_id)
{
    MessageRowData row;
    row.kind     = MessageRowData::Kind::Image;
    row.event_id = event_id;
    return row;
}

} // namespace

TEST_CASE(
    "RoomMediaView does not keep re-firing on_load_older_media once populated",
    "[tk][roommediaview]")
{
    TkRoomMediaViewStage st;
    RoomMediaView view;
    view.open("!room:example.org", "Test Room");

    // Only 3 items — content height is far shorter than the 600px viewport,
    // exactly the media-sparse-room scenario that triggered the bug.
    std::vector<MessageRowData> rows;
    rows.push_back(make_image_row("$ev1"));
    rows.push_back(make_image_row("$ev2"));
    rows.push_back(make_image_row("$ev3"));
    view.set_media(std::move(rows));

    int fires = 0;
    view.on_load_older_media = [&](std::string) { ++fires; };

    auto lc = st.layout_ctx();
    // Simulate several relayout passes in a row (schedule_relayout_ /
    // request_relayout_ firing repeatedly while pagination results land) —
    // none of them are a real user scroll gesture.
    for (int i = 0; i < 5; ++i)
    {
        view.arrange(lc, {0, 0, 800, 600});
    }

    // Contract per RoomMediaView.h: "at most once per approach to the top."
    // Before the fix this fired once per arrange() call (5 times).
    CHECK(fires <= 1);
}

TEST_CASE("RoomMediaView::content_fills_viewport reflects actual content vs bounds",
          "[tk][roommediaview]")
{
    TkRoomMediaViewStage st;
    RoomMediaView view;
    view.open("!room:example.org", "Test Room");
    auto lc = st.layout_ctx();
    view.arrange(lc, {0, 0, 800, 600});

    // No items at all: content is far shorter than the 600px viewport.
    CHECK_FALSE(view.content_fills_viewport());

    // A handful of items still doesn't fill a tall viewport.
    std::vector<MessageRowData> few;
    few.push_back(make_image_row("$ev1"));
    few.push_back(make_image_row("$ev2"));
    few.push_back(make_image_row("$ev3"));
    view.set_media(std::move(few));
    view.arrange(lc, {0, 0, 800, 600});
    CHECK_FALSE(view.content_fills_viewport());

    // Enough rows to exceed 600px of grid content.
    std::vector<MessageRowData> many;
    for (int i = 0; i < 200; ++i)
        many.push_back(make_image_row("$ev" + std::to_string(i)));
    view.set_media(std::move(many));
    view.arrange(lc, {0, 0, 800, 600});
    CHECK(view.content_fills_viewport());
}

TEST_CASE("RoomMediaView::estimated_capacity is 0 before any arrange() pass",
          "[tk][roommediaview]")
{
    // Reproduces the "stopped after only 6" bug: a fixed magic-number floor
    // used as the real capacity target stops pagination long before the
    // actual (much larger) viewport is filled. estimated_capacity() must
    // report the real, geometry-derived target once arranged — and 0 (a
    // clear "unknown yet" signal, not a plausible real capacity) beforehand,
    // so callers know to fall back rather than mistake it for "viewport is
    // tiny."
    RoomMediaView view;
    view.open("!room:example.org", "Test Room");
    CHECK(view.estimated_capacity() == 0);
}

TEST_CASE("RoomMediaView::estimated_capacity reflects the real grid size once "
          "arranged",
          "[tk][roommediaview]")
{
    TkRoomMediaViewStage st;
    RoomMediaView view;
    view.open("!room:example.org", "Test Room");
    auto lc = st.layout_ctx();
    view.arrange(lc, {0, 0, 800, 600});

    // 800x600 comfortably fits several columns and rows of 120px cells —
    // must be far more than a small fixed constant (the bug this guards
    // against: a threshold of 6 stopped pagination almost immediately).
    CHECK(view.estimated_capacity() > 20);

    // A much smaller viewport must report a correspondingly smaller target.
    view.arrange(lc, {0, 0, 200, 200});
    CHECK(view.estimated_capacity() < 20);
    CHECK(view.estimated_capacity() > 0);
}
