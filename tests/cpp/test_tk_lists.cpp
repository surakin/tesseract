#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/theme.h"
#include "views/MessageListView.h"
#include "views/RoomListView.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>
#include <vector>

using namespace tk;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomListView;
using tesseract::RoomInfo;

namespace {

struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);
    LayoutCtx layout_ctx() {
        return LayoutCtx{ surface->factory(), Theme::light() };
    }
    PaintCtx paint_ctx() {
        return PaintCtx{ surface->canvas(), surface->factory(),
                          Theme::light() };
    }
    void run(Widget& root, Rect bounds) {
        auto lc = layout_ctx();
        root.measure(lc, { bounds.w, bounds.h });
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

// Minimal adapter for ListView mechanics testing.
struct FixedHeightAdapter : ListAdapter {
    std::size_t n = 0;
    float row_h = 30.0f;
    std::vector<int> click_log;

    std::size_t count() const override { return n; }
    float measure_row_height(std::size_t, LayoutCtx&, float) override {
        return row_h;
    }
    void paint_row(std::size_t index, PaintCtx& ctx, Rect bounds,
                    bool selected, bool /*hovered*/) override {
        Color c = selected ? Color::rgb(0x0084FF)
                            : (index % 2 == 0 ? Color::rgb(0xEEEEEE)
                                                : Color::rgb(0xFFFFFF));
        ctx.canvas.fill_rect(bounds, c);
    }
};

} // namespace

TEST_CASE("ListView lays out variable + fixed rows + reports content height",
          "[tk][listview]") {
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 10;
    ad.row_h = 25.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, { 0, 0, 200, 100 });

    CHECK(list.content_height() == 25.0f * 10);
    CHECK(list.scroll_y() == 0.0f);

    // index_at, with no scroll.
    CHECK(list.index_at({ 50, 12 }) == 0);
    CHECK(list.index_at({ 50, 28 }) == 1);   // 25..50 = row 1
    CHECK(list.index_at({ 50, 99 }) == 3);   // row 3 spans 75..100
}

TEST_CASE("ListView clamps scroll to [0, content - viewport]",
          "[tk][listview]") {
    Stage st;
    ListView list;
    FixedHeightAdapter ad; ad.n = 4; ad.row_h = 50.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, { 0, 0, 200, 100 });

    // 4 * 50 = 200 content, viewport 100 → max scroll = 100.
    CHECK(list.on_wheel({ 50, 50 }, 0, 30));   // scroll down by 30
    CHECK(list.scroll_y() == 30.0f);
    CHECK(list.on_wheel({ 50, 50 }, 0, 200));  // try to over-scroll
    CHECK(list.scroll_y() == 100.0f);          // clamped
    list.on_wheel({ 50, 50 }, 0, -1000);       // try to under-scroll
    CHECK(list.scroll_y() == 0.0f);
}

TEST_CASE("ListView::on_pointer_down/up fires on_row_clicked",
          "[tk][listview]") {
    Stage st;
    ListView list;
    FixedHeightAdapter ad; ad.n = 5; ad.row_h = 20.0f;
    list.set_adapter(&ad);
    int clicked = -1;
    list.on_row_clicked = [&](int idx) { clicked = idx; };

    auto lc = st.layout_ctx();
    list.arrange(lc, { 0, 0, 200, 200 });

    // Click on row 2 (y in [40, 60]).
    CHECK(list.on_pointer_down({ 50, 45 }));
    list.on_pointer_up({ 50, 45 }, true);
    CHECK(clicked == 2);
    CHECK(list.selected_index() == 2);
}

TEST_CASE("ListView pointer_up outside the row doesn't fire click",
          "[tk][listview]") {
    Stage st;
    ListView list;
    FixedHeightAdapter ad; ad.n = 5; ad.row_h = 20.0f;
    list.set_adapter(&ad);
    int clicked = -1;
    list.on_row_clicked = [&](int idx) { clicked = idx; };

    auto lc = st.layout_ctx();
    list.arrange(lc, { 0, 0, 200, 200 });

    CHECK(list.on_pointer_down({ 50, 45 }));     // row 2
    list.on_pointer_up({ 50, 45 }, false);       // released outside
    CHECK(clicked == -1);
    CHECK(list.selected_index() == -1);
}

TEST_CASE("ListView::scroll_to_bottom snaps to the end",
          "[tk][listview]") {
    Stage st;
    ListView list;
    FixedHeightAdapter ad; ad.n = 50; ad.row_h = 20.0f;
    list.set_adapter(&ad);
    auto lc = st.layout_ctx();
    list.arrange(lc, { 0, 0, 200, 100 });

    list.scroll_to_bottom();
    list.arrange(lc, { 0, 0, 200, 100 });   // sticky-bottom snaps on arrange
    CHECK(list.scroll_y() == list.content_height() - 100);
}

// ─────────────────────────────────────────────────────────────────────────
//  RoomListView
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("RoomListView paints rows + tracks selection by room ID",
          "[tk][view][roomlist]") {
    Stage st;
    RoomListView view;

    std::vector<RoomInfo> rooms;
    rooms.push_back({ "!a:example.org", "Alpha room", "", 2,
                       false, "", "preview a", 0, false });
    rooms.push_back({ "!b:example.org", "Beta room",  "", 0,
                       false, "", "preview b", 0, false });
    rooms.push_back({ "!c:example.org", "Gamma room", "", 99,
                       false, "", "",          0, false });
    view.set_rooms(rooms);

    std::string selected;
    view.on_room_selected = [&](const std::string& id) { selected = id; };

    st.run(view, { 0, 0, 260, 200 });

    // Each row is 62 px tall; the click at y=70 lands inside row 1.
    REQUIRE(view.on_pointer_down({ 10, 70 }));
    view.on_pointer_up({ 10, 70 }, true);
    CHECK(selected == "!b:example.org");
    CHECK(view.selected_room_id() == "!b:example.org");

    // set_selected_room translates ID → index.
    view.set_selected_room("!c:example.org");
    CHECK(view.selected_index() == 2);
}

TEST_CASE("RoomListView preserves selection after rooms swap",
          "[tk][view][roomlist]") {
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        { "!a:x", "A", "", 0, false, "", "", 0, false },
        { "!b:x", "B", "", 0, false, "", "", 0, false },
    };
    view.set_rooms(rooms);
    view.set_selected_room("!b:x");
    CHECK(view.selected_index() == 1);

    // Re-order; "b" is now index 0.
    std::vector<RoomInfo> rooms2 = {
        { "!b:x", "B", "", 0, false, "", "", 0, false },
        { "!a:x", "A", "", 0, false, "", "", 0, false },
    };
    view.set_rooms(rooms2);
    CHECK(view.selected_index() == 0);
    CHECK(view.selected_room_id() == "!b:x");
}

// ─────────────────────────────────────────────────────────────────────────
//  MessageListView
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("MessageListView lays out text rows with avatar + body + timestamp",
          "[tk][view][messagelist]") {
    Stage st;
    MessageListView view;

    MessageRowData m1{};
    m1.kind = MessageRowData::Kind::Text;
    m1.event_id    = "$evt1";
    m1.sender_name = "Alice";
    m1.body        = "Hello, world!";
    m1.timestamp_ms = 1700000000000ull;

    MessageRowData m2{};
    m2.kind = MessageRowData::Kind::Text;
    m2.event_id    = "$evt2";
    m2.sender_name = "Bob";
    m2.body        = "Hi Alice! This message should wrap onto two lines if "
                      "the column is narrow enough.";
    m2.timestamp_ms = 1700000060000ull;
    m2.reactions.push_back({"\xF0\x9F\x91\x8D", 2, false, {}, {}}); // 👍

    view.set_messages({ m1, m2 });
    st.run(view, { 0, 0, 320, 400 });

    REQUIRE(view.messages().size() == 2);
    CHECK(view.content_height() > 0);

    // Append should extend the list and (because we were pinned) scroll
    // to the new bottom.
    MessageRowData m3{}; m3.kind = MessageRowData::Kind::Text;
    m3.event_id = "$evt3"; m3.sender_name = "Carol"; m3.body = "Last";
    view.append_message(m3);
    st.run(view, { 0, 0, 320, 400 });
    CHECK(view.messages().size() == 3);
}

TEST_CASE("MessageListView measures Image rows taller than text rows",
          "[tk][view][messagelist]") {
    Stage st;
    MessageListView view;

    MessageRowData txt{}; txt.kind = MessageRowData::Kind::Text;
    txt.event_id = "$t"; txt.sender_name = "A"; txt.body = "tiny";

    MessageRowData img{};
    img.kind = MessageRowData::Kind::Image;
    img.event_id    = "$i";
    img.sender_name = "B";
    img.media_url   = "mxc://example.org/picture";
    img.media_w     = 800;
    img.media_h     = 500;

    view.set_messages({ txt, img });
    st.run(view, { 0, 0, 360, 800 });

    // Heights aren't directly exposed; check that the message row total
    // height accounts for the image (200 px max-cap) on top of chrome.
    CHECK(view.content_height() > 200.0f);
}

TEST_CASE("MessageListView click fires on_message_clicked with event_id",
          "[tk][view][messagelist]") {
    Stage st;
    MessageListView view;
    MessageRowData m{}; m.kind = MessageRowData::Kind::Text;
    m.event_id = "$only"; m.sender_name = "A"; m.body = "click me";
    view.set_messages({ m });

    std::string clicked;
    view.on_message_clicked = [&](const std::string& id) { clicked = id; };

    st.run(view, { 0, 0, 320, 200 });
    REQUIRE(view.on_pointer_down({ 50, 20 }));
    view.on_pointer_up({ 50, 20 }, true);
    CHECK(clicked == "$only");
}

// ─────────────────────────────────────────────────────────────────────────
//  Reaction chips: tooltip data, "+" pseudo-chip, click → callbacks
// ─────────────────────────────────────────────────────────────────────────

namespace {

MessageRowData make_row_with_reactions() {
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$evt";
    m.sender_name = "Alice";
    m.body = "hi";
    tesseract::Reaction r1{};
    r1.key = "👍";
    r1.count = 2;
    r1.reacted_by_me = false;
    r1.senders = { "@alice:example.org", "@bob:example.org" };
    tesseract::Reaction r2{};
    r2.key = "🎉";
    r2.count = 1;
    r2.reacted_by_me = true;
    r2.senders = { "@carol:example.org" };
    m.reactions = { r1, r2 };
    return m;
}

// First paint the view as hovered so the chip geometry is captured,
// then a second paint to make hover state stick. Pointer-move on the
// hovered row records the chip rects we'll then hit-test against.
void paint_with_hover(Stage& st, MessageListView& view, Rect bounds,
                       tk::Point local_in_row) {
    st.run(view, bounds);
    // First pointer-move primes ListView's hovered_row_index_, but
    // hovered_row_geom_ is empty until paint_row runs for the hovered
    // row. Painting again now picks that row up and records geometry.
    view.on_pointer_move(local_in_row);
    auto pc = st.paint_ctx();
    view.paint(pc);
    // A second pointer-move now hit-tests against the just-recorded
    // chip rects.
    view.on_pointer_move(local_in_row);
}

} // namespace

TEST_CASE("MessageListView paints + pseudo-chip only on hover",
          "[tk][view][messagelist][reactions]") {
    Stage st;
    MessageListView view;
    view.set_messages({ make_row_with_reactions() });
    st.run(view, { 0, 0, 320, 200 });

    // No hover yet: add-button is not visible.
    CHECK_FALSE(view.hovered_row_geom().add_visible);

    // Move pointer into the row.
    view.on_pointer_move({ 50, 20 });
    auto pc = st.paint_ctx();
    view.paint(pc);

    CHECK(view.hovered_row_geom().add_visible);
    CHECK(view.hovered_row_geom().chips.size() == 2);
}

TEST_CASE("MessageListView resolves chip hover target",
          "[tk][view][messagelist][reactions]") {
    Stage st;
    MessageListView view;
    view.set_messages({ make_row_with_reactions() });
    paint_with_hover(st, view, { 0, 0, 320, 200 }, { 50, 20 });

    // Aim at the centre of the first reaction chip.
    auto chips = view.hovered_row_geom().chips;
    REQUIRE(chips.size() == 2);
    tk::Point in_chip{
        chips[0].x + chips[0].w * 0.5f - view.bounds().x,
        chips[0].y + chips[0].h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_chip);
    CHECK(view.hover_target() == MessageListView::HoverTarget::Chip);
    CHECK(view.hover_chip_index() == 0);

    // And then the second chip.
    tk::Point in_chip2{
        chips[1].x + chips[1].w * 0.5f - view.bounds().x,
        chips[1].y + chips[1].h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_chip2);
    CHECK(view.hover_chip_index() == 1);

    // Aim at the centre of the "+" pseudo-chip.
    auto add = view.hovered_row_geom().add_button;
    REQUIRE(add.w > 0);
    tk::Point in_add{
        add.x + add.w * 0.5f - view.bounds().x,
        add.y + add.h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_add);
    CHECK(view.hover_target() == MessageListView::HoverTarget::AddButton);
}

TEST_CASE("MessageListView reaction-chip click fires on_reaction_toggled",
          "[tk][view][messagelist][reactions]") {
    Stage st;
    MessageListView view;
    view.set_messages({ make_row_with_reactions() });
    paint_with_hover(st, view, { 0, 0, 320, 200 }, { 50, 20 });

    std::string got_event, got_key;
    view.on_reaction_toggled =
        [&](const std::string& ev, const std::string& k) {
            got_event = ev; got_key = k;
        };

    auto chips = view.hovered_row_geom().chips;
    REQUIRE(chips.size() == 2);
    tk::Point in_chip{
        chips[1].x + chips[1].w * 0.5f - view.bounds().x,
        chips[1].y + chips[1].h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_chip);
    REQUIRE(view.on_pointer_down(in_chip));
    view.on_pointer_up(in_chip, true);

    CHECK(got_event == "$evt");
    CHECK(got_key   == "🎉");
}

TEST_CASE("MessageListView + button click fires on_add_reaction_requested",
          "[tk][view][messagelist][reactions]") {
    Stage st;
    MessageListView view;
    view.set_messages({ make_row_with_reactions() });
    paint_with_hover(st, view, { 0, 0, 320, 200 }, { 50, 20 });

    std::string got_event;
    view.on_add_reaction_requested =
        [&](const std::string& ev, tk::Rect /*anchor*/) {
            got_event = ev;
        };

    auto add = view.hovered_row_geom().add_button;
    REQUIRE(add.w > 0);
    tk::Point in_add{
        add.x + add.w * 0.5f - view.bounds().x,
        add.y + add.h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_add);
    REQUIRE(view.on_pointer_down(in_add));
    view.on_pointer_up(in_add, true);

    CHECK(got_event == "$evt");
}
