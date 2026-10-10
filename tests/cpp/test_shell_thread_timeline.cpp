// ShellBase_timeline.cpp / ShellBase_search.cpp: events for an OPEN thread
// (reset / insert / update / remove / prepend / append into the thread view),
// find-in-thread, the animation tick that advances GIF frames, and the
// Settings panel's search-index / cache-size polling.

#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "views/SettingsView.h"
#include "views/ThreadView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct TtImg : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return 4; }
};

struct TtShell : tesseract::test::TestShellBase
{
    ~TtShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        main_room_pane_.reset();
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        delayed.emplace_back(ms, std::move(fn));
    }
    void prep_row_media_(const tesseract::Event&, bool) override {}
    void request_relayout_() override { ++relayouts; }
    void repaint_anim_frame_() override { ++anim_repaints; }
    void stop_anim_tick_() override { ++anim_stops; }
    std::int64_t monotonic_ms_() override { return now_ms; }
    void fire_delayed()
    {
        auto d = std::move(delayed);
        delayed.clear();
        for (auto& [ms, fn] : d)
            fn();
    }

    std::vector<std::pair<int, std::function<void()>>> delayed;
    int relayouts = 0, anim_repaints = 0, anim_stops = 0;
    std::int64_t now_ms = 1000;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::handle_thread_inserted_ui_;
    using ShellBase::handle_thread_messages_appended_ui_;
    using ShellBase::handle_thread_messages_prepended_ui_;
    using ShellBase::handle_thread_removed_ui_;
    using ShellBase::handle_thread_reset_ui_;
    using ShellBase::handle_thread_search_failed_ui_;
    using ShellBase::handle_thread_search_query_;
    using ShellBase::handle_thread_search_results_ui_;
    using ShellBase::handle_thread_updated_ui_;
    using ShellBase::main_room_pane_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::refresh_cache_sizes_poll_;
    using ShellBase::refresh_search_index_stats_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::search_stats_panel_open_;
    using ShellBase::start_search_index_stats_poll_;
    using ShellBase::stats_settings_view_;
    using ShellBase::stop_search_index_stats_poll_;
    using ShellBase::thread_search_current_;
    using ShellBase::thread_search_matches_;
    using ShellBase::thread_search_pending_;
    using ShellBase::thread_search_request_id_;
    using ShellBase::tick_anim_;
    using ShellBase::thread_search_clear_;
    using ShellBase::thread_search_navigate_;
    using ShellBase::pool_;
    using ShellBase::mut_pool_;
};

std::unique_ptr<tesseract::Event> tt_ev(const std::string& id,
                                        const std::string& reply_to = "")
{
    auto e = std::make_unique<tesseract::Event>();
    e->event_id = id;
    e->type = tesseract::EventType::Text;
    e->body = "body " + id;
    e->in_reply_to_id = reply_to;
    return e;
}

tesseract::EventList tt_list(std::initializer_list<const char*> ids)
{
    tesseract::EventList l;
    for (auto* i : ids)
        l.push_back(tt_ev(i));
    return l;
}

struct TtFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    TtShell s;
    std::unique_ptr<tesseract::views::RoomView> view =
        tk::create_root_widget<tesseract::views::RoomView>(nullptr);

    TtFx()
    {
        tesseract::RoomInfo info;
        info.id = "!r:x";
        view->set_room(info);
        s.client_ = &client;
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.my_user_id_ = "@me:x";
        s.room_view_ = view.get();
        s.current_room_id_ = "!r:x";
        s.rooms_ = {info};
        s.mark_room_index_dirty_();
        s.main_room_pane_ = std::make_unique<tesseract::RoomPane>(
            tesseract::RoomPane::Deps{
                .shell = &s, .repaint = [] {}, .relayout = [] {}},
            "!r:x");
        s.main_room_pane_->attach({.room_view = view.get()});
        view->on_thread_open_requested("$root");
    }
    tesseract::views::MessageListView* thread_list()
    {
        auto* tv = view->thread_view();
        return tv ? tv->message_list() : nullptr;
    }
    std::vector<std::string> thread_ids()
    {
        std::vector<std::string> v;
        if (auto* ml = thread_list())
            for (const auto& m : ml->messages())
                v.push_back(m.event_id);
        return v;
    }
};

using Ids = std::vector<std::string>;

} // namespace

TEST_CASE("thread events fill the open thread view", "[shell][thread_timeline]")
{
    TtFx f;
    REQUIRE(f.s.main_room_pane_->thread_root() == "$root");
    REQUIRE(f.thread_list() != nullptr);

    f.s.handle_thread_reset_ui_("!r:x", "$root", tt_list({"$a", "$c"}));
    CHECK(f.thread_ids() == Ids{"$a", "$c"});

    f.s.handle_thread_inserted_ui_("!r:x", "$root", 1, tt_ev("$b"));
    CHECK(f.thread_ids() == Ids{"$a", "$b", "$c"});

    auto upd = tt_ev("$b");
    upd->body = "edited";
    f.s.handle_thread_updated_ui_("!r:x", "$root", 1, std::move(upd));
    CHECK(f.thread_list()->messages()[1].body == "edited");

    f.s.handle_thread_removed_ui_("!r:x", "$root", 0);
    CHECK(f.thread_ids() == Ids{"$b", "$c"});

    f.s.handle_thread_messages_appended_ui_("!r:x", "$root", tt_list({"$d", "$e"}));
    CHECK(f.thread_ids() == Ids{"$b", "$c", "$d", "$e"});
    f.s.handle_thread_messages_prepended_ui_("!r:x", "$root", tt_list({"$z"}));
    CHECK(f.thread_ids().front() == "$z");
}

TEST_CASE("events for another thread or room are dropped", "[shell][thread_timeline]")
{
    TtFx f;
    f.s.handle_thread_reset_ui_("!r:x", "$root", tt_list({"$a"}));
    f.s.handle_thread_messages_appended_ui_("!r:x", "$other", tt_list({"$x"}));
    f.s.handle_thread_messages_prepended_ui_("!other:x", "$root", tt_list({"$y"}));
    f.s.handle_thread_inserted_ui_("!r:x", "$other", 0, tt_ev("$z"));
    f.s.handle_thread_updated_ui_("!r:x", "$other", 0, tt_ev("$z"));
    f.s.handle_thread_removed_ui_("!r:x", "$other", 0);
    f.s.handle_thread_reset_ui_("!r:x", "$other", tt_list({"$q"}));
    CHECK(f.thread_ids() == Ids{"$a"});
}

TEST_CASE("unhandled and null thread events are ignored, replies resolve",
          "[shell][thread_timeline]")
{
    TtFx f;
    f.s.handle_thread_reset_ui_("!r:x", "$root", tt_list({"$a"}));
    auto unhandled = tt_ev("$u");
    unhandled->type = tesseract::EventType::Unhandled;
    tesseract::EventList mixed;
    mixed.push_back(std::move(unhandled));
    mixed.push_back(nullptr);
    mixed.push_back(tt_ev("$reply", "$a"));
    f.s.handle_thread_messages_appended_ui_("!r:x", "$root", std::move(mixed));
    CHECK(f.thread_ids() == Ids{"$a", "$reply"});
    f.s.handle_thread_inserted_ui_("!r:x", "$root", 0, nullptr);
    f.s.handle_thread_updated_ui_("!r:x", "$root", 0, nullptr);
    f.s.handle_thread_inserted_ui_("!r:x", "$root", 0, tt_ev("$r2", "$a"));
    f.s.handle_thread_updated_ui_("!r:x", "$root", 0, tt_ev("$r2", "$a"));
    f.s.handle_thread_reset_ui_("!r:x", "$root", [] {
        tesseract::EventList l;
        l.push_back(tt_ev("$x1", "$x0"));
        return l;
    }());
    CHECK(f.thread_ids() == Ids{"$x1"});
}

TEST_CASE("find-in-thread debounces, then searches the open thread",
          "[shell][thread_timeline][search]")
{
    TtFx f;
    f.s.handle_thread_search_query_("hello");
    f.s.handle_thread_search_query_("hello w");
    f.s.fire_delayed();
    CHECK(f.s.thread_search_request_id_ == 1);
    REQUIRE(f.s.thread_search_pending_.size() == 1);
    CHECK(f.s.thread_search_pending_.at(1) == "hello w");

    // Leaving the room before the debounce fires drops the request.
    f.s.handle_thread_search_query_("again");
    f.s.current_room_id_ = "!elsewhere:x";
    f.s.fire_delayed();
    CHECK(f.s.thread_search_request_id_ == 1);
}

TEST_CASE("thread search highlights and focuses matches in the thread list",
          "[shell][thread_timeline][search]")
{
    TtFx f;
    f.s.handle_thread_reset_ui_("!r:x", "$root", tt_list({"$a", "$b", "$c"}));
    f.s.thread_search_pending_[1] = "q";
    f.s.thread_search_request_id_ = 1;
    tesseract::SearchHit h1, h2;
    h1.event_id = "$a";
    h1.timestamp_ms = 1;
    h2.event_id = "$c";
    h2.timestamp_ms = 3;
    f.s.handle_thread_search_results_ui_(1, {h2, h1});
    CHECK(f.s.thread_search_current_ == 1); // newest
    CHECK(f.thread_list()->highlighted_event() == "$c");
    CHECK(f.thread_list()->search_match_ids().size() == 2);

    f.s.thread_search_navigate_(-1);
    CHECK(f.thread_list()->highlighted_event() == "$a");
    f.s.thread_search_navigate_(-1); // wraps
    CHECK(f.thread_list()->highlighted_event() == "$c");

    f.s.thread_search_clear_();
    CHECK(f.s.thread_search_matches_.empty());

    // A failure after clear just resets the (already empty) state.
    f.s.thread_search_pending_[2] = "q";
    f.s.thread_search_request_id_ = 2;
    f.s.handle_thread_search_failed_ui_(2, "x");
    CHECK_FALSE(f.thread_list()->has_search_matches());
}

TEST_CASE("the animation tick advances GIF frames and keeps running",
          "[shell][thread_timeline][anim]")
{
    TtShell s;
    const auto key = tk::CacheKey::media("mxc://hs/gif");
    std::vector<std::unique_ptr<tk::Image>> frames;
    frames.push_back(std::make_unique<TtImg>());
    frames.push_back(std::make_unique<TtImg>());
    s.account_manager_.anim_cache().store(key, std::move(frames), {10, 10}, 0);
    s.account_manager_.anim_cache().peek_frame(key, false); // mark visible

    s.now_ms = 50; // past the first frame's 10 ms delay
    CHECK(s.tick_anim_());
    CHECK(s.anim_repaints == 1);
    CHECK(s.anim_stops == 0);
}

TEST_CASE("the animation tick stops once nothing animated is visible",
          "[shell][thread_timeline][anim]")
{
    TtShell s;
    const auto key = tk::CacheKey::media("mxc://hs/gif");
    std::vector<std::unique_ptr<tk::Image>> frames;
    frames.push_back(std::make_unique<TtImg>());
    s.account_manager_.anim_cache().store(key, std::move(frames), {10}, 0);
    s.account_manager_.anim_cache().erase(key); // scrolled away and evicted
    CHECK_FALSE(s.tick_anim_());
    CHECK(s.anim_stops == 1);
}

TEST_CASE("search-index stats poll only while the settings panel is open",
          "[shell][thread_timeline][stats]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    TtShell s;
    auto settings = tk::create_root_widget<tesseract::views::SettingsView>(nullptr);
    s.stats_settings_view_ = settings.get();
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    s.client_ = &client;

    s.refresh_search_index_stats_(); // panel closed: nothing
    s.pool_.wait_idle(std::chrono::seconds(5));
    CHECK(s.delayed.empty());

    tesseract::Settings::instance().index_messages_for_search = true;
    s.start_search_index_stats_poll_();
    CHECK(s.search_stats_panel_open_);
    s.pool_.wait_idle(std::chrono::seconds(5));
    // Backfill not done on a sessionless client: the poll re-arms itself.
    CHECK_FALSE(s.delayed.empty());

    s.stop_search_index_stats_poll_();
    CHECK_FALSE(s.search_stats_panel_open_);
    s.fire_delayed(); // the cancelled debounce does nothing

    // Indexing off: one read, no re-arm.
    tesseract::Settings::instance().index_messages_for_search = false;
    s.delayed.clear();
    s.start_search_index_stats_poll_();
    s.pool_.wait_idle(std::chrono::seconds(5));
    s.stop_search_index_stats_poll_();
}

TEST_CASE("cache-size polling needs the About tab", "[shell][thread_timeline][stats]")
{
    TtShell s;
    s.refresh_cache_sizes_poll_(); // panel closed
    auto settings = tk::create_root_widget<tesseract::views::SettingsView>(nullptr);
    s.stats_settings_view_ = settings.get();
    s.search_stats_panel_open_ = true;
    s.refresh_cache_sizes_poll_(); // About tab not selected: cancels the poll
    SUCCEED();
}
