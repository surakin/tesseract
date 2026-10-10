// ShellBase_spaces.cpp: space hierarchy bookkeeping (parents, child counts),
// the unjoined-child summary fetch/backoff state machine, optimistic
// add/remove of space children and server-info ingestion.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct SpacesShell : tesseract::test::TestShellBase
{
    ~SpacesShell() override
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
    std::vector<std::function<void()>> delayed;
    int server_info_ready = 0;
    void on_server_info_ready_ui_() override { ++server_info_ready; }
    int cache_ready = 0;
    void on_space_children_cache_ready_ui_() override { ++cache_ready; }

    using ShellBase::active_account_;
    using ShellBase::active_space_id_;
    using ShellBase::apply_space_child_counts_;
    using ShellBase::cancel_unjoined_summaries_;
    using ShellBase::client_;
    using ShellBase::get_cached_unjoined_summaries_;
    using ShellBase::handle_server_info_async_ready_ui_;
    using ShellBase::handle_space_child_summary_ready_ui_;
    using ShellBase::main_app_;
    using ShellBase::next_request_id_;
    using ShellBase::parent_spaces_for_room_;
    using ShellBase::pending_room_actions_;
    using ShellBase::pending_summaries_;
    using ShellBase::request_add_room_to_space_;
    using ShellBase::request_remove_room_from_space_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::space_children_cache_;
    using ShellBase::space_root_shown_id_;
    using ShellBase::unjoined_fetch_gen_;
    using ShellBase::unjoined_fetch_pending_;
    using ShellBase::unjoined_fetch_retry_;
    using ShellBase::unjoined_space_children_cache_;
    using ShellBase::unjoined_summaries_cache_;
    using ShellBase::update_space_children_cache_;
    using ShellBase::show_space_root_;
    using ShellBase::mark_room_index_dirty_;
};

tesseract::RoomInfo space_room(const std::string& id, bool is_space = false)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    r.is_space = is_space;
    return r;
}

std::shared_ptr<tesseract::AccountSession> space_account(tesseract::Client* c)
{
    auto s = std::make_shared<tesseract::AccountSession>();
    s->user_id = "@me:x";
    (void)c;
    return s;
}

} // namespace

TEST_CASE("parent_spaces_for_room_ walks nested spaces and survives cycles",
          "[shell][spaces]")
{
    SpacesShell s;
    CHECK(s.parent_spaces_for_room_("").empty());
    CHECK(s.parent_spaces_for_room_("!orphan").empty());

    s.space_children_cache_["!inner"] = {"!room", "!other"};
    s.space_children_cache_["!outer"] = {"!inner"};
    auto parents = s.parent_spaces_for_room_("!room");
    REQUIRE(parents.size() == 2);
    CHECK(parents[0] == "!inner");
    CHECK(parents[1] == "!outer");

    // A cycle (outer is also inside inner) terminates.
    s.space_children_cache_["!inner"].push_back("!outer");
    parents = s.parent_spaces_for_room_("!room");
    CHECK(parents.size() == 2);
}

TEST_CASE("space rows aggregate unread counts from their joined children",
          "[shell][spaces]")
{
    SpacesShell s;
    auto sp = space_room("!space", true);
    auto a = space_room("!a");
    a.notification_count = 3;
    a.highlight_count = 1;
    a.unread_count = 4;
    a.last_activity_ts = 100;
    auto b = space_room("!b");
    b.unread_count = 2; // quiet unread only
    b.last_activity_ts = 500;
    auto c = space_room("!c");
    c.notification_count = 2;
    c.unread_count = 2;
    c.last_activity_ts = 300;
    s.rooms_ = {sp, a, b, c};
    s.space_children_cache_["!space"] = {"!a", "!b", "!c", "!missing"};

    auto display = s.rooms_;
    s.apply_space_child_counts_(display);
    CHECK(display[0].notification_count == 5);
    CHECK(display[0].highlight_count == 1);
    CHECK(display[0].unread_count == 8);
    // Newest *notifying* child wins over a newer quiet one.
    CHECK(display[0].last_activity_ts == 300);
    // Non-space rows are untouched.
    CHECK(display[1].notification_count == 3);
}

TEST_CASE("a space with only quiet unread children ranks by the quiet recency",
          "[shell][spaces]")
{
    SpacesShell s;
    auto sp = space_room("!space", true);
    auto b = space_room("!b");
    b.unread_count = 2;
    b.last_activity_ts = 500;
    s.rooms_ = {sp, b};
    s.space_children_cache_["!space"] = {"!b"};
    auto display = s.rooms_;
    s.apply_space_child_counts_(display);
    CHECK(display[0].notification_count == 0);
    CHECK(display[0].last_activity_ts == 500);
}

TEST_CASE("apply_space_child_counts_ is a no-op without a cache",
          "[shell][spaces]")
{
    SpacesShell s;
    auto sp = space_room("!space", true);
    sp.notification_count = 9;
    s.rooms_ = {sp};
    auto display = s.rooms_;
    s.apply_space_child_counts_(display);
    CHECK(display[0].notification_count == 9);
}

TEST_CASE("cancel_unjoined_summaries_ resets all in-flight state",
          "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    s.active_space_id_ = "!space";
    s.unjoined_fetch_pending_.insert("!x");
    s.pending_summaries_[1] = {"!space", "!x", 0};
    s.unjoined_fetch_retry_["!x"].attempts = 3;
    const auto gen = s.unjoined_fetch_gen_;
    s.cancel_unjoined_summaries_();
    CHECK(s.unjoined_fetch_gen_ == gen + 1);
    CHECK(s.unjoined_fetch_pending_.empty());
    CHECK(s.pending_summaries_.empty());
    CHECK(s.unjoined_fetch_retry_.empty());
    CHECK(s.active_space_id_.empty());
}

TEST_CASE("entering a space kicks off summary fetches for unloaded children once",
          "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    s.active_account_ = space_account(&client);
    s.active_account_->client = nullptr;
    s.unjoined_space_children_cache_["!space"] = {"!c1", "!c2"};

    const auto& summaries = s.get_cached_unjoined_summaries_("!space");
    CHECK(summaries.empty());
    CHECK(s.active_space_id_ == "!space");
    // No usable account client: both fetch slots are released again rather
    // than left pending forever.
    CHECK(s.unjoined_fetch_pending_.empty());
}

TEST_CASE("switching spaces bumps the fetch generation and drops stale state",
          "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    s.get_cached_unjoined_summaries_("!one");
    const auto gen = s.unjoined_fetch_gen_;
    s.unjoined_fetch_pending_.insert("!stale");
    s.get_cached_unjoined_summaries_("!one"); // same space: untouched
    CHECK(s.unjoined_fetch_gen_ == gen);
    CHECK(s.unjoined_fetch_pending_.count("!stale") == 1);
    s.get_cached_unjoined_summaries_("!two");
    CHECK(s.unjoined_fetch_gen_ == gen + 1);
    CHECK(s.unjoined_fetch_pending_.empty());
}

TEST_CASE("nameless summary stubs are pruned from the cache", "[shell][spaces]")
{
    SpacesShell s;
    tesseract::RoomSummary stub;
    stub.room_id = "!stub";
    tesseract::RoomSummary real;
    real.room_id = "!real";
    real.name = "Real";
    s.unjoined_summaries_cache_["!space"] = {stub, real};
    const auto& out = s.get_cached_unjoined_summaries_("!space");
    REQUIRE(out.size() == 1);
    CHECK(out[0].room_id == "!real");
}

TEST_CASE("a fetched summary is cached and replaces an older copy",
          "[shell][spaces]")
{
    SpacesShell s;
    s.pending_summaries_[7] = {"!space", "!c", s.unjoined_fetch_gen_};
    s.unjoined_fetch_pending_.insert("!c");
    s.handle_space_child_summary_ready_ui_(
        7, R"({"room_id":"!c","name":"First"})");
    CHECK(s.unjoined_fetch_pending_.empty());
    REQUIRE(s.unjoined_summaries_cache_["!space"].size() == 1);
    CHECK(s.unjoined_summaries_cache_["!space"][0].name == "First");

    s.pending_summaries_[8] = {"!space", "!c", s.unjoined_fetch_gen_};
    s.handle_space_child_summary_ready_ui_(
        8, R"({"room_id":"!c","name":"Second"})");
    REQUIRE(s.unjoined_summaries_cache_["!space"].size() == 1);
    CHECK(s.unjoined_summaries_cache_["!space"][0].name == "Second");
}

TEST_CASE("an unknown request id is ignored", "[shell][spaces]")
{
    SpacesShell s;
    s.handle_space_child_summary_ready_ui_(99, R"({"room_id":"!c","name":"x"})");
    CHECK(s.unjoined_summaries_cache_.empty());
}

TEST_CASE("a summary from a superseded generation frees its slot but is dropped",
          "[shell][spaces]")
{
    SpacesShell s;
    s.pending_summaries_[7] = {"!space", "!c", s.unjoined_fetch_gen_};
    s.unjoined_fetch_pending_.insert("!c");
    ++s.unjoined_fetch_gen_;
    s.handle_space_child_summary_ready_ui_(
        7, R"({"room_id":"!c","name":"Late"})");
    CHECK(s.unjoined_fetch_pending_.empty());
    CHECK(s.unjoined_summaries_cache_.empty());
}

TEST_CASE("failed summary fetches back off exponentially up to a cap",
          "[shell][spaces]")
{
    SpacesShell s;
    using namespace std::chrono;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        s.pending_summaries_[attempt] = {"!space", "!c", s.unjoined_fetch_gen_};
        const auto before = steady_clock::now();
        s.handle_space_child_summary_ready_ui_(attempt, "");
        const auto& rs = s.unjoined_fetch_retry_["!c"];
        CHECK(rs.attempts == attempt);
        const auto expected = seconds(5 * (1 << (attempt - 1)));
        CHECK(rs.next_retry >= before + expected - milliseconds(50));
        CHECK(rs.next_retry <= steady_clock::now() + expected);
    }
    // Far-out attempts are capped at five minutes.
    s.unjoined_fetch_retry_["!c"].attempts = 20;
    s.pending_summaries_[99] = {"!space", "!c", s.unjoined_fetch_gen_};
    const auto before = steady_clock::now();
    s.handle_space_child_summary_ready_ui_(99, "");
    CHECK(s.unjoined_fetch_retry_["!c"].next_retry <=
          steady_clock::now() + seconds(300));
    CHECK(s.unjoined_fetch_retry_["!c"].next_retry >= before + seconds(299));
    CHECK(s.unjoined_summaries_cache_.empty());
}

TEST_CASE("add/remove space child update caches optimistically and queue an action",
          "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    // No client: nothing happens.
    s.request_add_room_to_space_("!space", "!room");
    CHECK(s.space_children_cache_.empty());

    s.client_ = &client;
    s.request_add_room_to_space_("", "!room");
    s.request_add_room_to_space_("!space", "");
    CHECK(s.space_children_cache_.empty());

    s.unjoined_space_children_cache_["!space"] = {"!room", "!keep"};
    s.request_add_room_to_space_("!space", "!room");
    s.request_add_room_to_space_("!space", "!room"); // idempotent
    CHECK(s.space_children_cache_["!space"] == std::vector<std::string>{"!room"});
    CHECK(s.unjoined_space_children_cache_["!space"] ==
          std::vector<std::string>{"!keep"});
    REQUIRE(s.pending_room_actions_.size() == 2);
    for (const auto& [id, a] : s.pending_room_actions_)
    {
        CHECK(a.kind == ShellBase::RoomActionKind::AddSpaceChild);
        CHECK(a.space_id == "!space");
        CHECK(a.room_id == "!room");
    }

    s.pending_room_actions_.clear();
    s.request_remove_room_from_space_("!space", "!room");
    CHECK(s.space_children_cache_["!space"].empty());
    REQUIRE(s.pending_room_actions_.size() == 1);
    CHECK(s.pending_room_actions_.begin()->second.kind ==
          ShellBase::RoomActionKind::RemoveSpaceChild);
}

TEST_CASE("the space children cache is cleared when there is nothing to track",
          "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.space_children_cache_["!space"] = {"!a"};
    s.unjoined_fetch_pending_.insert("!x");
    s.update_space_children_cache_(); // no client
    CHECK(s.space_children_cache_.empty());
    CHECK(s.unjoined_fetch_pending_.empty());

    s.client_ = &client;
    s.space_children_cache_["!space"] = {"!a"};
    s.rooms_ = {space_room("!plain")}; // no spaces among the rooms
    s.update_space_children_cache_();
    CHECK(s.space_children_cache_.empty());
}

TEST_CASE("server info JSON updates capability flags", "[shell][spaces]")
{
    SpacesShell s;
    s.handle_server_info_async_ready_ui_(
        1, R"({"homeserver":"https://hs.invalid","supports_calls":true,
               "supports_qr_grant":true,"supports_profile_fields":false})");
    CHECK(s.server_info_.homeserver_url == "https://hs.invalid");
    CHECK(s.server_info_.supports_calls);
    CHECK(s.server_info_.supports_qr_grant);
    CHECK_FALSE(s.server_info_.supports_profile_fields);
    CHECK(s.server_info_ready == 1);
}

TEST_CASE("space root view shows for a known space only", "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    auto surface = TestSurface::create(1000, 700);
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    s.main_app_ = app.get();
    s.rooms_ = {space_room("!space", true), space_room("!room")};
    s.mark_room_index_dirty_();

    s.show_space_root_("");
    s.show_space_root_("!unknown");
    s.show_space_root_("!room"); // not a space
    CHECK(s.space_root_shown_id_.empty());

    s.show_space_root_("!space");
    CHECK(s.space_root_shown_id_ == "!space");
}
