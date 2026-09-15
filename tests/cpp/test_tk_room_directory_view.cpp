#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "views/RoomDirectoryView.h"

#include <tesseract/types.h>

using tesseract::views::RoomDirectoryView;

namespace
{
tesseract::RoomDirectoryEntry make_entry(const std::string& room_id)
{
    tesseract::RoomDirectoryEntry e;
    e.room_id = room_id;
    e.name = "Test Room";
    e.topic = "A test topic";
    e.join_rule = "public";
    e.joined_members = 42;
    return e;
}
} // namespace

TEST_CASE("RoomDirectoryView: open() issues an initial search",
          "[roomdirectory]")
{
    auto v = tk::create_root_widget<RoomDirectoryView>(nullptr);

    int requests = 0;
    std::uint64_t last_id = 0;
    v->on_search_requested =
        [&](std::uint64_t id, const std::string&, const std::string&)
        {
            ++requests;
            last_id = id;
        };

    v->open();
    CHECK(v->is_open());
    CHECK(requests == 1);
    CHECK(last_id != 0);

    // Re-opening (e.g. switching back to the tab) doesn't re-issue the
    // search once one has already run.
    v->open();
    CHECK(requests == 1);
}

TEST_CASE("RoomDirectoryView: set_results appends only for the active "
          "request and ignores stale responses",
          "[roomdirectory]")
{
    auto v = tk::create_root_widget<RoomDirectoryView>(nullptr);

    std::uint64_t id = 0;
    v->on_search_requested =
        [&](std::uint64_t rid, const std::string&, const std::string&)
        { id = rid; };
    v->open();
    REQUIRE(id != 0);

    v->set_results(id, {make_entry("!a:s")}, false);
    CHECK(v->row_count_for_test() == 1);

    // Stale request id — ignored.
    v->set_results(id + 1000, {make_entry("!stale:s")}, true);
    CHECK(v->row_count_for_test() == 1);

    // Next page under the same request id — appended.
    v->set_results(id, {make_entry("!b:s")}, true);
    CHECK(v->row_count_for_test() == 2);
}

TEST_CASE("RoomDirectoryView: join button enables only after a row is "
          "selected, and fires the right room id",
          "[roomdirectory]")
{
    auto v = tk::create_root_widget<RoomDirectoryView>(nullptr);

    std::uint64_t id = 0;
    v->on_search_requested =
        [&](std::uint64_t rid, const std::string&, const std::string&)
        { id = rid; };
    v->open();
    v->set_results(id, {make_entry("!a:s")}, true);

    CHECK_FALSE(v->join_button_enabled());

    v->select_row_for_test(0);
    CHECK(v->join_button_enabled());

    int joins = 0;
    std::string joined_id;
    v->on_join_requested = [&](const std::string& rid, const std::string&)
    {
        ++joins;
        joined_id = rid;
    };
    v->trigger_join_for_test();
    CHECK(joins == 1);
    CHECK(joined_id == "!a:s");

    // Disabled while the join is in flight — a second click is a no-op.
    CHECK_FALSE(v->join_button_enabled());
    v->trigger_join_for_test();
    CHECK(joins == 1);

    // A failure re-enables it so the user can retry.
    v->set_join_failed("boom");
    CHECK(v->join_button_enabled());
}

TEST_CASE("RoomDirectoryView: search_failed on the active request surfaces "
          "an error and does not touch stale ones",
          "[roomdirectory]")
{
    auto v = tk::create_root_widget<RoomDirectoryView>(nullptr);

    std::uint64_t id = 0;
    v->on_search_requested =
        [&](std::uint64_t rid, const std::string&, const std::string&)
        { id = rid; };
    v->open();

    v->set_search_failed(id + 1000, "ignored");
    CHECK(v->row_count_for_test() == 0);

    v->set_search_failed(id, "boom");
    CHECK(v->row_count_for_test() == 0);
}
