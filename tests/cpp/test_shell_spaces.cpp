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
    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    std::vector<std::function<void()>> delayed;
    int server_info_ready = 0;
    void on_server_info_ready_ui_() override
    {
        ++server_info_ready;
    }
    int cache_ready = 0;
    void on_space_children_cache_ready_ui_() override
    {
        ++cache_ready;
    }

    using ShellBase::active_account_;
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
    using ShellBase::unjoined_fetch_;
    using ShellBase::unjoined_fetch_gen_counter_;
    using ShellBase::fetch_gen_current_;
    using ShellBase::reset_unjoined_fetch_state_;
    using ShellBase::request_summary_fetch_;
    using ShellBase::unjoined_fetch_retry_;
    using ShellBase::unjoined_space_children_cache_;
    using ShellBase::unjoined_summaries_cache_;
    using ShellBase::update_space_children_cache_;
    using ShellBase::show_space_root_;
    using ShellBase::space_stack_;
    using ShellBase::space_nav_frames_;
    using ShellBase::is_space_pinned_;
    using ShellBase::pin_space_;
    using ShellBase::unpin_space_;
    using ShellBase::sidebar_shows_space_;
    using ShellBase::push_unjoined_to_sidebar_;
    using ShellBase::reorder_pinned_space_;
    using ShellBase::select_picker_space_;
    using ShellBase::select_picker_home_;
    using ShellBase::refresh_space_picker_;
    using ShellBase::hide_pinned_spaces_;
    using ShellBase::refresh_room_list_;
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

TEST_CASE("parent_spaces_for_room_ walks nested spaces and survives cycles", "[shell][spaces]")
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

TEST_CASE("space rows aggregate unread counts from their joined children", "[shell][spaces]")
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

TEST_CASE("a space with only quiet unread children ranks by the quiet recency", "[shell][spaces]")
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

TEST_CASE("apply_space_child_counts_ is a no-op without a cache", "[shell][spaces]")
{
    SpacesShell s;
    auto sp = space_room("!space", true);
    sp.notification_count = 9;
    s.rooms_ = {sp};
    auto display = s.rooms_;
    s.apply_space_child_counts_(display);
    CHECK(display[0].notification_count == 9);
}

TEST_CASE("cancel_unjoined_summaries_ drops in-flight state except kept spaces", "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    s.get_cached_unjoined_summaries_("!one");
    s.get_cached_unjoined_summaries_("!two");
    s.get_cached_unjoined_summaries_("!three");
    s.unjoined_fetch_["!one"].pending.insert("!x");
    s.pending_summaries_[1] = {"!one", "!x", s.unjoined_fetch_["!one"].gen};
    s.pending_summaries_[2] = {"!two", "!y", s.unjoined_fetch_["!two"].gen};
    s.unjoined_fetch_retry_["!x"].attempts = 3;

    s.cancel_unjoined_summaries_("!two", "");
    CHECK(s.unjoined_fetch_.count("!two") == 1);
    CHECK(s.unjoined_fetch_.count("!one") == 0);
    CHECK(s.unjoined_fetch_.count("!three") == 0);
    CHECK(s.pending_summaries_.count(1) == 0);
    CHECK(s.pending_summaries_.count(2) == 1);
    // Backoff is per room and outlives a cancel.
    CHECK(s.unjoined_fetch_retry_["!x"].attempts == 3);

    s.cancel_unjoined_summaries_();
    CHECK(s.unjoined_fetch_.empty());
    CHECK(s.pending_summaries_.empty());
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
    REQUIRE(s.unjoined_fetch_.count("!space") == 1);
    CHECK(s.unjoined_fetch_["!space"].gen != 0);
    // No usable account client: both fetch slots are released again rather
    // than left pending forever.
    CHECK(s.unjoined_fetch_["!space"].pending.empty());
}

TEST_CASE("looking up one space leaves another space's fetch state alone", "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.client_ = &client;
    s.get_cached_unjoined_summaries_("!one");
    const auto gen_one = s.unjoined_fetch_["!one"].gen;
    s.unjoined_fetch_["!one"].pending.insert("!inflight");
    s.unjoined_fetch_retry_["!inflight"].attempts = 2;

    s.get_cached_unjoined_summaries_("!two");
    s.get_cached_unjoined_summaries_("!one"); // same space again: untouched

    CHECK(s.unjoined_fetch_["!one"].gen == gen_one);
    CHECK(s.unjoined_fetch_["!one"].pending.count("!inflight") == 1);
    CHECK(s.unjoined_fetch_["!two"].gen != gen_one);
    CHECK(s.unjoined_fetch_retry_["!inflight"].attempts == 2);
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

TEST_CASE("a fetched summary is cached and replaces an older copy", "[shell][spaces]")
{
    SpacesShell s;
    auto& fetch = s.unjoined_fetch_["!space"];
    fetch.gen = ++s.unjoined_fetch_gen_counter_;
    s.pending_summaries_[7] = {"!space", "!c", fetch.gen};
    fetch.pending.insert("!c");
    s.handle_space_child_summary_ready_ui_(7, R"({"room_id":"!c","name":"First"})");
    CHECK(s.unjoined_fetch_["!space"].pending.empty());
    REQUIRE(s.unjoined_summaries_cache_["!space"].size() == 1);
    CHECK(s.unjoined_summaries_cache_["!space"][0].name == "First");

    s.pending_summaries_[8] = {"!space", "!c", s.unjoined_fetch_["!space"].gen};
    s.handle_space_child_summary_ready_ui_(8, R"({"room_id":"!c","name":"Second"})");
    REQUIRE(s.unjoined_summaries_cache_["!space"].size() == 1);
    CHECK(s.unjoined_summaries_cache_["!space"][0].name == "Second");
}

TEST_CASE("an unknown request id is ignored", "[shell][spaces]")
{
    SpacesShell s;
    s.handle_space_child_summary_ready_ui_(99, R"({"room_id":"!c","name":"x"})");
    CHECK(s.unjoined_summaries_cache_.empty());
}

TEST_CASE("a summary from a superseded generation frees its slot but is dropped", "[shell][spaces]")
{
    SpacesShell s;
    auto& fetch = s.unjoined_fetch_["!space"];
    fetch.gen = ++s.unjoined_fetch_gen_counter_;
    s.pending_summaries_[7] = {"!space", "!c", fetch.gen};
    fetch.pending.insert("!c");
    fetch.gen = ++s.unjoined_fetch_gen_counter_; // superseded
    s.handle_space_child_summary_ready_ui_(7, R"({"room_id":"!c","name":"Late"})");
    CHECK(s.unjoined_fetch_["!space"].pending.empty());
    CHECK(s.unjoined_summaries_cache_.empty());
}

TEST_CASE("failed summary fetches back off exponentially up to a cap", "[shell][spaces]")
{
    SpacesShell s;
    using namespace std::chrono;
    const auto gen = (s.unjoined_fetch_["!space"].gen = ++s.unjoined_fetch_gen_counter_);
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        s.pending_summaries_[attempt] = {"!space", "!c", gen};
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
    s.pending_summaries_[99] = {"!space", "!c", gen};
    const auto before = steady_clock::now();
    s.handle_space_child_summary_ready_ui_(99, "");
    CHECK(s.unjoined_fetch_retry_["!c"].next_retry <= steady_clock::now() + seconds(300));
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
    CHECK(s.unjoined_space_children_cache_["!space"] == std::vector<std::string>{"!keep"});
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

TEST_CASE("the space children cache is cleared when there is nothing to track", "[shell][spaces]")
{
    tesseract::Client client;
    SpacesShell s;
    s.space_children_cache_["!space"] = {"!a"};
    s.unjoined_fetch_["!space"].pending.insert("!x");
    s.update_space_children_cache_(); // no client
    CHECK(s.space_children_cache_.empty());
    CHECK(s.unjoined_fetch_.empty());

    s.client_ = &client;
    s.space_children_cache_["!space"] = {"!a"};
    s.rooms_ = {space_room("!plain")}; // no spaces among the rooms
    s.update_space_children_cache_();
    CHECK(s.space_children_cache_.empty());
}

TEST_CASE("server info updates capability flags", "[shell][spaces]")
{
    SpacesShell s;
    tesseract::ServerInfo info;
    info.homeserver_url = "https://hs.invalid";
    info.supports_calls = true;
    info.supports_qr_grant = true;
    info.supports_profile_fields = false;
    s.handle_server_info_async_ready_ui_(1, info);
    CHECK(s.server_info_.homeserver_url == "https://hs.invalid");
    CHECK(s.server_info_.supports_calls);
    CHECK(s.server_info_.supports_qr_grant);
    CHECK_FALSE(s.server_info_.supports_profile_fields);
    CHECK(s.server_info_ready == 1);
}

TEST_CASE("a degraded server info refetch keeps the previous info", "[shell][spaces]")
{
    SpacesShell s;
    tesseract::ServerInfo good;
    good.homeserver_url = "https://hs.invalid";
    good.supports_calls = true;
    good.spec_versions.push_back({1, 11});
    s.handle_server_info_async_ready_ui_(1, good);
    CHECK(s.server_info_ready == 1);

    tesseract::ServerInfo degraded;
    degraded.homeserver_url = "https://hs.invalid";
    s.handle_server_info_async_ready_ui_(2, degraded);
    CHECK(s.server_info_.supports_calls);
    CHECK(s.server_info_.spec_versions.size() == 1);
    CHECK(s.server_info_ready == 1);
}

TEST_CASE("an empty server info is accepted when there was none before", "[shell][spaces]")
{
    SpacesShell s;
    tesseract::ServerInfo info;
    info.homeserver_url = "https://hs.invalid";
    s.handle_server_info_async_ready_ui_(1, info);
    CHECK(s.server_info_.homeserver_url == "https://hs.invalid");
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

namespace
{
struct PinFixture
{
    tesseract::Client client;
    SpacesShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);

    PinFixture()
    {
        s.client_ = &client;
        s.active_account_ = space_account(&client);
        s.main_app_ = app.get();
        s.rooms_ = {space_room("!sp", true), space_room("!other", true), space_room("!child")};
        s.space_children_cache_["!sp"] = {"!child"};
        s.mark_room_index_dirty_();
    }
};
} // namespace

TEST_CASE("pinning a space persists on the account and hides it from the list",
          "[shell][spaces][pin]")
{
    PinFixture f;
    CHECK_FALSE(f.s.is_space_pinned_("!sp"));
    f.s.pin_space_("!sp");
    f.s.pin_space_("!sp"); // idempotent
    CHECK(f.s.is_space_pinned_("!sp"));
    CHECK(f.s.active_account_->pinned_spaces == std::vector<std::string>{"!sp"});

    std::vector<tesseract::RoomInfo> list = f.s.rooms_;
    f.s.hide_pinned_spaces_(list);
    CHECK(std::none_of(list.begin(), list.end(),
                       [](const auto& r)
                       {
                           return r.id == "!sp";
                       }));
    CHECK(std::any_of(list.begin(), list.end(),
                      [](const auto& r)
                      {
                          return r.id == "!other";
                      }));
}

TEST_CASE("the picker shows pinned spaces only while something is pinned", "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.refresh_space_picker_();
    CHECK(f.app->space_picker()->entries().empty());

    f.s.pin_space_("!sp");
    REQUIRE(f.app->space_picker()->entries().size() == 1);
    CHECK(f.app->space_picker()->entries()[0].space_id == "!sp");

    f.s.unpin_space_("!sp");
    CHECK(f.app->space_picker()->entries().empty());
    CHECK(f.s.active_account_->pinned_spaces.empty());
}

TEST_CASE("picker selection drills into a space and Home returns to the root",
          "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.pin_space_("!sp");

    f.s.select_picker_space_("!sp");
    CHECK(f.s.space_stack_ == std::vector<std::string>{"!sp"});
    CHECK(f.app->space_picker()->active_space() == "!sp");

    f.s.select_picker_home_();
    CHECK(f.s.space_stack_.empty());
    CHECK(f.app->space_picker()->active_space().empty());
}

TEST_CASE("re-selecting the active picker tab opens that space's root panel",
          "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.pin_space_("!sp");
    f.s.select_picker_space_("!sp");
    CHECK(f.s.space_root_shown_id_.empty());
    f.s.select_picker_space_("!sp");
    CHECK(f.s.space_root_shown_id_ == "!sp");
    CHECK(f.s.space_stack_.size() == 1);
}

TEST_CASE("unpinning the space the sidebar is inside steps out to Home", "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.pin_space_("!sp");
    f.s.select_picker_space_("!sp");
    f.s.unpin_space_("!sp");
    CHECK(f.s.space_stack_.empty());
}

TEST_CASE("selecting an unknown space from the picker is ignored", "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.select_picker_space_("!nope");
    CHECK(f.s.space_stack_.empty());
}

TEST_CASE("reordering pinned spaces moves the id and keeps the picker in step",
          "[shell][spaces][pin]")
{
    PinFixture f;
    f.s.rooms_.push_back(space_room("!third", true));
    f.s.mark_room_index_dirty_();
    f.s.pin_space_("!sp");
    f.s.pin_space_("!other");
    f.s.pin_space_("!third");

    f.s.reorder_pinned_space_("!third", "!sp"); // to the front
    CHECK(f.s.active_account_->pinned_spaces ==
          std::vector<std::string>{"!third", "!sp", "!other"});
    REQUIRE(f.app->space_picker()->entries().size() == 3);
    CHECK(f.app->space_picker()->entries()[0].space_id == "!third");

    f.s.reorder_pinned_space_("!third", ""); // to the end
    CHECK(f.s.active_account_->pinned_spaces ==
          std::vector<std::string>{"!sp", "!other", "!third"});

    f.s.reorder_pinned_space_("!nope", "!sp"); // unknown: ignored
    CHECK(f.s.active_account_->pinned_spaces.size() == 3);
}

TEST_CASE("an unjoined-room summary only reaches the sidebar section for the space it is in",
          "[shell][spaces]")
{
    PinFixture f;
    auto* rl = f.app->room_list_view();
    auto summary_json = [](const char* id)
    {
        return std::string(R"({"room_id":")") + id + R"(","name":"N"})";
    };
    auto arrive = [&](std::uint64_t req, const char* space, const char* room)
    {
        auto& fetch = f.s.unjoined_fetch_[space];
        fetch.gen = ++f.s.unjoined_fetch_gen_counter_;
        f.s.pending_summaries_[req] = {space, room, fetch.gen};
        f.s.handle_space_child_summary_ready_ui_(req, summary_json(room));
    };

    // Space-root panel shows !other while the sidebar is at the root list.
    f.s.space_stack_.clear();
    arrive(1, "!other", "!x");
    CHECK(rl->unjoined_room_count() == 0);

    // Sidebar drilled into !sp: a result for !other still stays out.
    f.s.space_stack_ = {"!sp"};
    arrive(2, "!other", "!y");
    CHECK(rl->unjoined_room_count() == 0);
    CHECK(f.s.unjoined_summaries_cache_["!other"].size() == 2); // still cached

    // A result for the space the sidebar is in does show up.
    arrive(3, "!sp", "!z");
    CHECK(rl->unjoined_room_count() == 1);
}
