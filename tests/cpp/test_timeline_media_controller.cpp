// Tests for TimelineMediaController playback, focused on the single-click
// behavior: a play click on a clip whose bytes aren't cached yet must arm a
// pending play that auto-starts once the background fetch warms the cache —
// the user should not have to click twice.

#include <catch2/catch_test_macros.hpp>

#include "tesseract/media_source.h"
#include "tk/audio.h"
#include "views/MessageListView.h"
#include "views/TimelineMediaController.h"

using tesseract::views::MessageRowData;
using tesseract::views::TimelineMediaController;

namespace
{

// Minimal AudioPlayer fake: records the last play() call so tests can assert
// playback actually started, with what mime and at what rate.
struct FakeAudioPlayer : tk::AudioPlayer
{
    int           play_count = 0;
    std::size_t   last_size  = 0;
    std::string   last_mime;
    float         rate       = 1.0f;
    bool          playing    = false;
    std::uint64_t pos        = 0;
    // Mirrors real backends (e.g. Qt's QMediaPlayer): natural completion does
    // NOT reset pos to 0 — it stays at/near the clip's duration. `ended` is
    // the backend-native "genuinely reached the end" signal instead.
    bool          ended      = false;

    void play(const std::uint8_t*, std::size_t size, std::string_view mime) override
    {
        ++play_count;
        last_size = size;
        last_mime = std::string(mime);
        playing   = true;
        pos       = 0;
        ended     = false;
    }
    void pause() override { playing = false; }
    void resume() override
    {
        playing = true;
        ended   = false;
    }
    // Real backends (e.g. Qt's QMediaPlayer on a same-thread direct
    // connection) commonly emit their state-change signal synchronously
    // from within stop(). Mirror that here so tests can exercise the
    // reentrancy guard in TimelineMediaController::on_audio_progress().
    void stop() override
    {
        playing = false;
        pos     = 0;
        ended   = false;
        if (on_progress)
        {
            on_progress();
        }
    }
    void          seek(std::uint64_t ms) override { pos = ms; ended = false; }
    void          set_playback_rate(float r) override { rate = r; }
    float         playback_rate() const override { return rate; }
    std::uint64_t position_ms() const override { return pos; }
    std::uint64_t duration_ms() const override { return 0; }
    bool          is_playing() const override { return playing; }
    bool          reached_end() const override { return ended; }
};

MessageRowData make_voice_row(const std::string& event_id, const std::string& mxc)
{
    MessageRowData row;
    row.kind         = MessageRowData::Kind::Voice;
    row.event_id     = event_id;
    row.audio_source = tesseract::MediaSource::plain(mxc);
    row.audio_mime   = "audio/ogg";
    return row;
}

MessageRowData make_audio_row(const std::string& event_id, const std::string& mxc)
{
    MessageRowData row;
    row.kind         = MessageRowData::Kind::Audio;
    row.event_id     = event_id;
    row.audio_source = tesseract::MediaSource::plain(mxc);
    row.audio_mime   = "audio/mpeg";
    return row;
}

} // namespace

TEST_CASE("Voice: cache miss arms a pending play, retry starts playback on arrival",
          "[media][voice]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));

    // Provider simulates the warm cache: empty until bytes "arrive".
    std::vector<std::uint8_t> warm;
    controller.set_bytes_provider(
        [&warm](const std::string&) { return warm; });

    MessageRowData row = make_voice_row("$ev1", "mxc://example.org/voice");

    // First click: cold cache → no playback, but pending is armed.
    controller.handle_voice_play_click(row);
    CHECK(fake->play_count == 0);
    CHECK(controller.playing_event_id().empty());

    // A relayout before bytes land must not start playback.
    controller.retry_pending_voice_play();
    CHECK(fake->play_count == 0);

    // Bytes arrive; the relayout-driven retry now starts playback — no second
    // click required.
    warm = {1, 2, 3, 4};
    controller.retry_pending_voice_play();
    CHECK(fake->play_count == 1);
    CHECK(fake->last_size == 4);
    CHECK(fake->last_mime == "audio/ogg");
    CHECK(controller.playing_event_id() == "$ev1");

    // Pending was consumed: a further retry is a no-op.
    controller.retry_pending_voice_play();
    CHECK(fake->play_count == 1);
}

TEST_CASE("Voice: warm cache plays on the first click", "[media][voice]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{9, 9}; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    CHECK(fake->play_count == 1);
    CHECK(controller.playing_event_id() == "$ev1");
}

TEST_CASE("Audio: cache miss arms a pending play at rate 1.0", "[media][audio]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    fake->rate                     = 2.0f; // ensure retry forces 1.0 for plain audio
    controller.set_player(std::move(player));

    std::vector<std::uint8_t> warm;
    controller.set_bytes_provider([&warm](const std::string&) { return warm; });

    controller.handle_audio_play_click(make_audio_row("$aud", "mxc://x/aud"));
    CHECK(fake->play_count == 0);

    warm = {7};
    controller.retry_pending_voice_play();
    CHECK(fake->play_count == 1);
    CHECK(fake->last_mime == "audio/mpeg");
    CHECK(fake->rate == 1.0f);
    CHECK(controller.playing_event_id() == "$aud");
}

TEST_CASE("reset_pending_play discards an armed play (room switch)", "[media]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));

    std::vector<std::uint8_t> warm;
    controller.set_bytes_provider([&warm](const std::string&) { return warm; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    controller.reset_pending_play();

    // Even after bytes arrive, nothing plays — the pending play was discarded.
    warm = {1, 2, 3};
    controller.retry_pending_voice_play();
    CHECK(fake->play_count == 0);
    CHECK(controller.playing_event_id().empty());
}

TEST_CASE("stop_active_playback stops a clip actively playing (room switch)",
          "[media]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{9, 9}; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    REQUIRE(fake->playing);
    REQUIRE(controller.playing_event_id() == "$ev1");

    controller.stop_active_playback();

    CHECK_FALSE(fake->playing);
    CHECK(controller.playing_event_id().empty());
}

TEST_CASE("stop_active_playback is a no-op when nothing is playing", "[media]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));

    controller.stop_active_playback();

    CHECK(fake->play_count == 0);
    CHECK(controller.playing_event_id().empty());
}

TEST_CASE("Voice: natural finish auto-advances via the next-voice lookup",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{1, 2, 3}; });

    MessageRowData next_row = make_voice_row("$ev2", "mxc://x/2");
    int            lookup_calls = 0;
    controller.set_next_voice_lookup(
        [&](const std::string& finished_event_id) -> const MessageRowData*
        {
            ++lookup_calls;
            CHECK(finished_event_id == "$ev1");
            return &next_row;
        });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    CHECK(fake->play_count == 1);

    // Confirm the clip actually started (playing_ever_active_) before the
    // backend reports natural end-of-media.
    fake->playing = true;
    controller.on_audio_progress();

    // Real backends (e.g. Qt's QMediaPlayer) leave position_ms() at/near the
    // clip's duration on natural completion rather than resetting it to 0 —
    // reached_end() is the only reliable signal in that case.
    fake->playing = false;
    fake->pos     = 4200;
    fake->ended   = true;
    controller.on_audio_progress();

    CHECK(lookup_calls == 1);
    CHECK(fake->play_count == 2);
    CHECK(controller.playing_event_id() == "$ev2");
}

TEST_CASE("Voice: natural finish with no next match plays nothing further",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{1, 2, 3}; });
    controller.set_next_voice_lookup(
        [](const std::string&) -> const MessageRowData* { return nullptr; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    fake->playing = true;
    controller.on_audio_progress();

    fake->playing = false;
    fake->pos     = 4200;
    fake->ended   = true;
    controller.on_audio_progress();

    CHECK(fake->play_count == 1);
    CHECK(controller.playing_event_id().empty());
}

TEST_CASE("Voice: a pause near the end (not reached_end) does not auto-advance",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{1, 2, 3}; });

    int lookup_calls = 0;
    controller.set_next_voice_lookup(
        [&](const std::string&) -> const MessageRowData*
        {
            ++lookup_calls;
            return nullptr;
        });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    fake->playing = true;
    controller.on_audio_progress();

    // User pauses a moment before the clip's actual end: not playing, a
    // non-zero position, and the backend has NOT signaled reached_end().
    // This must not be treated as "finished".
    fake->playing = false;
    fake->pos     = 4200;
    controller.on_audio_progress();

    CHECK(lookup_calls == 0);
    CHECK(fake->play_count == 1);
    CHECK(controller.playing_event_id() == "$ev1");
}

TEST_CASE("Voice: manually switching rows does not invoke the next-voice lookup",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    FakeAudioPlayer*        fake   = player.get();
    controller.set_player(std::move(player));
    controller.set_bytes_provider(
        [](const std::string&) { return std::vector<std::uint8_t>{1, 2, 3}; });

    int lookup_calls = 0;
    controller.set_next_voice_lookup(
        [&](const std::string&) -> const MessageRowData*
        {
            ++lookup_calls;
            return nullptr;
        });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    fake->playing = true;
    controller.on_audio_progress();

    // User clicks a different row while $ev1 is still active: this calls
    // stop() internally (fake->playing=false, pos=0) before starting $ev2.
    // The reentrant on_audio_progress() this can trigger must not treat the
    // switch as a natural finish.
    controller.handle_voice_play_click(make_voice_row("$ev2", "mxc://x/2"));

    CHECK(lookup_calls == 0);
    CHECK(fake->play_count == 2);
    CHECK(controller.playing_event_id() == "$ev2");
}

TEST_CASE("Voice: auto-advance cache miss tags the pending play to skip "
          "the visibility gate",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    controller.set_player(std::make_unique<FakeAudioPlayer>());

    std::vector<std::uint8_t> warm;
    controller.set_bytes_provider([&warm](const std::string&) { return warm; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"),
                                       /*is_auto_advance=*/true);
    CHECK(controller.has_pending_play());
    CHECK(controller.pending_play_skip_visibility_gate());

    controller.reset_pending_play();
    CHECK_FALSE(controller.pending_play_skip_visibility_gate());
}

TEST_CASE("Voice: a manual click's cache miss does not skip the visibility gate",
          "[media][voice][autoadvance]")
{
    TimelineMediaController controller;
    auto                    player = std::make_unique<FakeAudioPlayer>();
    controller.set_player(std::move(player));

    std::vector<std::uint8_t> warm;
    controller.set_bytes_provider([&warm](const std::string&) { return warm; });

    controller.handle_voice_play_click(make_voice_row("$ev1", "mxc://x/1"));
    CHECK(controller.has_pending_play());
    CHECK_FALSE(controller.pending_play_skip_visibility_gate());
}
