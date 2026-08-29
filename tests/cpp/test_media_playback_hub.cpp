#include <catch2/catch_test_macros.hpp>

#include "app/MediaPlaybackHub.h"

using tesseract::MediaPlaybackHub;
using tesseract::NowPlaying;

namespace
{
NowPlaying playing(const std::string& event_id, bool is_playing = true)
{
    NowPlaying np;
    np.kind = NowPlaying::Kind::Voice;
    np.room_id = "!r:x";
    np.event_id = event_id;
    np.title = "clip";
    np.is_playing = is_playing;
    return np;
}
} // namespace

TEST_CASE("report populates current() and dispatches controls",
          "[media_playback_hub]")
{
    MediaPlaybackHub hub;
    int play_calls = 0;
    hub.report(playing("$a"), {.play = [&] { ++play_calls; }});

    CHECK(hub.current().event_id == "$a");
    CHECK(hub.current().is_playing);
    hub.play();
    CHECK(play_calls == 1);
}

TEST_CASE("report_stopped clears current() and controls", "[media_playback_hub]")
{
    MediaPlaybackHub hub;
    int play_calls = 0;
    hub.report(playing("$a"), {.play = [&] { ++play_calls; }});
    hub.report_stopped();

    CHECK(hub.current().kind == NowPlaying::Kind::None);
    CHECK(hub.current().event_id.empty());
    hub.play(); // controls were cleared too — must not call the stale lambda
    CHECK(play_calls == 0);
}

TEST_CASE("on_changed fires on track change and play/pause, not on position ticks",
          "[media_playback_hub]")
{
    MediaPlaybackHub hub;
    int changed = 0;
    hub.on_changed = [&] { ++changed; };

    hub.report(playing("$a"));
    CHECK(changed == 1);

    // Same track, same is_playing, only position advanced — no signal.
    NowPlaying tick = playing("$a");
    tick.position_ms = 500;
    hub.report(tick);
    CHECK(changed == 1);

    // Pause (is_playing flips) — signal.
    hub.report(playing("$a", /*is_playing=*/false));
    CHECK(changed == 2);

    // New track — signal.
    hub.report(playing("$b"));
    CHECK(changed == 3);
}

TEST_CASE("on_changed fires on report_stopped only if something was playing",
          "[media_playback_hub]")
{
    MediaPlaybackHub hub;
    int changed = 0;
    hub.on_changed = [&] { ++changed; };

    hub.report_stopped(); // nothing was playing — no signal
    CHECK(changed == 0);

    hub.report(playing("$a"));
    CHECK(changed == 1);

    hub.report_stopped();
    CHECK(changed == 2);
}

TEST_CASE("seek/pause/play_pause/stop no-op when no controls are set",
          "[media_playback_hub]")
{
    MediaPlaybackHub hub;
    hub.report(playing("$a")); // no Controls fields set
    hub.play();
    hub.pause();
    hub.play_pause();
    hub.stop();
    hub.seek(1000);
    // Reaching here without crashing is the assertion.
    SUCCEED();
}
