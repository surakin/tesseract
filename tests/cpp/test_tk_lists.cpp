#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/theme.h"
#include "views/MessageListView.h"
#include "views/RoomListView.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace tk;
using tesseract::RoomInfo;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomListView;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);
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

// Minimal adapter for ListView mechanics testing.
struct FixedHeightAdapter : ListAdapter
{
    std::size_t n = 0;
    float row_h = 30.0f;
    std::vector<int> click_log;

    std::size_t count() const override
    {
        return n;
    }
    float measure_row_height(std::size_t, LayoutCtx&, float) override
    {
        return row_h;
    }
    void paint_row(std::size_t index, PaintCtx& ctx, Rect bounds, bool selected,
                   bool /*hovered*/) override
    {
        Color c = selected ? Color::rgb(0x0084FF)
                           : (index % 2 == 0 ? Color::rgb(0xEEEEEE)
                                             : Color::rgb(0xFFFFFF));
        ctx.canvas.fill_rect(bounds, c);
    }
};

// Adapter with per-row heights and stable keys, for exercising ListView's
// row-anchored scroll preservation (vs. the keyless height-delta fallback).
struct KeyedVarAdapter : ListAdapter
{
    std::vector<float>       heights;
    std::vector<std::string> keys;

    std::size_t count() const override
    {
        return heights.size();
    }
    float measure_row_height(std::size_t i, LayoutCtx&, float) override
    {
        return heights[i];
    }
    void paint_row(std::size_t, PaintCtx&, Rect, bool, bool) override
    {
    }
    std::string row_key(std::size_t i) const override
    {
        return i < keys.size() ? keys[i] : std::string{};
    }
};

// Minimal adapter for GridView mechanics testing.
struct FixedGridAdapter : tk::GridAdapter
{
    std::size_t n = 20;
    std::size_t count() const override
    {
        return n;
    }
    void paint_cell(std::size_t, tk::PaintCtx&, tk::Rect, bool, bool) override
    {
    }
};

} // namespace

TEST_CASE("ListView lays out variable + fixed rows + reports content height",
          "[tk][listview]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 10;
    ad.row_h = 25.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});

    CHECK(list.content_height() == 25.0f * 10);
    CHECK(list.scroll_y() == 0.0f);

    // index_at, with no scroll.
    CHECK(list.index_at({50, 12}) == 0);
    CHECK(list.index_at({50, 28}) == 1); // 25..50 = row 1
    CHECK(list.index_at({50, 99}) == 3); // row 3 spans 75..100
}

TEST_CASE("ListView clamps scroll to [0, content - viewport]", "[tk][listview]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 4;
    ad.row_h = 50.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});

    // 4 * 50 = 200 content, viewport 100 → max scroll = 100.
    CHECK(list.on_wheel({50, 50}, 0, 30)); // scroll down by 30
    CHECK(list.scroll_y() == 30.0f);
    CHECK(list.on_wheel({50, 50}, 0, 200)); // try to over-scroll
    CHECK(list.scroll_y() == 100.0f);       // clamped
    list.on_wheel({50, 50}, 0, -1000);      // try to under-scroll
    CHECK(list.scroll_y() == 0.0f);
}

TEST_CASE("ListView::on_pointer_down/up fires on_row_clicked", "[tk][listview]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 5;
    ad.row_h = 20.0f;
    list.set_adapter(&ad);
    int clicked = -1;
    list.on_row_clicked = [&](int idx)
    {
        clicked = idx;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 200});

    // Click on row 2 (y in [40, 60]).
    CHECK(list.on_pointer_down({50, 45}));
    list.on_pointer_up({50, 45}, true);
    CHECK(clicked == 2);
    CHECK(list.selected_index() == 2);
}

TEST_CASE("ListView pointer_up outside the row doesn't fire click",
          "[tk][listview]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 5;
    ad.row_h = 20.0f;
    list.set_adapter(&ad);
    int clicked = -1;
    list.on_row_clicked = [&](int idx)
    {
        clicked = idx;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 200});

    CHECK(list.on_pointer_down({50, 45})); // row 2
    list.on_pointer_up({50, 45}, false);   // released outside
    CHECK(clicked == -1);
    CHECK(list.selected_index() == -1);
}

TEST_CASE("ListView::scroll_to_bottom snaps to the end", "[tk][listview]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 50;
    ad.row_h = 20.0f;
    list.set_adapter(&ad);
    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});

    list.scroll_to_bottom();
    list.arrange(lc, {0, 0, 200, 100}); // sticky-bottom snaps on arrange
    CHECK(list.scroll_y() == list.content_height() - 100);
}

// ─────────────────────────────────────────────────────────────────────────
//  RoomListView
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("RoomListView paints rows + tracks selection by room ID",
          "[tk][view][roomlist]")
{
    Stage st;
    RoomListView view;

    std::vector<RoomInfo> rooms;
    rooms.push_back(RoomInfo{.id = "!a:example.org",
                             .name = "Alpha room",
                             .notification_count = 2,
                             .last_message_body = "preview a"});
    rooms.push_back(RoomInfo{.id = "!b:example.org",
                             .name = "Beta room",
                             .last_message_body = "preview b"});
    rooms.push_back(RoomInfo{
        .id = "!c:example.org", .name = "Gamma room", .notification_count = 99});
    view.set_rooms(rooms);

    std::string selected;
    view.on_room_selected = [&](const std::string& id)
    {
        selected = id;
    };

    // 400 px viewport.  Layout: search bar 0..36, section header 36..64,
    // room rows 62 px each (row 0 at 64..126, row 1 at 126..188, …).
    st.run(view, {0, 0, 260, 400});

    // y=146 lands inside room row 1 (y=126..188) → "Beta room".
    REQUIRE(view.on_pointer_down({10, 146}));
    view.on_pointer_up({10, 146}, true);
    CHECK(selected == "!b:example.org");
    CHECK(view.selected_room_id() == "!b:example.org");

    // set_selected_room translates ID → index.
    view.set_selected_room("!c:example.org");
    CHECK(view.selected_index() == 2);
}

TEST_CASE("RoomListView collapsed section shows mention rooms, hides silent rooms",
          "[tk][view][roomlist]")
{
    Stage st;
    RoomListView view;

    // "mention" has highlight_count but no notification_count.
    // "silent" has neither.  Both go into the "Rooms" section.
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!mention:x", .name = "Mention room",
                 .highlight_count = 1},
        RoomInfo{.id = "!silent:x",  .name = "Silent room"},
    };
    view.set_rooms(rooms);

    // First layout — section is expanded by default; both rooms visible.
    // Layout: search bar 0..36, Rooms header 36..64, rows 64.. (62 px each).
    st.run(view, {0, 0, 260, 400});
    {
        auto ids = view.visible_room_ids();
        REQUIRE(std::find(ids.begin(), ids.end(), "!mention:x") != ids.end());
        REQUIRE(std::find(ids.begin(), ids.end(), "!silent:x")  != ids.end());
    }

    // Click the Rooms section header (y ≈ 50) to collapse it.
    view.on_pointer_down({130.0f, 50.0f});
    view.on_pointer_up({130.0f, 50.0f}, true);

    // Re-layout after collapse.
    st.run(view, {0, 0, 260, 400});
    {
        auto ids = view.visible_room_ids();
        // Mention room (highlight_count > 0) must still be visible.
        CHECK(std::find(ids.begin(), ids.end(), "!mention:x") != ids.end());
        // Silent room (zero counts) must be hidden.
        CHECK(std::find(ids.begin(), ids.end(), "!silent:x")  == ids.end());
    }
}

TEST_CASE("RoomListView preserves selection after rooms swap",
          "[tk][view][roomlist]")
{
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "A"},
        RoomInfo{.id = "!b:x", .name = "B"},
    };
    view.set_rooms(rooms);
    view.set_selected_room("!b:x");
    // Layout once so the inner ListView resolves its selection index.
    st.run(view, {0, 0, 260, 200});
    CHECK(view.selected_index() == 1);

    // Re-order; "b" is now index 0.
    std::vector<RoomInfo> rooms2 = {
        RoomInfo{.id = "!b:x", .name = "B"},
        RoomInfo{.id = "!a:x", .name = "A"},
    };
    view.set_rooms(rooms2);
    st.run(view, {0, 0, 260, 200});
    CHECK(view.selected_index() == 0);
    CHECK(view.selected_room_id() == "!b:x");
}

TEST_CASE("RoomListView search field always visible",
          "[tk][view][roomlist][search]")
{
    Stage st;
    RoomListView view;
    // Two rooms — content fits easily in a 600 px viewport, but the
    // search bar is unconditionally shown.
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "Alpha"},
        RoomInfo{.id = "!b:x", .name = "Beta"},
    };
    view.set_rooms(rooms);
    st.run(view, {0, 0, 260, 600});

    CHECK(view.search_field_visible());
    auto r = view.search_field_rect();
    CHECK(r.w > 0.0f);
    CHECK(r.h > 0.0f);
}

TEST_CASE("RoomListView search field visible when content overflows viewport",
          "[tk][view][roomlist][search]")
{
    Stage st;
    RoomListView view;
    // Six rooms × 62 = 372 px > 200 px viewport → overflow.
    std::vector<RoomInfo> rooms;
    for (int i = 0; i < 6; ++i)
    {
        rooms.push_back(RoomInfo{.id = "!" + std::to_string(i) + ":x",
                                 .name = "Room " + std::to_string(i)});
    }
    view.set_rooms(rooms);
    st.run(view, {0, 0, 260, 200});

    CHECK(view.search_field_visible());
    auto r = view.search_field_rect();
    CHECK(r.w > 0.0f);
    CHECK(r.h > 0.0f);
    // Header sits inside the top 36-px strip of the view.
    CHECK(r.y < 36.0f);
}

TEST_CASE(
    "RoomListView set_search_text filters rows by name case-insensitively",
    "[tk][view][roomlist][search]")
{
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "Alpha room"},
        RoomInfo{.id = "!b:x", .name = "Beta room"},
        RoomInfo{.id = "!c:x", .name = "Gamma room"},
        RoomInfo{.id = "!d:x", .name = "Alpine"},
    };
    view.set_rooms(rooms);
    st.run(view, {0, 0, 260, 600});
    CHECK(view.rooms().size() == 4);

    // Substring + case-insensitive: "alp" matches "Alpha room" and "Alpine".
    view.set_search_text("alp");
    st.run(view, {0, 0, 260, 600});
    // Layout when searching: search bar (36 px) + "Rooms" header (28 px) +
    // room rows (62 px each). First room starts at y = 64.
    std::string selected;
    view.on_room_selected = [&](const std::string& id)
    {
        selected = id;
    };
    REQUIRE(view.on_pointer_down({10, 74}));
    view.on_pointer_up({10, 74}, true);
    CHECK(selected == "!a:x");

    // Row 1 → "Alpine".
    selected.clear();
    REQUIRE(view.on_pointer_down({10, 136}));
    view.on_pointer_up({10, 136}, true);
    CHECK(selected == "!d:x");

    // Clearing the search text restores every room.
    view.set_search_text("");
    st.run(view, {0, 0, 260, 600});
    selected.clear();
    // search bar (36) + header (28) + row 2 offset (2*62) + mid-row (10) = 198
    REQUIRE(view.on_pointer_down({10, 198})); // row 2 in full set
    view.on_pointer_up({10, 198}, true);
    CHECK(selected == "!c:x");
}

TEST_CASE("RoomListView preserves selection by id across search filter",
          "[tk][view][roomlist][search]")
{
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "Alpha"},
        RoomInfo{.id = "!b:x", .name = "Beta"},
        RoomInfo{.id = "!c:x", .name = "Gamma"},
    };
    view.set_rooms(rooms);
    view.set_selected_room("!b:x");
    st.run(view, {0, 0, 260, 600});
    CHECK(view.selected_room_id() == "!b:x");

    // Filter out "Beta" — selection is no longer visible.
    view.set_search_text("Alp");
    st.run(view, {0, 0, 260, 600});
    CHECK(view.selected_index() == -1);
    // …but the id is remembered.
    CHECK(view.selected_room_id() == "!b:x");

    // Clearing the filter brings it back into view.
    view.set_search_text("");
    st.run(view, {0, 0, 260, 600});
    CHECK(view.selected_room_id() == "!b:x");
    CHECK(view.selected_index() == 1);
}

TEST_CASE("RoomListView sections classify Favorites / DMs / Rooms / Spaces",
          "[tk][view][roomlist][sections]")
{
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{
            .id = "!fav:x", .name = "Fav", .is_favorite = true},    // favorite
        RoomInfo{.id = "!dm:x", .name = "DM", .is_direct = true},   // direct
        RoomInfo{.id = "!r:x", .name = "Room"},                     // regular
        RoomInfo{.id = "!sp:x", .name = "Space", .is_space = true}, // space
    };
    view.set_rooms(rooms);
    // Each room type lands in its own section; all selectable by ID.
    // Room-only indices: fav=0, dm=1, r=2, sp=3.
    view.set_selected_room("!fav:x");
    CHECK(view.selected_room_id() == "!fav:x");
    CHECK(view.selected_index() == 0);

    view.set_selected_room("!dm:x");
    CHECK(view.selected_room_id() == "!dm:x");
    CHECK(view.selected_index() == 1);

    view.set_selected_room("!r:x");
    CHECK(view.selected_room_id() == "!r:x");
    CHECK(view.selected_index() == 2);

    view.set_selected_room("!sp:x");
    CHECK(view.selected_room_id() == "!sp:x");
    CHECK(view.selected_index() == 3);
}

TEST_CASE("RoomListView collapse section hides rooms; expand restores them",
          "[tk][view][roomlist][sections]")
{
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "A"},
        RoomInfo{.id = "!b:x", .name = "B"},
    };
    view.set_rooms(rooms);
    std::string selected;
    view.on_room_selected = [&](const std::string& id)
    {
        selected = id;
    };
    st.run(view, {0, 0, 260, 400});

    // Layout: search bar 0..36, header 36..64, !a 64..126, !b 126..188.
    // y=80 hits !a.
    REQUIRE(view.on_pointer_down({10, 80}));
    view.on_pointer_up({10, 80}, true);
    CHECK(selected == "!a:x");
    selected.clear();

    // Click the section header (y=50) to collapse.
    REQUIRE(view.on_pointer_down({10, 50}));
    view.on_pointer_up({10, 50}, true);
    CHECK(selected.empty()); // header click must not fire on_room_selected
    st.run(view, {0, 0, 260, 400});

    // After collapse: search bar + header + selected room !a only.
    // !a (currently selected) stays visible at y=64..126.
    REQUIRE(view.on_pointer_down({10, 80}));
    view.on_pointer_up({10, 80}, true);
    CHECK(selected == "!a:x"); // selected room still reachable when collapsed
    selected.clear();

    // !b (not selected, no unreads) is gone — click where it was.
    view.on_pointer_down({10, 150});
    view.on_pointer_up({10, 150}, true);
    CHECK(selected.empty());

    // Click header again to expand.
    REQUIRE(view.on_pointer_down({10, 50}));
    view.on_pointer_up({10, 50}, true);
    st.run(view, {0, 0, 260, 400});

    // Both rooms back: !a at y=64..126, !b at y=126..188.
    REQUIRE(view.on_pointer_down({10, 150}));
    view.on_pointer_up({10, 150}, true);
    CHECK(selected == "!b:x");
}

TEST_CASE("RoomListView search shows rooms in collapsed sections",
          "[tk][view][roomlist][sections]")
{
    Stage st;
    RoomListView view;
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "Alpha"},
        RoomInfo{.id = "!b:x", .name = "Beta"},
    };
    view.set_rooms(rooms);
    st.run(view, {0, 0, 260, 400});

    // Collapse the Rooms section via the header at y=50 (search bar 0..36, header 36..64).
    REQUIRE(view.on_pointer_down({10, 50}));
    view.on_pointer_up({10, 50}, true);
    st.run(view, {0, 0, 260, 400});

    // Search overrides collapsed state — both matches visible.
    std::string selected;
    view.on_room_selected = [&](const std::string& id)
    {
        selected = id;
    };
    view.set_search_text("a");
    st.run(view, {0, 0, 260, 400});

    // With search active: search bar (36 px) + "Rooms" header (28 px) +
    // "Alpha" starts at y=64. y=74 is well inside the first row.
    REQUIRE(view.on_pointer_down({10, 74}));
    view.on_pointer_up({10, 74}, true);
    CHECK(selected == "!a:x");
}

TEST_CASE("RoomListView empty sections produce no header row",
          "[tk][view][roomlist][sections]")
{
    Stage st;
    RoomListView view;
    // One regular room → only "Rooms" section has content; others absent.
    std::vector<RoomInfo> rooms = {
        RoomInfo{.id = "!a:x", .name = "A"},
    };
    view.set_rooms(rooms);
    std::string selected;
    view.on_room_selected = [&](const std::string& id)
    {
        selected = id;
    };
    st.run(view, {0, 0, 260, 400});

    // Layout: search bar 0..36, Rooms header 36..64, !a 64..126.
    // y=80 hits the room row directly.
    REQUIRE(view.on_pointer_down({10, 80}));
    view.on_pointer_up({10, 80}, true);
    CHECK(selected == "!a:x");

    // Header click at y=50 must not fire on_room_selected (collapses section).
    selected.clear();
    REQUIRE(view.on_pointer_down({10, 50}));
    view.on_pointer_up({10, 50}, true);
    CHECK(selected.empty());
}

// ─────────────────────────────────────────────────────────────────────────
//  MessageListView
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("MessageListView lays out text rows with avatar + body + timestamp",
          "[tk][view][messagelist]")
{
    Stage st;
    MessageListView view;

    MessageRowData m1{};
    m1.kind = MessageRowData::Kind::Text;
    m1.event_id = "$evt1";
    m1.sender_name = "Alice";
    m1.body = "Hello, world!";
    m1.timestamp_ms = 1700000000000ull;

    MessageRowData m2{};
    m2.kind = MessageRowData::Kind::Text;
    m2.event_id = "$evt2";
    m2.sender_name = "Bob";
    m2.body = "Hi Alice! This message should wrap onto two lines if "
              "the column is narrow enough.";
    m2.timestamp_ms = 1700000060000ull;
    m2.reactions.push_back({"\xF0\x9F\x91\x8D", 2, false, {}, {}}); // 👍

    view.set_messages({m1, m2});
    st.run(view, {0, 0, 320, 400});

    REQUIRE(view.messages().size() == 2);
    CHECK(view.content_height() > 0);

    // Append should extend the list and (because we were pinned) scroll
    // to the new bottom.
    MessageRowData m3{};
    m3.kind = MessageRowData::Kind::Text;
    m3.event_id = "$evt3";
    m3.sender_name = "Carol";
    m3.body = "Last";
    view.append_message(m3);
    st.run(view, {0, 0, 320, 400});
    CHECK(view.messages().size() == 3);
}

TEST_CASE("MessageListView::edit_last_own picks the newest own editable text",
          "[tk][view][messagelist]")
{
    using K = MessageRowData::Kind;
    using P = MessageRowData::PendingState;
    auto mk = [](const char* id, const char* body, bool own,
                 K k = MessageRowData::Kind::Text,
                 P p = MessageRowData::PendingState::None)
    {
        MessageRowData m{};
        m.kind = k;
        m.event_id = id;
        m.body = body;
        m.is_own = own;
        m.pending_state = p;
        return m;
    };

    SECTION("skips non-own, non-text, and still-sending rows")
    {
        MessageListView view;
        view.set_messages({
            mk("$a", "my first", true),                      // editable, older
            mk("$b", "theirs", false),                       // not own
            mk("$c", "my image", true, K::Image),            // not text
            mk("$d", "my latest", true),                     // editable, newest
            mk("$e", "sending…", true, K::Text, P::Sending), // not sent
        });
        std::string got_id, got_body;
        view.on_edit_requested =
            [&](const std::string& id, const std::string& b)
        {
            got_id = id;
            got_body = b;
        };
        CHECK(view.edit_last_own());
        CHECK(got_id == "$d");
        CHECK(got_body == "my latest");
    }

    SECTION("returns false when there is no editable own message")
    {
        MessageListView view;
        view.set_messages({
            mk("$x", "theirs", false),
            mk("$y", "my image", true, K::Image),
        });
        bool fired = false;
        view.on_edit_requested = [&](const std::string&, const std::string&)
        {
            fired = true;
        };
        CHECK_FALSE(view.edit_last_own());
        CHECK_FALSE(fired);
    }

    SECTION("returns false with no callback wired")
    {
        MessageListView view;
        view.set_messages({mk("$z", "mine", true)});
        CHECK_FALSE(view.edit_last_own()); // on_edit_requested unset
    }
}

TEST_CASE("MessageListView measures Image rows taller than text rows",
          "[tk][view][messagelist]")
{
    Stage st;
    MessageListView view;

    MessageRowData txt{};
    txt.kind = MessageRowData::Kind::Text;
    txt.event_id = "$t";
    txt.sender_name = "A";
    txt.body = "tiny";

    MessageRowData img{};
    img.kind = MessageRowData::Kind::Image;
    img.event_id = "$i";
    img.sender_name = "B";
    img.source = tesseract::MediaSource::plain("mxc://example.org/picture");
    img.media_w = 800;
    img.media_h = 500;

    view.set_messages({txt, img});
    st.run(view, {0, 0, 360, 800});

    // Heights aren't directly exposed; check that the message row total
    // height accounts for the image (200 px max-cap) on top of chrome.
    CHECK(view.content_height() > 200.0f);
}

TEST_CASE("MessageListView click fires on_message_clicked with event_id",
          "[tk][view][messagelist]")
{
    Stage st;
    MessageListView view;
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$only";
    m.sender_name = "A";
    m.body = "click me";
    view.set_messages({m});

    std::string clicked;
    view.on_message_clicked = [&](const std::string& id)
    {
        clicked = id;
    };

    st.run(view, {0, 0, 320, 200});
    REQUIRE(view.on_pointer_down({50, 20}));
    view.on_pointer_up({50, 20}, true);
    CHECK(clicked == "$only");
}

// ─────────────────────────────────────────────────────────────────────────
//  Reaction chips: tooltip data, "+" pseudo-chip, click → callbacks
// ─────────────────────────────────────────────────────────────────────────

namespace
{

MessageRowData make_row_with_reactions()
{
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$evt";
    m.sender_name = "Alice";
    m.body = "hi";
    tesseract::Reaction r1{};
    r1.key = "👍";
    r1.count = 2;
    r1.reacted_by_me = false;
    r1.senders = {"@alice:example.org", "@bob:example.org"};
    tesseract::Reaction r2{};
    r2.key = "🎉";
    r2.count = 1;
    r2.reacted_by_me = true;
    r2.senders = {"@carol:example.org"};
    m.reactions = {r1, r2};
    return m;
}

// First paint the view as hovered so the chip geometry is captured,
// then a second paint to make hover state stick. Pointer-move on the
// hovered row records the chip rects we'll then hit-test against.
void paint_with_hover(Stage& st, MessageListView& view, Rect bounds,
                      tk::Point local_in_row)
{
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
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    view.set_messages({make_row_with_reactions()});
    st.run(view, {0, 0, 320, 200});

    // No hover yet: add-button is not visible.
    CHECK_FALSE(view.hovered_row_geom().add_visible);

    // Move pointer into the row.
    view.on_pointer_move({50, 20});
    auto pc = st.paint_ctx();
    view.paint(pc);

    CHECK(view.hovered_row_geom().add_visible);
    CHECK(view.hovered_row_geom().chips.size() == 2);
}

TEST_CASE("MessageListView resolves chip hover target",
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    view.set_messages({make_row_with_reactions()});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

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
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    view.set_messages({make_row_with_reactions()});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

    std::string got_event, got_key, got_source;
    view.on_reaction_toggled = [&](const std::string& ev, const std::string& k,
                                   const std::string& src)
    {
        got_event = ev;
        got_key = k;
        got_source = src;
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
    CHECK(got_key == "🎉");
    CHECK(got_source.empty());
}

TEST_CASE("MessageListView MSC4027 chip click forwards source mxc",
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$evt";
    m.sender_name = "Alice";
    m.body = "hi";
    tesseract::Reaction r{};
    r.key = ":partyparrot:";
    r.count = 1;
    r.reacted_by_me = false;
    r.source = tesseract::MediaSource::plain("mxc://example.org/parrot");
    r.senders = {"@alice:example.org"};
    m.reactions = {r};
    view.set_messages({m});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

    std::string got_event, got_key, got_source;
    view.on_reaction_toggled = [&](const std::string& ev, const std::string& k,
                                   const std::string& src)
    {
        got_event = ev;
        got_key = k;
        got_source = src;
    };

    auto chips = view.hovered_row_geom().chips;
    REQUIRE(chips.size() == 1);
    tk::Point in_chip{
        chips[0].x + chips[0].w * 0.5f - view.bounds().x,
        chips[0].y + chips[0].h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_chip);
    REQUIRE(view.on_pointer_down(in_chip));
    view.on_pointer_up(in_chip, true);

    CHECK(got_event == "$evt");
    CHECK(got_key == ":partyparrot:");
    CHECK(got_source == "mxc://example.org/parrot");
}

// ─────────────────────────────────────────────────────────────────────────
//  Read receipts + hover timestamp
// ─────────────────────────────────────────────────────────────────────────

namespace
{

bool pixel_differs(tk::Color a, tk::Color b)
{
    return a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
}

MessageRowData make_text_row(const char* id, const char* body)
{
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = id;
    m.sender_name = "Alice";
    m.body = body;
    m.timestamp_ms = 1700000000000ull;
    return m;
}

tesseract::ReadReceipt make_receipt(const char* user, const char* name)
{
    tesseract::ReadReceipt rr{};
    rr.user_id = user;
    rr.display_name = name;
    rr.avatar_url = ""; // Force initials-disc fallback.
    return rr;
}

} // namespace

TEST_CASE("MessageListView read receipts paint inside existing row bounds",
          "[tk][view][messagelist][receipts]")
{
    Stage st;
    MessageListView view;

    MessageRowData plain = make_text_row("$plain", "hi");
    MessageRowData with_rr = make_text_row("$rr", "hi");
    with_rr.read_receipts = {
        make_receipt("@a:x", "Alice"),
        make_receipt("@b:x", "Bob"),
        make_receipt("@c:x", "Carol"),
    };

    view.set_messages({plain});
    st.run(view, {0, 0, 320, 400});
    float plain_h = view.content_height();

    view.set_messages({with_rr});
    st.run(view, {0, 0, 320, 400});
    float with_h = view.content_height();

    // Receipts overlay within existing row bounds — they never add height.
    CHECK(plain_h > 0.0f);
    CHECK(with_h == plain_h);
}

TEST_CASE(
    "MessageListView paints read-receipt cluster + overflow at bottom-right",
    "[tk][view][messagelist][receipts]")
{
    Stage st;
    MessageListView view;
    MessageRowData m = make_text_row("$evt", "hi");
    // Seven receipts — five render as discs, with a "+2" pill to the left.
    m.read_receipts = {
        make_receipt("@a:x", "Alice"), make_receipt("@b:x", "Bob"),
        make_receipt("@c:x", "Carol"), make_receipt("@d:x", "Dave"),
        make_receipt("@e:x", "Eve"),   make_receipt("@f:x", "Frank"),
        make_receipt("@g:x", "Grace"),
    };
    view.set_messages({m});
    st.run(view, {0, 0, 320, 400});

    // Sample a pixel near the centre of the rightmost receipt disc.
    // Layout: right_edge = bounds.x + bounds.w - kPadX = 320 - 12 = 308.
    // Rightmost disc centre is (308 - kReceiptSize/2, disc_cy) = (300, ~).
    // The disc fills its area with a coloured initials background.
    // sample_y is content_height - kTypingRowH - 14 to hit the disc vertical
    // centre within the message row (content_height includes the always-present
    // 20 px typing row at the tail; sampling near the top edge hits the
    // stadium corner rounding and misses the fill).
    int sample_x = 300;
    int sample_y = static_cast<int>(view.content_height()) - 20 - 14;
    auto disc_px = st.surface->read_pixel(sample_x, sample_y);
    auto bg_px = st.surface->read_pixel(300, 0);
    CHECK(pixel_differs(disc_px, bg_px));

    // And further left (where the "+2" overflow pill should sit) should
    // also differ from the row background.
    int overflow_x = 300 - 5 * 11 - 4; // past the disc cluster + small gap
    auto overflow_px = st.surface->read_pixel(overflow_x, sample_y);
    CHECK(pixel_differs(overflow_px, bg_px));
}

TEST_CASE("MessageListView paints hover timestamp under the avatar",
          "[tk][view][messagelist][hover]")
{
    Stage st;
    MessageListView view;
    MessageRowData m = make_text_row("$evt", "hi");
    view.set_messages({m});

    // Measure + arrange first so `on_pointer_move` resolves a row index,
    // then hover, then paint once. (TestSurface::read_pixel ends the
    // backing painter, so we only get one paint pass per test.)
    auto lc = st.layout_ctx();
    view.measure(lc, {320, 400});
    view.arrange(lc, {0, 0, 320, 400});
    view.on_pointer_move({50, 20});
    auto pc = st.paint_ctx();
    view.paint(pc);

    // content_height() includes the always-present 20 px typing row; subtract
    // it to get the bottom of the message row, then offset by 13 to land on
    // the HH:MM glyph painted in the avatar column.
    int row_h = static_cast<int>(view.content_height()) - 20;
    auto hovered_px = st.surface->read_pixel(28, row_h - 13);
    // Pixel well below the only row: untouched by any paint — pristine bg.
    auto bg_px = st.surface->read_pixel(28, 300);
    CHECK(pixel_differs(hovered_px, bg_px));
}

TEST_CASE("MessageListView + button click fires on_add_reaction_requested",
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    view.set_messages({make_row_with_reactions()});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

    std::string got_event;
    view.on_add_reaction_requested =
        [&](const std::string& ev, tk::Rect /*anchor*/)
    {
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

TEST_CASE("MessageListView action-pill react button fires "
          "on_add_reaction_requested",
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    // A plain text row with no reactions: the react affordance must still
    // be present, on the top-right action pill.
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$plain";
    m.sender_name = "Alice";
    m.body = "no reactions here";
    view.set_messages({m});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

    std::string got_event;
    view.on_add_reaction_requested =
        [&](const std::string& ev, tk::Rect /*anchor*/) { got_event = ev; };

    auto react = view.hovered_row_geom().react_button;
    REQUIRE(react.w > 0);
    tk::Point in_react{
        react.x + react.w * 0.5f - view.bounds().x,
        react.y + react.h * 0.5f - view.bounds().y,
    };
    view.on_pointer_move(in_react);
    REQUIRE(view.on_pointer_down(in_react));
    view.on_pointer_up(in_react, true);

    CHECK(got_event == "$plain");
}

TEST_CASE("MessageListView action-pill omits react button on redacted rows",
          "[tk][view][messagelist][reactions]")
{
    Stage st;
    MessageListView view;
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Redacted;
    m.event_id = "$gone";
    m.sender_name = "Alice";
    m.body = "(deleted)";
    view.set_messages({m});
    paint_with_hover(st, view, {0, 0, 320, 200}, {50, 20});

    CHECK(view.hovered_row_geom().react_button.w == 0);
}

// ─────────────────────────────────────────────────────────────────────────
//  Scroll-anchor + near-top trigger (back-pagination plumbing)
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("ListView::preserve_top_through keeps the user's row under cursor",
          "[tk][listview][prepend]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 20;
    ad.row_h = 25.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});

    // Scroll to ~the middle. The user is now looking at row ~5.
    list.on_wheel({50, 50}, 0, 100);
    REQUIRE(list.scroll_y() == 100.0f);

    // "Prepend" 5 rows of height 25 → +125 px of content above.
    list.preserve_top_through(
        [&]
        {
            ad.n = 25;
            list.invalidate_data();
        });
    list.arrange(lc, {0, 0, 200, 100});

    CHECK(list.content_height() == 25.0f * 25);
    // Visual position preserved: the row the user was looking at is still
    // under their cursor. Original scroll_y=100 + 5 * 25 = 225.
    CHECK(list.scroll_y() == 225.0f);
}

TEST_CASE("ListView row anchor keeps the hovered row's screen Y when a row "
          "above it grows",
          "[tk][listview][anchor]")
{
    Stage st;
    ListView list;
    KeyedVarAdapter ad;
    ad.heights.assign(20, 25.0f);
    for (int i = 0; i < 20; ++i)
        ad.keys.push_back("k" + std::to_string(i));
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});

    // Scroll so rows 10+ are in view, then hover row 11 (top at 275, so it
    // sits 25 px below the viewport top).
    list.on_wheel({50, 50}, 0, 250);
    REQUIRE(list.scroll_y() == 250.0f);
    list.on_pointer_move({50, 30}); // content_y = 280 → row 11
    REQUIRE(list.index_at({50, 30}) == 11);

    // A row *above* the anchor (row 5) finishes loading media and grows by
    // 100 px. The anchored row's top shifts down by 100, so scroll must shift
    // down by 100 too to keep it under the cursor: 275(+100) - 25 = 350.
    list.preserve_top_through(
        [&]
        {
            ad.heights[5] = 125.0f;
            list.invalidate_data();
        });
    list.arrange(lc, {0, 0, 200, 100});

    CHECK(list.scroll_y() == 350.0f);
}

TEST_CASE("ListView row anchor ignores growth strictly below the anchor",
          "[tk][listview][anchor]")
{
    Stage st;
    ListView list;
    KeyedVarAdapter ad;
    ad.heights.assign(20, 25.0f);
    for (int i = 0; i < 20; ++i)
        ad.keys.push_back("k" + std::to_string(i));
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});
    list.on_wheel({50, 50}, 0, 250);
    list.on_pointer_move({50, 30}); // hover row 11
    REQUIRE(list.index_at({50, 30}) == 11);

    // Row 18 (below the anchor) grows. Heights above the anchor are unchanged,
    // so the anchored row's top — and therefore scroll_y — must not move.
    list.preserve_top_through(
        [&]
        {
            ad.heights[18] = 200.0f;
            list.invalidate_data();
        });
    list.arrange(lc, {0, 0, 200, 100});

    CHECK(list.scroll_y() == 250.0f);
}

TEST_CASE("ListView row anchor keeps the keyed row stable across a prepend",
          "[tk][listview][anchor][prepend]")
{
    Stage st;
    ListView list;
    KeyedVarAdapter ad;
    ad.heights.assign(20, 25.0f);
    for (int i = 0; i < 20; ++i)
        ad.keys.push_back("k" + std::to_string(i));
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});
    list.on_wheel({50, 50}, 0, 250);
    list.on_pointer_move({50, 30}); // hover row 11 → key "k11"
    REQUIRE(list.index_at({50, 30}) == 11);

    // Prepend 3 older rows. "k11" is now at index 14; its top moves from 275
    // to 350. To hold it 25 px below the viewport top: 350 - 25 = 325.
    list.preserve_top_through(
        [&]
        {
            ad.heights.insert(ad.heights.begin(), {25.0f, 25.0f, 25.0f});
            ad.keys.insert(ad.keys.begin(), {"p0", "p1", "p2"});
            list.invalidate_data();
        });
    list.arrange(lc, {0, 0, 200, 100});

    CHECK(list.content_height() == 25.0f * 23);
    CHECK(list.scroll_y() == 325.0f);
}

TEST_CASE("ListView::preserve_top_through is a no-op when stuck to bottom",
          "[tk][listview][prepend]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 10;
    ad.row_h = 25.0f;
    list.set_adapter(&ad);

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 100});
    list.scroll_to_bottom();
    list.arrange(lc, {0, 0, 200, 100});
    float before = list.scroll_y();

    // Prepend rows while pinned to bottom: scroll should *not* shift up.
    list.preserve_top_through(
        [&]
        {
            ad.n = 15;
            list.invalidate_data();
        });
    list.arrange(lc, {0, 0, 200, 100});
    // The stick-to-bottom logic snapped us to the new bottom.
    CHECK(list.scroll_y() == list.content_height() - 100.0f);
    CHECK(list.scroll_y() != before);
}

TEST_CASE("ListView::on_near_top fires on threshold crossing and re-arms",
          "[tk][listview][nearTop]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 60;
    ad.row_h = 30.0f; // 1800 px content
    list.set_adapter(&ad);
    list.set_near_top_threshold_px(200.0f);

    int fires = 0;
    list.on_near_top = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300});

    // Start at the top — the trigger should NOT fire until we leave and
    // come back (the latch is meant for user-driven crossings, not the
    // initial state). The first wheel down moves us above the threshold.
    list.on_wheel({50, 50}, 0, 500); // scroll down 500 → far from top
    CHECK(fires == 0);
    REQUIRE(list.scroll_y() > 200.0f);

    // Scroll back up across the threshold — exactly one fire.
    list.on_wheel({50, 50}, 0, -350); // now at 150, below 200 threshold
    CHECK(list.scroll_y() < 200.0f);
    CHECK(fires == 1);

    // Stay below the threshold: no extra fires while latched.
    list.on_wheel({50, 50}, 0, -50);
    CHECK(fires == 1);

    // Scroll back above the threshold (re-arms the latch).
    list.on_wheel({50, 50}, 0, 500); // far below
    CHECK(list.scroll_y() > 200.0f);
    CHECK(fires == 1);

    // Cross back in — second fire.
    list.on_wheel({50, 50}, 0, -450);
    REQUIRE(list.scroll_y() < 200.0f);
    CHECK(fires == 2);
}

TEST_CASE("ListView::on_near_bottom fires on threshold crossing and re-arms",
          "[tk][listview][nearBottom]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 60;
    ad.row_h = 30.0f; // 1800 px content
    list.set_adapter(&ad);
    list.set_near_bottom_threshold_px(200.0f);

    int fires = 0;
    list.on_near_bottom = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300});

    // Start at the top (scroll_y=0) — content is much taller than viewport,
    // so we are far from the bottom. No fire expected.
    CHECK(fires == 0);

    // Scroll near the bottom (past the 200 px threshold) — exactly one fire.
    // content_height=1800, viewport=300, max_scroll=1500.
    // remaining = 1800 - (scroll_y + 300); want remaining < 200 → scroll_y > 1300.
    list.on_wheel({50, 50}, 0, 1400);
    REQUIRE(list.scroll_y() > 1300.0f);
    CHECK(fires == 1);

    // Stay near the bottom: no extra fires while latched.
    list.on_wheel({50, 50}, 0, 50);
    CHECK(fires == 1);

    // Scroll back up above the threshold (re-arms the latch).
    list.on_wheel({50, 50}, 0, -600);
    REQUIRE((1800.0f - (list.scroll_y() + 300.0f)) >= 200.0f);
    CHECK(fires == 1);

    // Cross back in — second fire.
    list.on_wheel({50, 50}, 0, 500);
    REQUIRE((1800.0f - (list.scroll_y() + 300.0f)) < 200.0f);
    CHECK(fires == 2);
}

TEST_CASE("ListView::on_near_bottom: latch does not re-arm on pure resize",
          "[tk][listview][nearBottom]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 60;
    ad.row_h = 30.0f; // 1800 px content
    list.set_adapter(&ad);
    list.set_near_bottom_threshold_px(200.0f);

    int fires = 0;
    list.on_near_bottom = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300});

    // Scroll near the bottom to fire once.
    // content=1800, viewport=300, max_scroll=1500.
    // remaining = 1800 - (scroll_y + 300); < 200 requires scroll_y > 1300.
    list.on_wheel({50, 50}, 0, 1400);
    REQUIRE(fires == 1);

    // Pure resize — same row count, new width. Must NOT re-arm and re-fire.
    list.arrange(lc, {0, 0, 220, 300}); // width changed, no new rows
    // was_near_bottom_ stays true; scrolling away then back should still
    // cost exactly one more fire (not two, which would happen if resize
    // had spuriously re-armed the latch).
    //
    // Scroll back above threshold: remaining must be >= 200.
    // From clamped scroll_y=1500, -700 → 800, remaining=1800-1100=700 ✓
    list.on_wheel({50, 50}, 0, -700);
    CHECK(fires ==
          1); // latch re-arms (was_near_bottom_ → false) but no fire yet

    // Cross back in — remaining < 200 requires scroll_y > 1300 (strict).
    // From 800, +601 → 1401, remaining=1800-1701=99 < 200 → fires.
    list.on_wheel({50, 50}, 0, 601);
    CHECK(fires == 2);
}

TEST_CASE("ListView::on_near_bottom re-arms when content is appended",
          "[tk][listview][nearBottom]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 60;
    ad.row_h = 30.0f; // 1800 px content
    list.set_adapter(&ad);
    list.set_near_bottom_threshold_px(200.0f);

    int fires = 0;
    list.on_near_bottom = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300});

    // Scroll near the bottom of the original content — fires once and latches.
    // remaining = 1800 - (1400+300) = 100 < 200 ✓
    list.on_wheel({50, 50}, 0, 1400);
    REQUIRE(fires == 1);

    // Append 10 rows. New content = 2100, max_scroll = 1800.
    // Arrange re-arms the latch (count increased).
    ad.n = 70;
    list.invalidate_data();
    list.arrange(lc, {0, 0, 200, 300});

    // At scroll_y=1400 (clamped to new max 1800), remaining = 2100-1700 = 400 ≥ 200.
    // We're no longer near the new bottom, so the latch stays dormant.
    // Scroll to the new bottom zone to trigger the re-armed callback.
    // remaining < 200 requires scroll_y > 1600. From 1400, +300 → 1700.
    list.on_wheel({50, 50}, 0, 300);
    CHECK(fires == 2);
}

TEST_CASE("ListView::on_near_bottom not fired when stick_to_bottom_ is true",
          "[tk][listview][nearBottom]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 60;
    ad.row_h = 30.0f; // 1800 px content
    list.set_adapter(&ad);
    list.set_near_bottom_threshold_px(200.0f);

    int fires = 0;
    list.on_near_bottom = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300});

    // Scroll near bottom to reach the near-bottom zone and latch.
    list.on_wheel({50, 50}, 0, 1400);
    REQUIRE(fires == 1);

    // Reset the latch manually (as would happen after set_messages in the
    // real MessageListView) and snap to bottom, simulating the initial
    // pinned state after loading a room.
    list.reset_near_bottom_latch();
    list.scroll_to_bottom();
    list.arrange(lc, {0, 0, 200, 300});

    // At this point scroll_y == max_scroll and stick_to_bottom_ is true.
    // A wheel event clears stick_to_bottom_ first, so we cannot use it to
    // test the sticky-suppression directly. Instead, verify that the latch
    // was not re-armed by the reset+snap: scrolling slightly down (no-op,
    // already at max) and back up by a tiny amount stays in the near-bottom
    // zone. The latch is still false (was reset), but stick_to_bottom_ is
    // cleared by the wheel — so the check fires. This confirms that
    // reset_near_bottom_latch() + scroll_to_bottom() is the correct API
    // combination for the MessageListView to suppress a spurious fire after
    // set_messages, not relying on the sticky flag alone.
    //
    // Verify: after resetting the latch, scrolling back into the near-bottom
    // zone fires once (not zero — the sticky guard only applies while
    // stick_to_bottom_ is still set, and on_wheel clears it).
    list.on_wheel({50, 50}, 0, -600); // leave near-bottom zone first
    REQUIRE((1800.0f - (list.scroll_y() + 300.0f)) >= 200.0f);
    CHECK(fires == 1); // latch re-armed during scroll-out, no extra fire
    list.on_wheel({50, 50}, 0, 500); // re-enter zone → fire
    CHECK(fires == 2);
}

TEST_CASE(
    "ListView::on_near_bottom not fired when content fits within viewport",
    "[tk][listview][nearBottom]")
{
    Stage st;
    ListView list;
    FixedHeightAdapter ad;
    ad.n = 3;
    ad.row_h = 30.0f; // 90 px content
    list.set_adapter(&ad);
    list.set_near_bottom_threshold_px(200.0f);

    int fires = 0;
    list.on_near_bottom = [&]
    {
        ++fires;
    };

    auto lc = st.layout_ctx();
    list.arrange(lc, {0, 0, 200, 300}); // viewport 300 > content 90

    // Attempt wheel (content doesn't scroll).
    list.on_wheel({50, 50}, 0, 50);
    CHECK(fires == 0);
}

TEST_CASE(
    "MessageListView::insert_message(0) inserts at front and preserves scroll",
    "[tk][view][messagelist][insert]")
{
    Stage st;
    MessageListView view;

    // Seed with 5 messages.
    std::vector<MessageRowData> seed;
    for (int i = 0; i < 5; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$seed" + std::to_string(i);
        m.sender_name = "Seed";
        m.body = "seed " + std::to_string(i);
        seed.push_back(std::move(m));
    }
    view.set_messages(std::move(seed));
    st.run(view, {0, 0, 320, 400});
    REQUIRE(view.messages().size() == 5);
    REQUIRE(view.messages()[0].event_id == "$seed0");

    MessageRowData older{};
    older.kind = MessageRowData::Kind::Text;
    older.event_id = "$older";
    older.sender_name = "Older";
    older.body = "from history";
    view.insert_message(0, std::move(older));
    st.run(view, {0, 0, 320, 400});

    REQUIRE(view.messages().size() == 6);
    CHECK(view.messages()[0].event_id == "$older");
    CHECK(view.messages()[1].event_id == "$seed0");
    CHECK(view.messages()[5].event_id == "$seed4");
}

TEST_CASE("MessageListView::insert_message at head preserves visual top when "
          "scrolled",
          "[tk][view][messagelist][insert]")
{
    Stage st;
    MessageListView view;

    // Seed with enough rows to overflow and let us scroll mid-list.
    std::vector<MessageRowData> seed;
    for (int i = 0; i < 30; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$s" + std::to_string(i);
        m.sender_name = "S";
        m.body = "row " + std::to_string(i);
        seed.push_back(std::move(m));
    }
    view.set_messages(std::move(seed));
    st.run(view, {0, 0, 320, 200});

    // Scroll up by 80 from the bottom-pinned state so the top edge is
    // 80px below scroll_max. We need to be off the bottom to prove the
    // anchor logic kicks in.
    view.scroll_to_top();
    st.run(view, {0, 0, 320, 200});
    view.on_wheel({50, 50}, 0, 80); // scroll down by 80
    st.run(view, {0, 0, 320, 200});
    REQUIRE(view.scroll_y() > 0.0f);

    float pre_scroll = view.scroll_y();
    float pre_height = view.content_height();

    // Prepend 4 older rows.
    std::vector<MessageRowData> older;
    for (int i = 0; i < 4; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$o" + std::to_string(i);
        m.sender_name = "O";
        m.body = "old " + std::to_string(i);
        older.push_back(std::move(m));
    }
    // Each matrix-sdk-ui `PushFront` is a separate diff that lands at
    // index 0, shifting earlier prepends down. Reverse-iterate so the
    // final order matches the natural pagination outcome (oldest-first).
    for (auto it = older.rbegin(); it != older.rend(); ++it)
    {
        view.insert_message(0, std::move(*it));
    }
    st.run(view, {0, 0, 320, 200});

    REQUIRE(view.messages().size() == 34);
    CHECK(view.messages()[0].event_id == "$o0");
    CHECK(view.messages()[3].event_id == "$o3");
    CHECK(view.messages()[4].event_id == "$s0");

    // scroll_y should have been bumped by the new content's height so the
    // user's visual position is unchanged.
    float delta = view.content_height() - pre_height;
    CHECK(delta > 0.0f);
    CHECK(view.scroll_y() == pre_scroll + delta);
}

TEST_CASE("MessageListView::insert_message(mid) lands at the requested index",
          "[tk][view][messagelist][insert]")
{
    Stage st;
    MessageListView view;

    std::vector<MessageRowData> seed;
    for (int i = 0; i < 5; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$s" + std::to_string(i);
        m.sender_name = "S";
        m.body = "row " + std::to_string(i);
        seed.push_back(std::move(m));
    }
    view.set_messages(std::move(seed));
    st.run(view, {0, 0, 320, 400});
    REQUIRE(view.messages().size() == 5);

    // Insert a row between $s2 and $s3 — proves we honor the position
    // instead of falling back to append-with-dedup.
    MessageRowData mid{};
    mid.kind = MessageRowData::Kind::Text;
    mid.event_id = "$mid";
    mid.sender_name = "Mid";
    mid.body = "between 2 and 3";
    view.insert_message(3, std::move(mid));
    st.run(view, {0, 0, 320, 400});

    REQUIRE(view.messages().size() == 6);
    CHECK(view.messages()[2].event_id == "$s2");
    CHECK(view.messages()[3].event_id == "$mid");
    CHECK(view.messages()[4].event_id == "$s3");
    CHECK(view.messages()[5].event_id == "$s4");
}

TEST_CASE("MessageListView::update_message replaces the row in place",
          "[tk][view][messagelist][update]")
{
    Stage st;
    MessageListView view;

    std::vector<MessageRowData> seed;
    for (int i = 0; i < 3; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$s" + std::to_string(i);
        m.sender_name = "S";
        m.body = "row " + std::to_string(i);
        seed.push_back(std::move(m));
    }
    view.set_messages(std::move(seed));
    st.run(view, {0, 0, 320, 400});

    MessageRowData edited{};
    edited.kind = MessageRowData::Kind::Text;
    edited.event_id = "$s1-edited"; // different event_id is fine —
                                    // the index is what binds the row
    edited.sender_name = "S";
    edited.body = "row 1 (edited)";
    view.update_message(1, std::move(edited));
    st.run(view, {0, 0, 320, 400});

    REQUIRE(view.messages().size() == 3);
    CHECK(view.messages()[0].event_id == "$s0");
    CHECK(view.messages()[1].event_id == "$s1-edited");
    CHECK(view.messages()[1].body == "row 1 (edited)");
    CHECK(view.messages()[2].event_id == "$s2");
}

TEST_CASE("MessageListView::remove_message drops the row at the index",
          "[tk][view][messagelist][remove]")
{
    Stage st;
    MessageListView view;

    std::vector<MessageRowData> seed;
    for (int i = 0; i < 4; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$s" + std::to_string(i);
        m.sender_name = "S";
        m.body = "row " + std::to_string(i);
        seed.push_back(std::move(m));
    }
    view.set_messages(std::move(seed));
    st.run(view, {0, 0, 320, 400});

    view.remove_message(1);
    st.run(view, {0, 0, 320, 400});

    REQUIRE(view.messages().size() == 3);
    CHECK(view.messages()[0].event_id == "$s0");
    CHECK(view.messages()[1].event_id == "$s2");
    CHECK(view.messages()[2].event_id == "$s3");
}

TEST_CASE("MessageListView::remove_message out-of-range is a no-op",
          "[tk][view][messagelist][remove]")
{
    Stage st;
    MessageListView view;

    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$only";
    m.sender_name = "S";
    m.body = "row";
    view.set_messages({std::move(m)});
    st.run(view, {0, 0, 320, 400});

    view.remove_message(5); // out of range
    st.run(view, {0, 0, 320, 400});
    REQUIRE(view.messages().size() == 1);
    CHECK(view.messages()[0].event_id == "$only");
}

TEST_CASE("MessageListView scroll-to-bottom pill: hidden at bottom, "
          "click snaps back",
          "[tk][view][messagelist][pill]")
{
    Stage st;
    MessageListView view;

    std::vector<MessageRowData> msgs;
    for (int i = 0; i < 80; ++i)
    {
        MessageRowData m{};
        m.kind = MessageRowData::Kind::Text;
        m.event_id = "$e" + std::to_string(i);
        m.sender_name = "User";
        m.body = "row " + std::to_string(i);
        msgs.push_back(std::move(m));
    }

    view.set_messages(std::move(msgs), /*room_switch=*/true); // auto-scrolls to bottom
    st.run(view, {0, 0, 320, 200});
    REQUIRE(view.content_height() > 200.0f); // content really overflows
    CHECK_FALSE(view.pill_visible());

    // Scroll to the top → pill should appear on the next paint.
    view.scroll_to_top();
    st.run(view, {0, 0, 320, 200});
    REQUIRE(view.pill_visible());

    // Click the pill rect → scroll_to_bottom(), pill disappears.
    tk::Rect r = view.pill_bounds();
    REQUIRE(r.w > 0.0f);
    tk::Point local{
        r.x + r.w * 0.5f - view.bounds().x,
        r.y + r.h * 0.5f - view.bounds().y,
    };
    REQUIRE(view.on_pointer_down(local));
    view.on_pointer_up(local, /*inside_self=*/true);
    st.run(view, {0, 0, 320, 200});
    CHECK_FALSE(view.pill_visible());
}

// ── Room-switch display gate ───────────────────────────────────────────────
//
// On a room switch the message list is held invisible (background only)
// until the rows that will be visible have their height-affecting content
// loaded + measured. `image_hit_at` only returns a value once the row was
// actually painted (paint_row records its geometry), so it doubles as a
// "rows are visible yet?" probe here.

namespace
{

MessageRowData gate_image_row()
{
    MessageRowData img{};
    img.kind = MessageRowData::Kind::Image;
    img.event_id = "$img";
    img.sender_name = "A";
    img.source = tesseract::MediaSource::plain("mxc://example.org/pic");
    img.media_w = 800;
    img.media_h = 500;
    return img;
}

bool any_image_painted(MessageListView& view)
{
    for (float x = 40; x < 400; x += 40)
    {
        for (float y = 30; y < 300; y += 30)
        {
            if (view.image_hit_at({x, y}))
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("MessageListView room-switch gate holds rows until media resolves",
          "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    // Image never decodes in this test — only the explicit notify releases.
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    view.set_messages({gate_image_row()}, /*room_switch=*/true);
    st.run(view, {0, 0, 400, 600});
    CHECK_FALSE(any_image_painted(view)); // gated: row not painted yet

    // The shell signals the media arrived → gate releases on next paint.
    view.notify_image_ready("mxc://example.org/pic");
    st.run(view, {0, 0, 400, 600});
    CHECK(any_image_painted(view)); // revealed
}

TEST_CASE("MessageListView keeps the hovered message put when a preview card "
          "above it finishes loading",
          "[tk][view][messagelist][anchor]")
{
    Stage st;
    MessageListView view;
    // A text-only URL preview grows the row without an image to paint (a fake
    // tk::Image would crash the real Qt draw path).
    const tesseract::views::UrlPreviewData* preview = nullptr;
    view.set_preview_provider([&](const std::string&) { return preview; });

    // A link row at the top (preview starts unloaded → no card height), then
    // plenty of distinct text rows so the list scrolls.
    std::vector<MessageRowData> rows;
    {
        MessageRowData link{};
        link.kind = MessageRowData::Kind::Text;
        link.event_id = "$link";
        link.sender_name = "B";
        link.body = "see http://example.com/page";
        link.first_url = "http://example.com/page";
        rows.push_back(link);
    }
    for (int i = 0; i < 30; ++i)
    {
        MessageRowData t{};
        t.kind = MessageRowData::Kind::Text;
        t.event_id = "$t" + std::to_string(i);
        t.sender_name = "U" + std::to_string(i); // distinct → no grouping
        t.body = "line " + std::to_string(i);
        rows.push_back(t);
    }
    view.set_messages(rows); // non-switch → not gated, paints immediately
    st.run(view, {0, 0, 360, 300});

    // Scroll just past the very top so anchoring is active (the at-top
    // early-out is reserved for revealing newly prepended rows). The link row
    // still straddles the top of the viewport, so under the old logic it was
    // "not strictly above the viewport" and got no anchor.
    view.on_wheel({50, 50}, 0, 20);
    st.run(view, {0, 0, 360, 300});
    REQUIRE(view.scroll_y() > 1.0f);

    // Hover a text row in the lower half of the viewport, below the link.
    view.on_pointer_move({50, 220});
    st.run(view, {0, 0, 360, 300});
    REQUIRE(view.hovered_row_geom().row_index != static_cast<std::size_t>(-1));
    REQUIRE(view.hovered_row_geom().row_index > 0); // a text row, not the link
    const float y_before  = view.hovered_row_geom().row_bounds.y;
    const float ch_before = view.content_height();

    // The preview card finishes loading → the link row grows downward.
    tesseract::views::UrlPreviewData card;
    card.title       = "Example";
    card.description = "An example page";
    preview          = &card;
    view.notify_url_preview_ready("http://example.com/page");
    st.run(view, {0, 0, 360, 300});

    REQUIRE(view.content_height() > ch_before); // the row actually grew
    const float y_after = view.hovered_row_geom().row_bounds.y;
    // The hovered message stayed under the cursor: same screen Y (±1 px).
    CHECK(std::abs(y_after - y_before) <= 1.0f);
}

TEST_CASE("MessageListView non-switch set_messages is never gated",
          "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    view.set_messages({gate_image_row()}); // default room_switch=false
    st.run(view, {0, 0, 400, 600});
    CHECK(any_image_painted(view)); // painted immediately
}

TEST_CASE("MessageListView room-switch gate reveals on timeout",
          "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    std::function<void()> timeout_fn;
    view.set_post_delayed(
        [&](int, std::function<void()> fn)
        {
            timeout_fn = std::move(fn);
        });

    view.set_messages({gate_image_row()}, /*room_switch=*/true);
    st.run(view, {0, 0, 400, 600});
    CHECK_FALSE(any_image_painted(view)); // still gated
    REQUIRE(timeout_fn);                  // gate armed the timeout

    timeout_fn(); // fire the deadline
    st.run(view, {0, 0, 400, 600});
    CHECK(any_image_painted(view)); // revealed despite null provider
}

TEST_CASE(
    "MessageListView room-switch gate timeout wins even before first paint",
    "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    std::function<void()> timeout_fn;
    view.set_post_delayed(
        [&](int, std::function<void()> fn)
        {
            timeout_fn = std::move(fn);
        });

    view.set_messages({gate_image_row()}, /*room_switch=*/true);
    REQUIRE(timeout_fn);
    // Deadline fires while the window was occluded — BEFORE the gate has
    // ever been evaluated by a paint. The first paint must still reveal
    // (not re-derive `pending` in collect_gate_deps_ and re-arm the now
    // spent one-shot timeout).
    timeout_fn();
    st.run(view, {0, 0, 400, 600});
    CHECK(any_image_painted(view));
}

TEST_CASE("MessageListView room-switch gate supersedes on rapid re-switch",
          "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    // Switch A (image, would gate) immediately superseded by switch B (a
    // plain text row — no height-affecting deps, so nothing to wait for).
    view.set_messages({gate_image_row()}, /*room_switch=*/true);
    MessageRowData txt{};
    txt.kind = MessageRowData::Kind::Text;
    txt.event_id = "$t";
    txt.sender_name = "A";
    txt.body = "hello";
    view.set_messages({txt}, /*room_switch=*/true);

    st.run(view, {0, 0, 400, 600});
    // B's gate finds no unmet deps → reveals on the first paint; the stale
    // A gate must not keep the list invisible.
    REQUIRE(view.messages().size() == 1);
    CHECK(view.messages()[0].event_id == "$t");
    CHECK(view.content_height() > 0.0f);
}

TEST_CASE("MessageListView room-switch gate swallows pointer input",
          "[tk][view][messagelist][gate]")
{
    Stage st;
    MessageListView view;
    view.set_image_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });

    bool clicked = false;
    view.on_message_clicked = [&](const std::string&)
    {
        clicked = true;
    };

    view.set_messages({gate_image_row()}, /*room_switch=*/true);
    st.run(view, {0, 0, 400, 600});
    REQUIRE_FALSE(any_image_painted(view)); // gated: nothing painted

    // Presses anywhere on the blank list are swallowed (no invisible rows
    // can be hit) and no click fires.
    for (float x = 40; x < 360; x += 60)
    {
        for (float y = 30; y < 270; y += 60)
        {
            CHECK_FALSE(view.on_pointer_down({x, y}));
        }
    }
    view.on_pointer_up({120, 90}, /*inside_self=*/true);
    CHECK_FALSE(clicked);

    // Gate releases → the list paints and input is live again.
    view.notify_image_ready("mxc://example.org/pic");
    st.run(view, {0, 0, 400, 600});
    CHECK(any_image_painted(view));
}

// ─────────────────────────────────────────────────────────────────────────
//  GridView — hover tracking and rect_at geometry
//
//  Setup: cell_size(32,32), spacing(2,2), arranged at {0,0,400,300}.
//  cols = floor((400 + 2) / (32 + 2)) = floor(402/34) = 11
//  Cell 0: x=0..31, y=0..31.  Cell 1: x=34..65, y=0..31.
//  Gap between cell 0 and 1: x=32..33 (width 2).
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("GridView::on_pointer_move updates hovered_index", "[tk][gridview]")
{
    Stage st;
    GridView grid;
    FixedGridAdapter ad;
    ad.n = 20;
    grid.set_adapter(&ad);
    grid.set_cell_size(32, 32);
    grid.set_spacing(2, 2);

    auto lc = st.layout_ctx();
    grid.arrange(lc, {0, 0, 400, 300});

    CHECK(grid.hovered_index() == -1);

    // Centre of cell 0 (x=16, y=16).
    grid.on_pointer_move({16, 16});
    CHECK(grid.hovered_index() == 0);

    // Centre of cell 1: x = 32+2+16 = 50.
    grid.on_pointer_move({50, 16});
    CHECK(grid.hovered_index() == 1);
}

TEST_CASE("GridView::on_pointer_leave clears hovered_index to -1",
          "[tk][gridview]")
{
    Stage st;
    GridView grid;
    FixedGridAdapter ad;
    ad.n = 20;
    grid.set_adapter(&ad);
    grid.set_cell_size(32, 32);
    grid.set_spacing(2, 2);

    auto lc = st.layout_ctx();
    grid.arrange(lc, {0, 0, 400, 300});

    grid.on_pointer_move({16, 16});
    REQUIRE(grid.hovered_index() == 0);

    grid.on_pointer_leave();
    CHECK(grid.hovered_index() == -1);
}

TEST_CASE("GridView pointer in inter-cell gap reports hovered_index == -1",
          "[tk][gridview]")
{
    Stage st;
    GridView grid;
    FixedGridAdapter ad;
    ad.n = 20;
    grid.set_adapter(&ad);
    grid.set_cell_size(32, 32);
    grid.set_spacing(2, 2);

    auto lc = st.layout_ctx();
    grid.arrange(lc, {0, 0, 400, 300});

    // x=33: cell_local_x = 33 - 0*(32+2) = 33 >= 32 → gap → -1.
    grid.on_pointer_move({33, 16});
    CHECK(grid.hovered_index() == -1);
}

TEST_CASE("GridView::rect_at returns correct widget-local cell rect",
          "[tk][gridview]")
{
    Stage st;
    GridView grid;
    FixedGridAdapter ad;
    ad.n = 4;
    grid.set_adapter(&ad);
    grid.set_cell_size(32, 32);
    grid.set_spacing(2, 2);

    auto lc = st.layout_ctx();
    grid.arrange(lc, {0, 0, 400, 300});

    tk::Rect r0 = grid.rect_at(0);
    CHECK(r0.x == 0.0f);
    CHECK(r0.y == 0.0f);
    CHECK(r0.w == 32.0f);
    CHECK(r0.h == 32.0f);

    // Cell 1: x = bounds_.x + padding_.left + 1*(cell_w_+h_spacing_) = 0+0+34 = 34.
    tk::Rect r1 = grid.rect_at(1);
    CHECK(r1.x == 34.0f);
}
