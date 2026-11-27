#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/DatePickerView.h"
#include "views/RoomHeader.h"

#include <tesseract/settings.h>

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

using tesseract::views::RoomHeader;

namespace
{

struct TkRoomHeaderStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 60);

    tk::LayoutCtx layout_ctx() const
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }

    tk::PaintCtx paint_ctx() const
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
    }

    void arrange(tk::Widget& w, tk::Rect bounds) const
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }

    void paint(tk::Widget& w) const
    {
        auto pc = paint_ctx();
        w.paint(pc);
    }
};

} // namespace

TEST_CASE("threads button fires on_threads_requested", "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    REQUIRE(r.w == 28.0f);
    REQUIRE(r.h == 28.0f);

    // The threads button is now a child tk::Button; dispatch the press through
    // the widget tree (as the host does) so the button claims it, then release
    // inside it to fire the click.
    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);
    CHECK(clicked);
}

TEST_CASE("threads button does NOT fire if release leaves the button rect",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(press);
    REQUIRE(claimer != nullptr);
    // Release well outside the button (still within the header strip). The
    // button only fires when the release lands inside its own bounds.
    const tk::Point release{10.0f, 30.0f};
    claimer->on_pointer_up(claimer->world_to_local(release), claimer->contains_world(release));
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button sits 8 px left of the calendar button when shown",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_jump_to_date_enabled(true);
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    const tk::Rect threads = h.threads_btn_rect_for_test();
    REQUIRE(threads.w == 28.0f);
    // Calendar button is 8 px from the right edge → x = 800 - 8 - 28 = 764.
    // Threads should be 8 px left of that → x = 764 - 8 - 28 = 728.
    CHECK(threads.x == 728.0f);
}

TEST_CASE("threads button is hidden by default", "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    // A fresh header — no SDK confirmation that the room has threads — must
    // not paint a clickable threads button. The hit-rect is zeroed so a click
    // anywhere along the right-side action area falls through to other
    // header gestures (info / topic) instead of firing on_threads_requested.
    const tk::Rect r = h.threads_btn_rect_for_test();
    CHECK(r.w == 0.0f);
    CHECK(r.h == 0.0f);

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };
    // Click where the button would be if it were shown.
    h.on_pointer_down({764.0f, 30.0f});
    h.on_pointer_up({764.0f, 30.0f}, true);
    CHECK_FALSE(clicked);
}

TEST_CASE("toggling threads visibility off after a click in progress is safe",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(press);
    REQUIRE(claimer != nullptr);

    // SDK reports list became empty mid-press (e.g., last thread redacted).
    h.set_show_threads_btn(false);
    st.arrange(h, {0, 0, 800, 60});

    // Release at the original press location. The button's bounds are now
    // zeroed, so the release lands outside it and the click must not fire.
    claimer->on_pointer_up(claimer->world_to_local(press), claimer->contains_world(press));
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button alone takes the right-most slot when calendar is off",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    const tk::Rect r = h.threads_btn_rect_for_test();
    // With only the threads button visible, it occupies the calendar slot:
    // x = 800 - 8 (margin) - 28 (size) = 764.
    CHECK(r.x == 764.0f);
}

TEST_CASE("search button fires on_search_requested", "[room_header][search]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_search_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_search_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.search_btn_rect_for_test();
    REQUIRE(r.w == 28.0f);
    REQUIRE(r.h == 28.0f);

    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);
    CHECK(clicked);
}

TEST_CASE("search button does NOT fire if release leaves the button rect", "[room_header][search]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_search_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_search_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.search_btn_rect_for_test();
    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    const tk::Point outside{0.0f, 0.0f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(outside), false);
    CHECK_FALSE(clicked);
}

// ── Clickable title block (Widget hover/click feature) ──────────────────

#include "tesseract/types.h"
#include "tk_test_host.h"

TEST_CASE("Hovering the room name lights the title block; clicking it opens room info",
          "[room_header][hover]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.id = "!r:example.org";
    info.name = "Design";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    TestHost host(&h);

    int opened = 0;
    h.on_info_requested = [&]
    {
        ++opened;
    };

    const tk::Point on_name{120.0f, 30.0f};
    host.dispatch_pointer_move(on_name);
    REQUIRE(host.hover_feedback_.lock() != nullptr);
    tk::Widget* title_block = host.hover_feedback_.lock().get();
    CHECK(title_block->hovered());
    CHECK_FALSE(dynamic_cast<tk::Button*>(title_block)); // not an action button
    CHECK(host.last_cursor() == tk::CursorShape::Pointer);

    host.dispatch_pointer_down(on_name);
    host.dispatch_pointer_up(on_name);
    CHECK(opened == 1);

    host.dispatch_pointer_move({400.0f, 100.0f}); // off the header entirely
    CHECK_FALSE(title_block->hovered());
}

TEST_CASE("The title block adds no extra Tab stop beside its keyboard target",
          "[room_header][hover][keyboard]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    TestHost host(&h);

    host.dispatch_key_down({tk::Key::Tab});
    CHECK(host.focused_widget() == h.title_target());
}

TEST_CASE("The title block keeps the name and topic labels where RoomHeader placed them",
          "[room_header][hover]")
{
    // The labels are children of the hoverable title block; arranging that
    // block must not reset them to its own rect (base Widget::arrange would).
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    info.topic = "Weekly sync";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});

    const tk::Rect title = h.title_target()->bounds();
    const tk::Rect name = h.name_label_rect_for_test();
    const tk::Rect topic = h.topic_label_rect_for_test();
    REQUIRE(title.w > 0.0f);
    REQUIRE(name.w > 0.0f);
    REQUIRE(topic.w > 0.0f);
    CHECK(name.h < title.h); // not stretched to the whole block
    CHECK(topic.y > name.y); // topic sits below the name
    CHECK(name.y + name.h <= topic.y + 0.5f);
}

TEST_CASE("A bridged room shows a bridge badge beside the title; an override hides it",
          "[room_header][bridge]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    info.is_bridged = true;
    info.bridge_network_name = "WhatsApp";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});

    auto has_bridge_row = [&]
    {
        for (std::size_t i = 0; i < h.access_row_count(); ++i)
        {
            if (h.access_name_for_widget_row(i).find("WhatsApp") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has_bridge_row());

    info.bridge_overridden = true;
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    CHECK_FALSE(has_bridge_row());
}

#ifndef _WIN32
TEST_CASE("local_midnight_ms starts the picked day at local midnight",
          "[tk][view][room_header][jump]")
{
    // The caller's TZ is restored when the case ends.
    const char* saved = std::getenv("TZ");
    const std::string saved_tz = saved ? saved : "";
    auto set_tz = [](const char* tz)
    {
        setenv("TZ", tz, 1);
        tzset();
    };

    // POSIX zone strings: "XYZ-3" is three hours ahead of UTC, "PST+8" eight behind.
    set_tz("XYZ-3");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791752400000ULL);
    set_tz("PST+8");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791792000000ULL);
    set_tz("UTC");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791763200000ULL);

    if (saved)
    {
        set_tz(saved_tz.c_str());
    }
    else
    {
        unsetenv("TZ");
        tzset();
    }
}
#endif

TEST_CASE("set_room keeps topic state current while a presence subtitle shows",
          "[room_header][presence]")
{
    RoomHeader h;
    h.set_presence_text("Online");

    tesseract::RoomInfo info;
    info.id = "!b:x";
    info.name = "Other";
    info.topic = "first line\nsecond line";
    h.set_room(info);
    CHECK(h.topic_multiline_for_test());

    // Presence clearing afterwards must leave the new room's topic state.
    h.set_presence_text("");
    CHECK(h.topic_multiline_for_test());
}

namespace
{

tesseract::RoomInfo presence_test_room(const std::string& topic, const std::string& topic_html = {})
{
    tesseract::RoomInfo info;
    info.id = "!p:x";
    info.name = "Friend";
    info.topic = topic;
    info.topic_html = topic_html;
    return info;
}

} // namespace

TEST_CASE("set_room clears presence only when the room changes", "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    auto dm = presence_test_room("");
    dm.id = "!dm:x";
    h.set_room(dm);
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "Online");

    // Same room (an info update): presence stays.
    dm.name = "Renamed";
    h.set_room(dm);
    CHECK(h.presence_text_for_test() == "Online");
    CHECK(h.subtitle_for_test() == "Online");

    // Different room: presence is gone at once and only the new topic shows.
    auto other = presence_test_room("Weekly sync");
    other.id = "!room:x";
    h.set_room(other);
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.presence_text_for_test().empty());
    CHECK(h.subtitle_for_test() == "Weekly sync");

    // A topicless room with the same topic as before also drops presence.
    h.set_presence_text("Away");
    auto third = presence_test_room("Weekly sync");
    third.id = "!third:x";
    h.set_room(third);
    CHECK(h.presence_text_for_test().empty());
    CHECK(h.subtitle_for_test() == "Weekly sync");
}

TEST_CASE("A plain topic shows the presence appended after it", "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "Weekly sync · Online");
}

TEST_CASE("A multiline topic keeps its first line and ellipsis before the presence",
          "[room_header][presence]")
{
    RoomHeader h;
    h.set_room(presence_test_room("first line\nsecond line"));
    h.set_presence_text("Online");
    CHECK(h.subtitle_for_test() == "first line… · Online");
}

TEST_CASE("A topic with a link keeps the link and gets an unlinked presence tail",
          "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("see https://example.org/x now"));
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "see https://example.org/x now · Online");

    std::size_t links = 0;
    const std::size_t rows = h.access_row_count();
    for (std::size_t i = 0; i < rows; ++i)
    {
        links += h.access_role_for_widget_row(i) == tk::Role::Link ? 1 : 0;
    }
    CHECK(links == 1);
}

TEST_CASE("An HTML topic gets the presence appended", "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Docs here", "<a href=\"https://example.org\">Docs</a> here"));
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "Docs here · Online");
}

TEST_CASE("Presence alone and topic alone each fill the second line", "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room(""));
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "Online");

    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "Weekly sync");
}

TEST_CASE("Toggling presence with an unchanged topic updates the line", "[room_header][presence]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("see https://example.org/x"));
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "see https://example.org/x");
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "see https://example.org/x · Online");
    h.set_presence_text("");
    st.arrange(h, {0, 0, 800, 60});
    CHECK(h.subtitle_for_test() == "see https://example.org/x");
}

namespace
{

// Sets the message layout for the lifetime of the object and restores the
// previous value (the setting is process-global, see test_message_list_*).
struct ScopedMessageLayout
{
    explicit ScopedMessageLayout(tesseract::Settings::MessageLayout layout)
        : previous_(tesseract::Settings::instance().message_layout)
    {
        tesseract::Settings::instance().message_layout = layout;
    }
    ~ScopedMessageLayout()
    {
        tesseract::Settings::instance().message_layout = previous_;
    }
    tesseract::Settings::MessageLayout previous_;
};

} // namespace

TEST_CASE("The IRC layout halves the header of every room", "[room_header][irc]")
{
    ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("Last seen 5m ago");

    auto lc = st.layout_ctx();
    CHECK(h.measure(lc, {800, 60}).h == RoomHeader::kCompactHeight);
    CHECK(h.avatar_diameter_for_test() == RoomHeader::kCompactAvatarSize);

    // A normal room (no presence line) gets the same compact strip.
    h.set_presence_text("");
    CHECK(h.measure(lc, {800, 60}).h == RoomHeader::kCompactHeight);
    CHECK(h.avatar_diameter_for_test() == RoomHeader::kCompactAvatarSize);
}

TEST_CASE("Only the IRC layout compacts the header", "[room_header][irc]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("Online");

    for (const auto layout :
         {tesseract::Settings::MessageLayout::Classic, tesseract::Settings::MessageLayout::Bubbles})
    {
        ScopedMessageLayout scoped(layout);
        auto lc = st.layout_ctx();
        CHECK(h.measure(lc, {800, 60}).h == RoomHeader::kHeight);
        CHECK(h.avatar_diameter_for_test() == RoomHeader::kAvatarSize);
    }

    // The condensed tab-bar header keeps its own thin strip in IRC mode.
    {
        ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
        h.set_condensed(true);
        auto lc = st.layout_ctx();
        CHECK(h.measure(lc, {800, 60}).h < RoomHeader::kCompactHeight);
        h.set_condensed(false);
    }
}

TEST_CASE("Compact IRC header puts the presence right of the name, topic after it",
          "[room_header][irc]")
{
    ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("Last seen 5m ago");
    st.arrange(h, {0, 0, 800, RoomHeader::kCompactHeight});

    // halved avatar + gap after the strip padding
    const tk::Rect name = h.name_label_rect_for_test();
    CHECK(name.x == 16.0f + RoomHeader::kCompactAvatarSize + 8.0f);

    const tk::Rect presence = h.presence_rect_for_test();
    CHECK(presence.w > 0.0f);
    CHECK(presence.x > name.x + name.w);
    CHECK(presence.y >= 0.0f);
    CHECK(presence.y + presence.h <= RoomHeader::kCompactHeight);

    // The topic follows on the same row, no second line.
    const tk::Rect topic = h.topic_label_rect_for_test();
    CHECK(topic.x > presence.x);
    CHECK(topic.y + topic.h <= RoomHeader::kCompactHeight);

    // Paints without a real provider or cached avatar (initials fallback).
    CHECK_NOTHROW(st.paint(h));
}

TEST_CASE("Compact IRC header of a normal room puts the topic right after the name",
          "[room_header][irc]")
{
    ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("");
    st.arrange(h, {0, 0, 800, RoomHeader::kCompactHeight});

    CHECK(h.presence_rect_for_test().w == 0.0f);

    const tk::Rect name = h.name_label_rect_for_test();
    const tk::Rect topic = h.topic_label_rect_for_test();
    CHECK(topic.x > name.x + name.w);
    CHECK(topic.y + topic.h <= RoomHeader::kCompactHeight);
    CHECK(topic.x < 200.0f); // right after the name, not pushed out
}

TEST_CASE("Compact IRC header keeps the presence when the name is long", "[room_header][irc]")
{
    ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
    TkRoomHeaderStage st;
    RoomHeader h;
    auto room = presence_test_room("Weekly sync");
    room.name = std::string(200, 'W');
    h.set_room(room);
    h.set_presence_text("Online");
    st.arrange(h, {0, 0, 420, RoomHeader::kCompactHeight});

    const tk::Rect presence = h.presence_rect_for_test();
    CHECK(presence.w > 0.0f);
    CHECK(presence.x + presence.w <= 420.0f);
}

TEST_CASE("Leaving the IRC layout restores the two-line header", "[room_header][irc]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_room(presence_test_room("Weekly sync"));
    h.set_presence_text("Online");

    {
        ScopedMessageLayout irc(tesseract::Settings::MessageLayout::Irc);
        st.arrange(h, {0, 0, 800, RoomHeader::kCompactHeight});
        CHECK(h.presence_rect_for_test().w > 0.0f);
        CHECK(h.subtitle_for_test() == "Weekly sync · Online");
    }
    {
        ScopedMessageLayout classic(tesseract::Settings::MessageLayout::Classic);
        st.arrange(h, {0, 0, 800, RoomHeader::kHeight});
        CHECK(h.presence_rect_for_test().w == 0.0f);
        CHECK(h.subtitle_for_test() == "Weekly sync · Online");
    }
}
