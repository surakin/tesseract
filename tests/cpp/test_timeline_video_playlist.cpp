#include <catch2/catch_test_macros.hpp>

#include "views/TimelineVideoPlaylist.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using tesseract::views::TimelineVideoPlaylist;
using tesseract::views::VideoSourceInfo;

namespace
{

class FakeVideoPlayer : public tk::VideoPlayer
{
public:
    void play(const std::uint8_t*, std::size_t, std::string_view) override
    {
        playing_ = true;
        ++play_count;
    }
    void pause() override
    {
        playing_ = false;
        ++pause_count;
    }
    void resume() override
    {
        playing_ = true;
        ++resume_count;
    }
    void stop() override
    {
        playing_ = false;
    }
    void seek(std::uint64_t) override {}
    void set_playback_rate(float) override {}
    float playback_rate() const override { return 1.0f; }
    std::uint64_t position_ms() const override { return 0; }
    std::uint64_t duration_ms() const override { return 0; }
    bool is_playing() const override { return playing_; }
    const tk::Image* current_frame() const override { return nullptr; }
    std::size_t memory_bytes() const override { return 1000; }

    bool playing_ = false;
    int play_count = 0;
    int pause_count = 0;
    int resume_count = 0;
};

// Wires TimelineVideoPlaylist to FakeVideoPlayer instances (borrowed pointers
// kept in `players`, in creation order) with a fetch provider that resolves
// synchronously, so ensure_playing() fully completes (including its play())
// within the call.
struct PlaylistStage
{
    TimelineVideoPlaylist playlist;
    std::vector<FakeVideoPlayer*> players;

    PlaylistStage()
    {
        playlist.set_player_factory(
            [this]() -> std::unique_ptr<tk::VideoPlayer>
            {
                auto p = std::make_unique<FakeVideoPlayer>();
                players.push_back(p.get());
                return p;
            });
        playlist.set_fetch_provider(
            [](const std::string&,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                on_ready({1, 2, 3});
            });
    }

    void add_playing(const std::string& event_id)
    {
        VideoSourceInfo info;
        info.event_id = event_id;
        info.autoplay = true;
        playlist.ensure_playing(info);
    }
};

} // namespace

TEST_CASE("TimelineVideoPlaylist pause_all pauses every live playing player",
          "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");
    stage.add_playing("$b");
    REQUIRE(stage.players.size() == 2);
    CHECK(stage.players[0]->playing_);
    CHECK(stage.players[1]->playing_);

    stage.playlist.pause_all();

    CHECK_FALSE(stage.players[0]->playing_);
    CHECK_FALSE(stage.players[1]->playing_);
    CHECK(stage.players[0]->pause_count == 1);
    CHECK(stage.players[1]->pause_count == 1);
}

TEST_CASE(
    "TimelineVideoPlaylist pause_all does not re-pause an already-paused "
    "(non-autoplay) player, and resume_all leaves it paused",
    "[views][video]")
{
    PlaylistStage stage;
    VideoSourceInfo info;
    info.event_id = "$c";
    info.autoplay = false; // ensure_playing's fetch callback pauses it right
                            // after play(), so it's already paused.
    stage.playlist.ensure_playing(info);
    REQUIRE(stage.players.size() == 1);
    CHECK_FALSE(stage.players[0]->playing_);
    const int pause_count_before = stage.players[0]->pause_count;

    stage.playlist.pause_all();
    CHECK(stage.players[0]->pause_count == pause_count_before);

    // It wasn't actually playing when "suspended", so resume_all() — even
    // with a predicate that accepts everything — must not start it.
    stage.playlist.resume_all([](const std::string&) { return true; });
    CHECK_FALSE(stage.players[0]->playing_);
    CHECK(stage.players[0]->resume_count == 0);
}

TEST_CASE(
    "TimelineVideoPlaylist resume_all only resumes players whose row is "
    "visible",
    "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");
    stage.add_playing("$b");
    stage.playlist.pause_all();

    stage.playlist.resume_all(
        [](const std::string& event_id) { return event_id == "$a"; });

    CHECK(stage.players[0]->playing_);
    CHECK(stage.players[0]->resume_count == 1);
    CHECK_FALSE(stage.players[1]->playing_);
    CHECK(stage.players[1]->resume_count == 0);
}

TEST_CASE(
    "TimelineVideoPlaylist resume_all is a no-op without a prior pause_all",
    "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");

    stage.playlist.resume_all([](const std::string&) { return true; });

    CHECK(stage.players[0]->resume_count == 0);
}

TEST_CASE("TimelineVideoPlaylist pause_all does not touch the retired pool",
          "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");
    stage.playlist.drop("$a"); // retires it (already paused by retire_())
    REQUIRE(stage.players.size() == 1);
    const int pause_count_before = stage.players[0]->pause_count;

    // players_ is now empty (the entry moved to the retired pool) — pause_all
    // must not reach into the retired pool and re-pause it.
    stage.playlist.pause_all();

    CHECK(stage.players[0]->pause_count == pause_count_before);
}

TEST_CASE("TimelineVideoPlaylist GC retires players no painted row touched",
          "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$seen");
    stage.add_playing("$unseen");
    REQUIRE(stage.playlist.size() == 2);

    for (int i = 0; i < 2; ++i)
    {
        stage.playlist.advance_generation();
        stage.playlist.touch("$seen");
    }
    stage.playlist.retire_unseen(2);

    CHECK(stage.playlist.has("$seen"));
    CHECK_FALSE(stage.playlist.has("$unseen"));
    CHECK(stage.playlist.retired_count() == 1);
    CHECK_FALSE(stage.players[1]->playing_); // retired players are paused
}

TEST_CASE("TimelineVideoPlaylist touch wakes a GC-retired player",
          "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");
    stage.playlist.advance_generation();
    stage.playlist.advance_generation();
    stage.playlist.retire_unseen(2);
    REQUIRE_FALSE(stage.playlist.has("$a"));

    stage.playlist.touch("$a"); // row scrolled back into view

    CHECK(stage.playlist.has("$a"));
    CHECK(stage.players.size() == 1);        // reclaimed from the pool,
    CHECK(stage.players[0]->resume_count == 1); // not re-created or re-fetched
    CHECK(stage.players[0]->play_count == 1);
    CHECK(stage.playlist.retired_count() == 0);
}

TEST_CASE("TimelineVideoPlaylist release_idle_retired frees only aged entries",
          "[views][video]")
{
    using namespace std::chrono;
    PlaylistStage stage;
    steady_clock::time_point now{};
    stage.playlist.set_clock_for_testing([&] { return now; });

    stage.add_playing("$old");
    stage.add_playing("$new"); // both live, so neither reuses a pooled player
    stage.playlist.drop("$old");
    now += seconds{20};
    stage.playlist.drop("$new");
    REQUIRE(stage.playlist.retired_count() == 2);

    now += seconds{15}; // $old is 35s idle, $new 15s
    stage.playlist.release_idle_retired(seconds{30});
    CHECK(stage.playlist.retired_count() == 1);
}

TEST_CASE("TimelineVideoPlaylist drop forgets a GC-retired row",
          "[views][video]")
{
    PlaylistStage stage;
    stage.add_playing("$a");
    stage.playlist.advance_generation();
    stage.playlist.advance_generation();
    stage.playlist.retire_unseen(2);
    stage.playlist.drop("$a"); // row removed from the timeline

    stage.playlist.touch("$a");
    CHECK_FALSE(stage.playlist.has("$a"));
}

TEST_CASE("TimelineVideoPlaylist memory_bytes counts live and retired players",
          "[views][video]")
{
    using namespace std::chrono;
    PlaylistStage stage;
    steady_clock::time_point now{};
    stage.playlist.set_clock_for_testing([&] { return now; });

    stage.add_playing("$a");
    stage.add_playing("$b");
    CHECK(stage.playlist.memory_bytes() == 2000);

    stage.playlist.drop("$a"); // moves to the retired pool, still resident
    CHECK(stage.playlist.memory_bytes() == 2000);

    now += hours{1};
    stage.playlist.release_idle_retired(seconds{30});
    CHECK(stage.playlist.memory_bytes() == 1000);
}
