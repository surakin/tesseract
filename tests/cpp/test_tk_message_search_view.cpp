#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/MessageSearchView.h"

#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <chrono>
#include <string>
#include <vector>

using tesseract::views::MessageSearchView;

namespace
{

// Restores Settings::index_messages_for_search on scope exit.
struct MsvIndexingScope
{
    bool saved = tesseract::Settings::instance().index_messages_for_search;
    explicit MsvIndexingScope(bool on)
    {
        tesseract::Settings::instance().index_messages_for_search = on;
    }
    ~MsvIndexingScope() { tesseract::Settings::instance().index_messages_for_search = saved; }
};

std::uint64_t msv_now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

tesseract::SearchHit msv_hit(const std::string& ev, const std::string& room,
                             const std::string& body, std::uint64_t age_ms,
                             const std::string& sender = "Alice")
{
    tesseract::SearchHit h;
    h.event_id = ev;
    h.room_id = "!" + room + ":x";
    h.room_name = room;
    h.sender_name = sender;
    h.body = body;
    h.timestamp_ms = msv_now_ms() - age_ms;
    return h;
}

struct MsvStage : vt::Stage
{
    std::unique_ptr<MessageSearchView> view = tk::create_root_widget<MessageSearchView>(&host);
    MsvStage()
    {
        view->open();
        run();
    }
    void run() { mount(*view, {0, 0, 900, 700}); }
    std::vector<std::string> rows()
    {
        std::vector<std::string> out;
        for (const auto& n : vt::all_names(*view))
            if (n.find(", ") != std::string::npos)
                out.push_back(n);
        return out;
    }
};

} // namespace

TEST_CASE("MessageSearchView open/close lifecycle", "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    CHECK(s.view->is_open());
    CHECK(s.view->search_field_visible());
    CHECK(s.view->visible());
    int closed = 0;
    s.view->on_close = [&] { ++closed; };
    s.view->close();
    CHECK_FALSE(s.view->is_open());
    CHECK_FALSE(s.view->visible());
    CHECK(closed == 1);
    s.view->close();
    CHECK(closed == 1);
    s.view->open();
    CHECK(s.view->query().empty());
}

TEST_CASE("MessageSearchView typing queries; only results for the current "
          "query are accepted",
          "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    std::vector<std::string> queries;
    s.view->on_query_changed = [&](const std::string& q) { queries.push_back(q); };
    s.host.fields_created.at(0)->on_changed("hello");
    s.host.fields_created.at(0)->on_changed("hello"); // unchanged: ignored
    CHECK(queries == std::vector<std::string>{"hello"});
    CHECK(s.view->query() == "hello");

    s.view->set_results({msv_hit("$1", "Room", "stale", 1000)}, "hell"); // stale
    s.run();
    CHECK(s.rows().empty());

    s.view->set_results({msv_hit("$1", "Room", "hello world", 5 * 60 * 1000)}, "hello");
    s.run();
    REQUIRE(s.rows().size() == 1);
    CHECK(s.rows()[0] == "Room, Alice: hello world, 5m");
}

TEST_CASE("MessageSearchView relative time buckets and one-line snippets",
          "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    s.view->set_query("q");
    constexpr std::uint64_t kMin = 60ull * 1000, kHour = 60 * kMin, kDay = 24 * kHour;
    auto multi = msv_hit("$6", "R6", "line one\n\n  line\ttwo  ", 400 * kDay, "");
    multi.room_name.clear();
    s.view->set_results({msv_hit("$0", "R0", "a", 5 * 1000),
                         msv_hit("$1", "R1", "a", 2 * kHour),
                         msv_hit("$2", "R2", "a", 3 * kDay),
                         msv_hit("$3", "R3", "a", 21 * kDay),
                         msv_hit("$4", "R4", "a", 800 * kDay),
                         msv_hit("$5", "R5", "a", 0), multi},
                        "q");
    s.run();
    auto rows = s.rows();
    REQUIRE(rows.size() == 7);
    CHECK(rows[0] == "R0, Alice: a, now");
    CHECK(rows[1] == "R1, Alice: a, 2h");
    CHECK(rows[2] == "R2, Alice: a, 3d");
    CHECK(rows[3] == "R3, Alice: a, 3w");
    CHECK(rows[4] == "R4, Alice: a, 2y");
    CHECK(rows[5] == "R5, Alice: a, now");
    CHECK(rows[6] == "Unknown room, line one line two, 1y");
}

TEST_CASE("MessageSearchView selection moves, clamps and activates",
          "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    s.view->move_selection(1); // no results: no-op
    s.view->activate_selected();
    s.view->set_query("q");
    s.view->set_results({msv_hit("$a", "A", "x", 1000), msv_hit("$b", "B", "x", 1000),
                         msv_hit("$c", "C", "x", 1000)},
                        "q");
    s.run();

    std::string room, event;
    int closed = 0;
    s.view->on_result_activated = [&](const std::string& r, const std::string& e)
    {
        room = r;
        event = e;
    };
    s.view->on_close = [&] { ++closed; };

    s.view->move_selection(1);   // first selected by set_results -> index 1
    s.view->move_selection(10);  // clamps to last
    s.view->move_selection(-1);
    s.view->activate_selected();
    CHECK(event == "$b");
    CHECK(room == "!B:x");
    CHECK(closed == 1);
    CHECK_FALSE(s.view->is_open());
}

TEST_CASE("MessageSearchView submit key and row activation via accessibility "
          "activate the selected result",
          "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    s.view->set_query("q");
    s.view->set_results({msv_hit("$a", "A", "x", 1000), msv_hit("$b", "B", "x", 1000)}, "q");
    s.run();
    std::string event;
    s.view->on_result_activated = [&](const std::string&, const std::string& e) { event = e; };

    s.host.fields_created.at(0)->on_submit();
    CHECK(event == "$a");

    s.view->open();
    s.view->set_query("q");
    s.view->set_results({msv_hit("$a", "A", "x", 1000), msv_hit("$b", "B", "x", 1000)}, "q");
    s.run();
    auto nodes = vt::nodes_named(*s.view, "B, Alice: x, now");
    REQUIRE(nodes.size() == 1);
    CHECK(tk::invoke_default_action(nodes[0]));
    CHECK(event == "$b");
}

TEST_CASE("MessageSearchView backdrop click closes; card click and wheel "
          "are consumed",
          "[tk][view][msg_search]")
{
    MsvIndexingScope on(true);
    MsvStage s;
    int closed = 0;
    s.view->on_close = [&] { ++closed; };
    s.view->on_pointer_down({450, 300}); // inside the card
    s.view->on_pointer_up({450, 300}, true);
    CHECK(closed == 0);
    s.view->on_pointer_down({5, 5});
    s.view->on_pointer_up({5, 5}, false);
    CHECK(closed == 0);
    s.view->on_pointer_down({5, 5});
    s.view->on_pointer_up({5, 5}, true);
    CHECK(closed == 1);

    s.view->open();
    s.run();
    s.view->on_wheel({450, 300}, 0, 1, false);
    s.view->on_theme_changed(tk::Theme::dark());
    s.run();
}

TEST_CASE("MessageSearchView empty-state text depends on indexing and query",
          "[tk][view][msg_search]")
{
    {
        MsvIndexingScope off(false);
        MsvStage s;
        s.run(); // "Message search is disabled" path; field disabled
        s.view->set_query("anything");
        s.run();
    }
    {
        MsvIndexingScope on(true);
        MsvStage s;
        s.run(); // "Type to search"
        s.view->set_query("zzz");
        s.run(); // searching...
        s.view->set_results({}, "zzz");
        s.run(); // "No matches"
        s.view->set_visible(false);
    }
}
