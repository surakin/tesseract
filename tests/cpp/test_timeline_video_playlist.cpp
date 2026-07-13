#include <catch2/catch_test_macros.hpp>

#include "views/TimelineVideoPlaylist.h"

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
