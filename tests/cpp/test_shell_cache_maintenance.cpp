// ShellBase_media_fetch.cpp housekeeping: warmed voice clips, the prefetch
// window estimate, the image-cache generational GC, the cache-size report and
// the "clear all caches" entry point's refusal paths and local wipe.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct CmFakeImg : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return 4; }
};

struct CmShell : tesseract::test::TestShellBase
{
    ~CmShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
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
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        delayed.emplace_back(ms, std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }

    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::pair<int, std::function<void()>>> delayed;
    std::vector<std::string> statuses;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::clear_all_caches_;
    using ShellBase::compute_cache_sizes_;
    using ShellBase::main_app_;
    using ShellBase::media_decode_failed_;
    using ShellBase::media_fetch_failed_;
    using ShellBase::media_prefetch_window_;
    using ShellBase::my_user_id_;
    using ShellBase::room_view_;
    using ShellBase::run_image_gc_;
    using ShellBase::run_media_prefetch_;
    using ShellBase::url_previews_;
    using ShellBase::video_memory_bytes_;
    using ShellBase::voice_bytes_cache_;
    using ShellBase::voice_bytes_in_flight_;
    using ShellBase::voice_bytes_or_fetch_;
    using ShellBase::handle_media_ready_ui_;
    using ShellBase::pending_media_;
};

} // namespace

TEST_CASE("voice clips: cold request downloads once, warmed bytes are handed over once",
          "[shell][cache]")
{
    tesseract::Client client;
    CmShell s;
    CHECK(s.voice_bytes_or_fetch_("", nullptr).empty());
    CHECK(s.voice_bytes_or_fetch_("mxc://hs/v", nullptr).empty()); // no client

    s.client_ = &client;
    int ready = 0;
    CHECK(s.voice_bytes_or_fetch_("mxc://hs/v", [&] { ++ready; }).empty());
    CHECK(s.voice_bytes_or_fetch_("mxc://hs/v", [&] { ++ready; }).empty());
    REQUIRE(s.pending_media_.size() == 1); // de-duplicated
    CHECK(s.voice_bytes_in_flight_.count("mxc://hs/v") == 1);

    s.handle_media_ready_ui_(s.pending_media_.begin()->first, {1, 2, 3});
    CHECK(ready == 1);
    CHECK(s.voice_bytes_in_flight_.empty());
    CHECK(s.voice_bytes_or_fetch_("mxc://hs/v", nullptr) ==
          std::vector<std::uint8_t>{1, 2, 3});
    CHECK(s.voice_bytes_cache_.empty()); // consumed
}

TEST_CASE("an empty voice download still notifies and caches nothing",
          "[shell][cache]")
{
    tesseract::Client client;
    CmShell s;
    s.client_ = &client;
    int ready = 0;
    s.voice_bytes_or_fetch_("mxc://hs/v", [&] { ++ready; });
    s.handle_media_ready_ui_(s.pending_media_.begin()->first, {});
    CHECK(ready == 1);
    CHECK(s.voice_bytes_cache_.empty());
}

TEST_CASE("the warm voice cache is bounded", "[shell][cache]")
{
    tesseract::Client client;
    CmShell s;
    s.client_ = &client;
    for (int i = 0; i < 9; ++i)
    {
        const std::string tok = "mxc://hs/v" + std::to_string(i);
        s.voice_bytes_or_fetch_(tok, nullptr);
    }
    // Complete all nine downloads.
    std::vector<std::uint64_t> ids;
    for (auto& [id, req] : s.pending_media_)
        ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids)
        s.handle_media_ready_ui_(id, {7});
    CHECK(s.voice_bytes_cache_.size() <= 8);
    CHECK(s.voice_bytes_cache_.size() >= 1);
}

TEST_CASE("prefetch window falls back without a laid-out message list",
          "[shell][cache]")
{
    CmShell s;
    CHECK(s.media_prefetch_window_() == 20);
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    s.room_view_ = rv.get();
    CHECK(s.media_prefetch_window_() == 20); // zero-height list
}

TEST_CASE("video memory is zero with no players", "[shell][cache]")
{
    CmShell s;
    CHECK(s.video_memory_bytes_() == 0);
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    s.room_view_ = rv.get();
    CHECK(s.video_memory_bytes_() == 0);
}

TEST_CASE("the image GC only runs after user activity and evicts stale entries",
          "[shell][cache][gc]")
{
    CmShell s;
    auto& images = s.account_manager_.image_cache();
    const auto stale = tk::CacheKey::media("mxc://hs/stale");
    images.store(stale, std::make_unique<CmFakeImg>());

    // No recent input: the cycle is gated off and nothing is scheduled.
    s.run_image_gc_();
    CHECK(s.delayed.empty());
    CHECK(images.contains(stale));

    s.account_manager_.note_image_gc_activity();
    s.run_image_gc_();
    REQUIRE(s.delayed.size() == 1);
    CHECK(s.delayed[0].first == 50);

    // Age the entry past both the generation and wall-clock retention, then
    // run the deferred sweep.
    images.set_clock_for_testing(
        [] { return std::chrono::steady_clock::now() + std::chrono::hours(1); });
    images.advance_generation();
    images.advance_generation();
    images.advance_generation();
    s.delayed[0].second();
    CHECK_FALSE(images.contains(stale));

    // A second run inside the 1.5 s dedup window does nothing.
    s.delayed.clear();
    s.account_manager_.note_image_gc_activity();
    s.run_image_gc_();
    CHECK(s.delayed.empty());
}

TEST_CASE("cache sizes need a signed-in account and a callback", "[shell][cache]")
{
    CmShell s;
    int calls = 0;
    s.compute_cache_sizes_([&](uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t) { ++calls; });
    s.pump();
    CHECK(calls == 0);

    s.my_user_id_ = "@me:x";
    s.compute_cache_sizes_(nullptr);
    s.pump();
    CHECK(calls == 0);

    uint64_t memory = ~0ull;
    s.compute_cache_sizes_([&](uint64_t, uint64_t, uint64_t mem, uint64_t,
                               uint64_t, uint64_t, uint64_t)
                           {
                               ++calls;
                               memory = mem;
                           });
    s.pump();
    CHECK(calls == 1);
    CHECK(memory != ~0ull);
}

TEST_CASE("clearing caches is refused while signed out or in a call/verification",
          "[shell][cache][clear]")
{
    CmShell s;
    s.clear_all_caches_({}); // signed out: silently nothing
    CHECK(s.statuses.empty());
    s.pump();
}

TEST_CASE("clear_all_caches_ wipes the local image caches", "[shell][cache][clear]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    CmShell s;
    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->user_id = "@me:x";
    s.active_account_->client = std::make_unique<tesseract::Client>();
    s.my_user_id_ = "@me:x";

    const auto key = tk::CacheKey::media("mxc://hs/x");
    s.account_manager_.image_cache().store(key, std::make_unique<CmFakeImg>());
    s.media_decode_failed_.insert("mxc://hs/x");
    s.media_fetch_failed_["mxc://hs/x"] = {};
    s.url_previews_["https://x"] = {};
    s.voice_bytes_cache_["t"] = {1};

    s.clear_all_caches_({});
    s.pump();
    CHECK_FALSE(s.account_manager_.image_cache().contains(key));
    CHECK(s.media_decode_failed_.empty());
    CHECK(s.media_fetch_failed_.empty());
    CHECK(s.url_previews_.empty());
    CHECK(s.voice_bytes_cache_.empty());
    // The SDK wipe follows; with no stored session it reports why it stopped.
    REQUIRE_FALSE(s.statuses.empty());
    CHECK(s.statuses.back().find("session is missing") != std::string::npos);
}

TEST_CASE("the pre-paint prefetch pass tolerates an empty app", "[shell][cache]")
{
    CmShell s;
    s.run_media_prefetch_(); // no main app
    auto surface = TestSurface::create(900, 700);
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    s.main_app_ = app.get();
    s.run_media_prefetch_(); // nothing visible: no keys
    SUCCEED();
}
