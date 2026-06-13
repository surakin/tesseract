#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

#include <unordered_set>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

MessageRowData make_row(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("set_search_matches stores ids", "[message_list][search]")
{
    MessageListView v;
    CHECK_FALSE(v.has_search_matches());
    v.set_search_matches({"$a", "$b"});
    CHECK(v.has_search_matches());
    CHECK(v.search_match_ids().size() == 2);
    CHECK(v.search_match_ids().count("$a") == 1);
    CHECK(v.search_match_ids().count("$b") == 1);
    CHECK(v.search_match_ids().count("$c") == 0);
}

TEST_CASE("clear_search_matches empties the set", "[message_list][search]")
{
    MessageListView v;
    v.set_search_matches({"$a", "$b"});
    CHECK(v.has_search_matches());
    v.clear_search_matches();
    CHECK_FALSE(v.has_search_matches());
    CHECK(v.search_match_ids().empty());
}

TEST_CASE("set_messages does NOT clear search_match_ids", "[message_list][search]")
{
    MessageListView v;
    v.set_search_matches({"$a", "$b"});

    // Pagination / context rebuild (room_switch=false) must not clear matches.
    v.set_messages({make_row("$a"), make_row("$x")}, /*room_switch=*/false);
    CHECK(v.has_search_matches());
    CHECK(v.search_match_ids().count("$a") == 1);
    CHECK(v.search_match_ids().count("$b") == 1);

    // Even a full room switch must not clear matches — the shell owns clearing.
    v.set_messages({make_row("$y")}, /*room_switch=*/true);
    CHECK(v.has_search_matches());
    CHECK(v.search_match_ids().count("$a") == 1);
    CHECK(v.search_match_ids().count("$b") == 1);
}

TEST_CASE("set_search_matches with same set skips repaint", "[message_list][search]")
{
    MessageListView v;
    int repaint_count = 0;
    v.set_repaint_requester([&]{ ++repaint_count; });

    v.set_search_matches({"$a", "$b"});
    CHECK(repaint_count == 1);

    // Setting the identical set a second time must not trigger another repaint.
    v.set_search_matches({"$a", "$b"});
    CHECK(repaint_count == 1);

    // A genuinely different set must trigger a repaint.
    v.set_search_matches({"$a"});
    CHECK(repaint_count == 2);
}
