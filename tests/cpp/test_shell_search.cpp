// ShellBase_search.cpp: global message search (debounce + stale-drop), the
// per-room "find in conversation" and find-in-thread state machines (match
// ordering, focus, navigation wrap, pagination-driven re-runs), forward
// completion bookkeeping and the permalink scroll helper.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::MessageRowData;

namespace
{

struct SearchShell : tesseract::test::TestShellBase
{
    ~SearchShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void on_restore_status_ui_() override { ++restores; }
    void request_relayout_() override { ++relayouts; }
    void fire_delayed()
    {
        auto pending = std::move(delayed);
        delayed.clear();
        for (auto& fn : pending)
            fn();
    }
    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int restores = 0;
    int relayouts = 0;

    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::clear_focused_state_;
    using ShellBase::current_room_id_;
    using ShellBase::handle_forward_done_ui_;
    using ShellBase::handle_forward_failed_ui_;
    using ShellBase::handle_in_room_search_failed_ui_;
    using ShellBase::handle_in_room_search_query_;
    using ShellBase::handle_in_room_search_results_ui_;
    using ShellBase::handle_room_directory_search_failed_ui_;
    using ShellBase::handle_room_directory_search_results_ui_;
    using ShellBase::handle_search_failed_ui_;
    using ShellBase::handle_search_query_;
    using ShellBase::handle_search_result_activated_;
    using ShellBase::handle_search_results_ui_;
    using ShellBase::handle_thread_search_failed_ui_;
    using ShellBase::handle_thread_search_query_;
    using ShellBase::handle_thread_search_results_ui_;
    using ShellBase::in_room_search_apply_highlights_;
    using ShellBase::in_room_search_clear_;
    using ShellBase::in_room_search_current_;
    using ShellBase::in_room_search_focus_current_;
    using ShellBase::in_room_search_goto_oldest_;
    using ShellBase::in_room_search_matches_;
    using ShellBase::in_room_search_maybe_paginate_;
    using ShellBase::in_room_search_navigate_;
    using ShellBase::in_room_search_paginate_;
    using ShellBase::in_room_search_paginate_rerun_;
    using ShellBase::in_room_search_pending_;
    using ShellBase::in_room_search_prev_match_count_;
    using ShellBase::in_room_search_request_id_;
    using ShellBase::in_room_search_rerun_on_paginate_;
    using ShellBase::in_room_search_room_id_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::next_paginate_id_;
    using ShellBase::pagination_;
    using ShellBase::pending_forwards_;
    using ShellBase::pending_paginates_;
    using ShellBase::pending_scroll_room_event_id_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::search_pending_queries_;
    using ShellBase::search_request_id_;
    using ShellBase::set_in_room_search_paginate_;
    using ShellBase::status_override_active_;
    using ShellBase::thread_search_apply_highlights_;
    using ShellBase::thread_search_clear_;
    using ShellBase::thread_search_current_;
    using ShellBase::thread_search_focus_current_;
    using ShellBase::thread_search_matches_;
    using ShellBase::thread_search_navigate_;
    using ShellBase::thread_search_pending_;
    using ShellBase::thread_search_request_id_;
    using ShellBase::try_scroll_to_room_event_;
};

tesseract::SearchHit srch_hit(const std::string& ev, std::uint64_t ts,
                         const std::string& room = "!r:x")
{
    tesseract::SearchHit h;
    h.event_id = ev;
    h.room_id = room;
    h.timestamp_ms = ts;
    return h;
}

MessageRowData srch_row(const std::string& ev)
{
    MessageRowData m;
    m.event_id = ev;
    return m;
}

// A RoomView-backed shell with a Client, a current room and loaded messages.
struct SearchRoomFixture
{
    tesseract::Client client;
    SearchShell s;
    std::unique_ptr<tesseract::views::RoomView> rv =
        tk::create_root_widget<tesseract::views::RoomView>(nullptr);

    explicit SearchRoomFixture(std::vector<std::string> loaded = {})
    {
        tesseract::RoomInfo info;
        info.id = "!r:x";
        info.name = "R";
        rv->set_room(info);
        std::vector<MessageRowData> rows;
        for (auto& e : loaded)
            rows.push_back(srch_row(e));
        rv->message_list()->set_messages(std::move(rows), true);
        s.room_view_ = rv.get();
        s.client_ = &client;
        s.current_room_id_ = "!r:x";
        s.in_room_search_room_id_ = "!r:x";
        s.rooms_ = {info};
        s.mark_room_index_dirty_();
    }
    tesseract::views::MessageListView* ml() { return rv->message_list(); }
};

} // namespace

TEST_CASE("global search: empty query or no client schedules nothing",
          "[shell][search]")
{
    SearchShell s;
    s.handle_search_query_("hello"); // no client
    s.fire_delayed();
    CHECK(s.search_request_id_ == 0);

    tesseract::Client client;
    s.client_ = &client;
    s.handle_search_query_("");
    s.fire_delayed();
    CHECK(s.search_request_id_ == 0);
    CHECK(s.search_pending_queries_.empty());
}

TEST_CASE("global search: a keystroke burst issues one request for the last query",
          "[shell][search]")
{
    tesseract::Client client;
    SearchShell s;
    s.client_ = &client;
    s.handle_search_query_("h");
    s.handle_search_query_("he");
    s.handle_search_query_("hel");
    REQUIRE(s.delayed.size() == 3);
    s.fire_delayed();
    CHECK(s.search_request_id_ == 1);
    REQUIRE(s.search_pending_queries_.size() == 1);
    CHECK(s.search_pending_queries_.at(1) == "hel");
}

TEST_CASE("global search: clearing the field cancels the pending debounce",
          "[shell][search]")
{
    tesseract::Client client;
    SearchShell s;
    s.client_ = &client;
    s.handle_search_query_("abc");
    s.handle_search_query_("");
    s.fire_delayed();
    CHECK(s.search_request_id_ == 0);
}

TEST_CASE("global search results: unknown and superseded ids are dropped",
          "[shell][search]")
{
    SearchShell s;
    s.search_pending_queries_[1] = "old";
    s.search_pending_queries_[2] = "new";
    s.search_request_id_ = 2;

    s.handle_search_results_ui_(99, {srch_hit("$a", 1)});
    CHECK(s.search_pending_queries_.size() == 2); // untouched

    // The older request completes after a newer one was issued: consumed but
    // not applied.
    s.handle_search_results_ui_(1, {srch_hit("$a", 1)});
    CHECK(s.search_pending_queries_.count(1) == 0);
    CHECK(s.search_pending_queries_.count(2) == 1);

    s.handle_search_results_ui_(2, {});
    CHECK(s.search_pending_queries_.empty());
}

TEST_CASE("global search results: room names resolve from the cached list",
          "[shell][search]")
{
    tesseract::Client client;
    SearchShell s;
    auto surface = TestSurface::create(900, 700);
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    s.main_app_ = app.get();
    s.client_ = &client;
    tesseract::RoomInfo named;
    named.id = "!named:x";
    named.name = "Named";
    tesseract::RoomInfo unnamed;
    unnamed.id = "!unnamed:x";
    s.rooms_ = {named, unnamed};
    s.mark_room_index_dirty_();

    s.search_pending_queries_[1] = "q";
    s.search_request_id_ = 1;
    s.handle_search_results_ui_(
        1, {srch_hit("$a", 1, "!named:x"), srch_hit("$b", 2, "!unnamed:x"),
            srch_hit("$c", 3, "!gone:x")});
    CHECK(s.search_pending_queries_.empty());

    // Failure path also consumes the id.
    s.search_pending_queries_[2] = "q2";
    s.search_request_id_ = 2;
    s.handle_search_failed_ui_(2, "boom");
    CHECK(s.search_pending_queries_.empty());
    s.handle_search_failed_ui_(2, "boom"); // unknown now: no-op
}

TEST_CASE("room directory results with no add-room view are ignored",
          "[shell][search]")
{
    SearchShell s;
    s.handle_room_directory_search_results_ui_(1, {}, true);
    s.handle_room_directory_search_failed_ui_(1, "x");
    CHECK(s.relayouts == 0);
}

TEST_CASE("forward completion: tracked ids are erased, unknown ids ignored",
          "[shell][search][forward]")
{
    SearchShell s;
    s.pending_forwards_[1] = "!a:x";
    s.pending_forwards_[2] = "!b:x";
    s.handle_forward_done_ui_(1);
    CHECK(s.pending_forwards_.size() == 1);
    s.handle_forward_done_ui_(77); // belongs to no window
    CHECK(s.pending_forwards_.size() == 1);

    s.handle_forward_failed_ui_(2, "denied");
    CHECK(s.pending_forwards_.empty());
    s.handle_forward_failed_ui_(2, "denied"); // second time: no-op
}

TEST_CASE("in-room search: empty query resets state", "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a", "$b"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1)};
    f.s.in_room_search_current_ = 0;
    f.ml()->set_search_matches({"$a"});

    f.s.handle_in_room_search_query_("", nullptr);
    CHECK(f.s.in_room_search_matches_.empty());
    CHECK(f.s.in_room_search_current_ == -1);
    CHECK_FALSE(f.ml()->has_search_matches());
}

TEST_CASE("in-room search: query is debounced, tagged and stale-guarded",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>();
    f.s.active_account_->client = std::make_unique<tesseract::Client>();
    f.s.handle_in_room_search_query_("he", nullptr);
    f.s.handle_in_room_search_query_("hey", nullptr);
    f.s.fire_delayed();
    CHECK(f.s.in_room_search_request_id_ == 1);
    REQUIRE(f.s.in_room_search_pending_.size() == 1);
    CHECK(f.s.in_room_search_pending_.at(1) == "hey");

    // Switching rooms before the debounce fires drops the request.
    f.s.handle_in_room_search_query_("late", nullptr);
    f.s.in_room_search_room_id_ = "!other:x";
    f.s.fire_delayed();
    CHECK(f.s.in_room_search_request_id_ == 1);
}

TEST_CASE("in-room search results: sorted oldest-first, newest focused",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a", "$b", "$c"});
    f.s.in_room_search_pending_[1] = "q";
    f.s.in_room_search_request_id_ = 1;
    f.s.handle_in_room_search_results_ui_(
        1, {srch_hit("$c", 30), srch_hit("$a", 10), srch_hit("$b", 20)});
    REQUIRE(f.s.in_room_search_matches_.size() == 3);
    CHECK(f.s.in_room_search_matches_[0].event_id == "$a");
    CHECK(f.s.in_room_search_matches_[2].event_id == "$c");
    CHECK(f.s.in_room_search_current_ == 2);
    CHECK(f.ml()->highlighted_event() == "$c");
    CHECK(f.ml()->search_match_ids().size() == 3);
}

TEST_CASE("in-room search results: focus is restored across a refresh",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a", "$b", "$c"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 10), srch_hit("$b", 20)};
    f.s.in_room_search_current_ = 0; // focused on $a
    f.s.in_room_search_pending_[5] = "q";
    f.s.in_room_search_request_id_ = 5;
    f.s.handle_in_room_search_results_ui_(5, {srch_hit("$a", 10), srch_hit("$b", 20),
                                              srch_hit("$c", 30)});
    CHECK(f.s.in_room_search_current_ == 0);
    CHECK(f.ml()->highlighted_event() == "$a");
}

TEST_CASE("in-room search results: goto-oldest focuses the first match",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a", "$b"});
    f.s.in_room_search_goto_oldest_ = true;
    f.s.in_room_search_pending_[1] = "q";
    f.s.in_room_search_request_id_ = 1;
    f.s.handle_in_room_search_results_ui_(1, {srch_hit("$a", 10), srch_hit("$b", 20)});
    CHECK(f.s.in_room_search_current_ == 0);
    CHECK_FALSE(f.s.in_room_search_goto_oldest_);
}

TEST_CASE("in-room search results: stale and unknown ids are ignored",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_pending_[1] = "q";
    f.s.in_room_search_pending_[2] = "q2";
    f.s.in_room_search_request_id_ = 2;
    f.s.handle_in_room_search_results_ui_(9, {srch_hit("$a", 1)});
    CHECK(f.s.in_room_search_matches_.empty());
    f.s.handle_in_room_search_results_ui_(1, {srch_hit("$a", 1)});
    CHECK(f.s.in_room_search_matches_.empty());
    CHECK(f.s.in_room_search_pending_.size() == 1);
}

TEST_CASE("in-room search results: zero matches clears and may paginate",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1)};
    f.s.in_room_search_current_ = 0;
    f.s.in_room_search_pending_[1] = "q";
    f.s.in_room_search_request_id_ = 1;
    f.s.handle_in_room_search_results_ui_(1, {});
    CHECK(f.s.in_room_search_matches_.empty());
    CHECK(f.s.in_room_search_current_ == -1);
    CHECK_FALSE(f.ml()->has_search_matches());
}

TEST_CASE("in-room search failure clears matches", "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1)};
    f.s.in_room_search_current_ = 0;
    f.s.in_room_search_pending_[3] = "q";
    f.s.in_room_search_request_id_ = 3;
    f.s.handle_in_room_search_failed_ui_(4, "x"); // unknown
    CHECK(f.s.in_room_search_matches_.size() == 1);
    f.s.handle_in_room_search_failed_ui_(3, "x");
    CHECK(f.s.in_room_search_matches_.empty());
    CHECK(f.s.in_room_search_current_ == -1);
}

TEST_CASE("in-room search navigation wraps in both directions",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a", "$b", "$c"});
    f.s.in_room_search_navigate_(-1); // nothing: no-op
    CHECK(f.s.in_room_search_current_ == -1);

    f.s.in_room_search_matches_ = {srch_hit("$a", 1), srch_hit("$b", 2), srch_hit("$c", 3)};
    f.s.in_room_search_current_ = 2;
    f.s.in_room_search_navigate_(1); // past newest -> oldest
    CHECK(f.s.in_room_search_current_ == 0);
    f.s.in_room_search_navigate_(-1); // before oldest, no paginate -> newest
    CHECK(f.s.in_room_search_current_ == 2);
    f.s.in_room_search_navigate_(-1);
    CHECK(f.s.in_room_search_current_ == 1);
    CHECK(f.ml()->highlighted_event() == "$b");

    f.s.in_room_search_current_ = -1; // unset starts from newest
    f.s.in_room_search_navigate_(-1);
    CHECK(f.s.in_room_search_current_ == 1);
}

TEST_CASE("in-room search navigation paginates at the oldest boundary",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1), srch_hit("$b", 2)};
    f.s.in_room_search_current_ = 0;
    f.s.in_room_search_paginate_ = true;
    f.s.in_room_search_navigate_(-1);
    // A backward paginate was requested instead of wrapping.
    CHECK(f.s.in_room_search_current_ == 0);
    CHECK(f.s.pagination_.at("!r:x").in_flight);
    CHECK(f.s.in_room_search_rerun_on_paginate_);
    CHECK(f.s.in_room_search_goto_oldest_);
    REQUIRE(f.s.pending_paginates_.size() == 1);
    CHECK(f.s.pending_paginates_.begin()->second.first == "!r:x");
    CHECK(f.s.pending_paginates_.begin()->second.second);
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.status_override_active_);

    // A second request while one is in flight does nothing.
    const auto id = f.s.next_paginate_id_;
    f.s.in_room_search_maybe_paginate_(true);
    CHECK(f.s.next_paginate_id_ == id);
}

TEST_CASE("in-room search pagination stops at the start of history",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1), srch_hit("$b", 2)};
    f.s.in_room_search_current_ = 1;
    f.s.in_room_search_paginate_ = true;
    f.s.pagination_["!r:x"].reached_start = true;
    f.s.status_override_active_ = true;
    f.s.in_room_search_maybe_paginate_(true);
    CHECK(f.s.in_room_search_current_ == 0);
    CHECK_FALSE(f.s.status_override_active_);
    CHECK(f.s.restores == 1);
    CHECK(f.s.pending_paginates_.empty());

    // At the oldest boundary of a non-looping bar, clamp and re-focus.
    f.s.in_room_search_paginate_ = false;
    f.s.in_room_search_current_ = 1;
    f.s.in_room_search_maybe_paginate_(true);
    CHECK(f.s.in_room_search_current_ == 0);
    f.s.in_room_search_current_ = 1;
    f.s.in_room_search_maybe_paginate_(false);
    CHECK(f.s.in_room_search_current_ == 1);
}

TEST_CASE("in-room search paginate toggle: off restores the status line",
          "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.status_override_active_ = true;
    f.s.set_in_room_search_paginate_(false);
    CHECK_FALSE(f.s.in_room_search_paginate_);
    CHECK_FALSE(f.s.status_override_active_);
    CHECK(f.s.restores == 1);

    // On, but with an empty query there is nothing to paginate for.
    f.s.set_in_room_search_paginate_(true);
    CHECK(f.s.in_room_search_paginate_);
    CHECK(f.s.pending_paginates_.empty());
}

TEST_CASE("in-room search clear resets all state", "[shell][search][inroom]")
{
    SearchRoomFixture f({"$a"});
    f.s.in_room_search_matches_ = {srch_hit("$a", 1)};
    f.s.in_room_search_current_ = 0;
    f.s.in_room_search_pending_[1] = "q";
    f.s.in_room_search_rerun_on_paginate_ = true;
    f.s.in_room_search_goto_oldest_ = true;
    f.s.in_room_search_paginate_rerun_ = true;
    f.s.in_room_search_prev_match_count_ = 4;
    f.s.in_room_search_clear_();
    CHECK(f.s.in_room_search_matches_.empty());
    CHECK(f.s.in_room_search_current_ == -1);
    CHECK(f.s.in_room_search_pending_.empty());
    CHECK(f.s.in_room_search_room_id_.empty());
    CHECK_FALSE(f.s.in_room_search_rerun_on_paginate_);
    CHECK_FALSE(f.s.in_room_search_goto_oldest_);
    CHECK_FALSE(f.s.in_room_search_paginate_rerun_);
    CHECK(f.s.in_room_search_prev_match_count_ == 0);
}

TEST_CASE("thread search: no open thread behaves as empty query",
          "[shell][search][thread]")
{
    SearchRoomFixture f({"$a"});
    f.s.thread_search_matches_ = {srch_hit("$a", 1)};
    f.s.thread_search_current_ = 0;
    f.s.handle_thread_search_query_("x"); // no main_room_pane_ thread root
    CHECK(f.s.thread_search_matches_.empty());
    CHECK(f.s.thread_search_current_ == -1);
    f.s.fire_delayed();
    CHECK(f.s.thread_search_request_id_ == 0);
}

TEST_CASE("thread search results: ordering, focus restore and failure",
          "[shell][search][thread]")
{
    SearchRoomFixture f;
    f.s.thread_search_pending_[1] = "q";
    f.s.thread_search_request_id_ = 1;
    f.s.handle_thread_search_results_ui_(
        1, {srch_hit("$c", 30), srch_hit("$a", 10), srch_hit("$b", 20)});
    REQUIRE(f.s.thread_search_matches_.size() == 3);
    CHECK(f.s.thread_search_matches_[0].event_id == "$a");
    CHECK(f.s.thread_search_current_ == 2);

    // Refresh keeps focus on the previously focused hit.
    f.s.thread_search_current_ = 1;
    f.s.thread_search_pending_[2] = "q";
    f.s.thread_search_request_id_ = 2;
    f.s.handle_thread_search_results_ui_(
        2, {srch_hit("$a", 10), srch_hit("$b", 20), srch_hit("$d", 40)});
    CHECK(f.s.thread_search_current_ == 1);

    // Stale / unknown ids are ignored.
    f.s.thread_search_pending_[3] = "q";
    f.s.thread_search_request_id_ = 9;
    f.s.handle_thread_search_results_ui_(3, {});
    CHECK(f.s.thread_search_matches_.size() == 3);
    f.s.handle_thread_search_results_ui_(55, {});
    CHECK(f.s.thread_search_matches_.size() == 3);

    // Zero results clear the cursor.
    f.s.thread_search_pending_[10] = "q";
    f.s.thread_search_request_id_ = 10;
    f.s.handle_thread_search_results_ui_(10, {});
    CHECK(f.s.thread_search_matches_.empty());
    CHECK(f.s.thread_search_current_ == -1);

    f.s.thread_search_matches_ = {srch_hit("$a", 1)};
    f.s.thread_search_current_ = 0;
    f.s.thread_search_pending_[11] = "q";
    f.s.thread_search_request_id_ = 11;
    f.s.handle_thread_search_failed_ui_(11, "x");
    CHECK(f.s.thread_search_matches_.empty());
    f.s.handle_thread_search_failed_ui_(11, "x"); // already consumed
}

TEST_CASE("thread search navigation wraps", "[shell][search][thread]")
{
    SearchRoomFixture f;
    f.s.thread_search_navigate_(1); // empty: no-op
    CHECK(f.s.thread_search_current_ == -1);
    f.s.thread_search_matches_ = {srch_hit("$a", 1), srch_hit("$b", 2), srch_hit("$c", 3)};
    f.s.thread_search_current_ = 2;
    f.s.thread_search_navigate_(1);
    CHECK(f.s.thread_search_current_ == 0);
    f.s.thread_search_navigate_(-1);
    CHECK(f.s.thread_search_current_ == 2);
    f.s.thread_search_current_ = -1;
    f.s.thread_search_navigate_(-1);
    CHECK(f.s.thread_search_current_ == 1);

    f.s.thread_search_clear_();
    CHECK(f.s.thread_search_matches_.empty());
    CHECK(f.s.thread_search_pending_.empty());
    CHECK(f.s.thread_search_current_ == -1);
}

TEST_CASE("try_scroll_to_room_event_ defers a loaded event and requests the "
          "rest",
          "[shell][search][scroll]")
{
    SearchRoomFixture f({"$loaded"});
    f.s.try_scroll_to_room_event_("");
    CHECK(f.s.pending_scroll_room_event_id_.empty());

    f.s.try_scroll_to_room_event_("$loaded");
    CHECK(f.s.pending_scroll_room_event_id_ == "$loaded");

    f.s.try_scroll_to_room_event_("$missing");
    CHECK(f.s.pending_scroll_room_event_id_ == "$missing");

    SearchRoomFixture none;
    none.s.client_ = nullptr;
    none.s.try_scroll_to_room_event_("$x");
    CHECK(none.s.pending_scroll_room_event_id_.empty());
}

TEST_CASE("clear_focused_state_ resets the focused-timeline bookkeeping",
          "[shell][search]")
{
    SearchShell s;
    auto& st = s.pagination_["!r:x"];
    st.is_focused = true;
    st.focus_event_id = "$e";
    st.reached_end = true;
    st.fwd_in_flight = true;
    st.reached_start = true;
    s.clear_focused_state_("!r:x");
    CHECK_FALSE(st.is_focused);
    CHECK(st.focus_event_id.empty());
    CHECK_FALSE(st.reached_end);
    CHECK_FALSE(st.fwd_in_flight);
    CHECK(st.reached_start); // history bound is untouched
}
