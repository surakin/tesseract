#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/MessageListView.h"
#include "tk_test_surface.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

MessageRowData make_text_row(const std::string& id, std::uint64_t ts)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender_name = "User";
    r.body = "row " + id;
    r.timestamp_ms = ts;
    return r;
}

MessageRowData make_day_separator(std::uint64_t ts)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::DaySeparator;
    r.timestamp_ms = ts;
    return r;
}

struct DateBadgeStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    void run(tk::Widget& root, tk::Rect bounds)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("date badge is hidden for an empty timeline", "[message_list][date_badge]")
{
    DateBadgeStage st;
    MessageListView v;
    st.run(v, {0, 0, 320, 200});
    CHECK_FALSE(v.date_badge_visible());
}

TEST_CASE("date badge is hidden at the bottom and shown once scrolled up, "
          "even with only one day of history loaded",
          "[message_list][date_badge]")
{
    DateBadgeStage st;
    MessageListView v;

    const std::uint64_t ts = now_ms();
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 60; ++i)
        rows.push_back(make_text_row("$e" + std::to_string(i), ts));

    v.set_messages(std::move(rows), /*room_switch=*/true); // auto-scrolls to bottom
    st.run(v, {0, 0, 320, 200});
    REQUIRE(v.content_height() > 200.0f); // content really overflows the viewport
    CHECK_FALSE(v.date_badge_visible());

    v.scroll_to_top();
    st.run(v, {0, 0, 320, 200});
    // No DaySeparator row exists anywhere in this timeline, but the badge
    // must still show — visibility is driven purely by "not at the live
    // tail" (matches Telegram/WhatsApp/Signal), not by day count.
    CHECK(v.date_badge_visible());
    CHECK_FALSE(v.date_badge_label().empty());

    v.scroll_to_bottom();
    st.run(v, {0, 0, 320, 200});
    CHECK_FALSE(v.date_badge_visible());
}

TEST_CASE("date badge label reflects the day of the topmost visible row",
          "[message_list][date_badge]")
{
    DateBadgeStage st;
    MessageListView v;

    const std::uint64_t ts_old = now_ms() - std::uint64_t{6} * 86400000ULL;
    const std::uint64_t ts_new = now_ms();

    std::vector<MessageRowData> rows;
    for (int i = 0; i < 30; ++i)
        rows.push_back(make_text_row("$old" + std::to_string(i), ts_old));
    rows.push_back(make_day_separator(ts_new));
    for (int i = 0; i < 60; ++i)
        rows.push_back(make_text_row("$new" + std::to_string(i), ts_new));

    v.set_messages(std::move(rows), /*room_switch=*/true);
    st.run(v, {0, 0, 320, 200});

    // Top of history: topmost visible row is one of the old-day rows.
    v.scroll_to_index(0, /*align_top=*/true);
    st.run(v, {0, 0, 320, 200});
    REQUIRE(v.date_badge_visible());
    const std::string label_old = v.date_badge_label();
    CHECK_FALSE(label_old.empty());

    // Deep into the new-day rows, but well short of the absolute bottom so
    // the badge stays eligible to show.
    v.scroll_to_index(40, /*align_top=*/true);
    st.run(v, {0, 0, 320, 200});
    REQUIRE(v.date_badge_visible());
    const std::string label_new = v.date_badge_label();
    CHECK_FALSE(label_new.empty());

    CHECK(label_old != label_new);
}

TEST_CASE("date badge rests at the same position when no DaySeparator row "
          "is near the top of the viewport",
          "[message_list][date_badge]")
{
    DateBadgeStage st;
    MessageListView v;

    const std::uint64_t ts_old = now_ms() - std::uint64_t{6} * 86400000ULL;
    const std::uint64_t ts_new = now_ms();

    std::vector<MessageRowData> rows;
    for (int i = 0; i < 30; ++i)
        rows.push_back(make_text_row("$old" + std::to_string(i), ts_old));
    rows.push_back(make_day_separator(ts_new));
    for (int i = 0; i < 60; ++i)
        rows.push_back(make_text_row("$new" + std::to_string(i), ts_new));

    v.set_messages(std::move(rows), /*room_switch=*/true);
    st.run(v, {0, 0, 320, 200});

    // Far from the only separator in the timeline (it sits well below both
    // of these scroll positions) — the badge should rest at the same,
    // un-pushed world_y in both cases rather than drifting.
    v.scroll_to_index(0, /*align_top=*/true);
    st.run(v, {0, 0, 320, 200});
    REQUIRE(v.date_badge_visible());
    const float rest_y_a = v.date_badge_bounds().y;

    v.scroll_to_index(40, /*align_top=*/true);
    st.run(v, {0, 0, 320, 200});
    REQUIRE(v.date_badge_visible());
    const float rest_y_b = v.date_badge_bounds().y;

    CHECK(rest_y_a == rest_y_b);
}
