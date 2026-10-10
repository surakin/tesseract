// Scrub/speed/paint paths of TimelineMediaController that the playback tests
// in test_timeline_media_controller.cpp don't reach.

#include <catch2/catch_test_macros.hpp>

#include "tesseract/media_source.h"
#include "tk/audio.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"
#include "views/TimelineMediaController.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::views::MessageRowData;
using tesseract::views::TimelineMediaController;

namespace
{

struct TmcCardsPlayer : tk::AudioPlayer
{
    int plays = 0;
    float rate = 1.0f;
    bool playing = false;
    std::uint64_t pos = 0;
    std::uint64_t dur = 60000;
    std::vector<std::uint64_t> seeks;

    void play(const std::uint8_t*, std::size_t, std::string_view) override
    {
        ++plays;
        playing = true;
        pos = 0;
    }
    void pause() override { playing = false; }
    void resume() override { playing = true; }
    void stop() override
    {
        playing = false;
        pos = 0;
    }
    void seek(std::uint64_t ms) override
    {
        pos = ms;
        seeks.push_back(ms);
    }
    void set_playback_rate(float r) override { rate = r; }
    float playback_rate() const override { return rate; }
    std::uint64_t position_ms() const override { return pos; }
    std::uint64_t duration_ms() const override { return dur; }
    bool is_playing() const override { return playing; }
    bool reached_end() const override { return false; }
};

MessageRowData tmc_row(const std::string& id, MessageRowData::Kind kind, std::uint64_t dur_ms = 0)
{
    MessageRowData r;
    r.kind = kind;
    r.event_id = id;
    r.audio_source = tesseract::MediaSource::plain("mxc://x/" + id);
    r.audio_mime = "audio/ogg";
    r.duration_ms = dur_ms;
    return r;
}

struct TmcCardsFixture
{
    TimelineMediaController c;
    TmcCardsPlayer* player;
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 200);
    int repaints = 0;
    tk::IconCache icon;
    std::vector<std::uint8_t> bytes{1, 2, 3};

    TmcCardsFixture()
    {
        auto p = std::make_unique<TmcCardsPlayer>();
        player = p.get();
        c.set_player(std::move(p));
        c.set_bytes_provider([this](const std::string&) { return bytes; });
        c.set_repaint([this] { ++repaints; });
    }
    tk::PaintCtx pc()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
    }
};

} // namespace

TEST_CASE("Voice card paint records geometry used for scrubbing", "[media][voice][cards]")
{
    TmcCardsFixture f;
    auto row = tmc_row("$v", MessageRowData::Kind::Voice, 20000);
    row.waveform = {10, 200, 400, 800, 1000, 50, 0, 700};
    auto pc = f.pc();
    f.c.paint_voice_card(row, pc, {10, 10, 300, 48}, f.icon);
    const auto* g = f.c.voice_geom_at("$v");
    REQUIRE(g != nullptr);
    CHECK(g->waveform_strip.w > 0);
    CHECK(g->play_button.w > 0);
    CHECK(g->speed_pill.w == 0); // the pill only appears for the active clip
    CHECK(f.c.voice_geom().size() == 1);

    // Scrub to the middle of an unloaded clip: loads and seeks to ~50%.
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x + g->waveform_strip.w * 0.5f);
    CHECK(f.player->plays == 1);
    CHECK(f.c.playing_event_id() == "$v");
    REQUIRE_FALSE(f.player->seeks.empty());
    CHECK(f.player->seeks.back() == 10000);

    // Scrubbing the loaded clip just seeks; out-of-range x clamps.
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x - 500);
    CHECK(f.player->seeks.back() == 0);
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x + g->waveform_strip.w + 500);
    CHECK(f.player->seeks.back() == 20000);
    CHECK(f.repaints >= 2);

    // Painting while playing and again with a different elapsed fraction.
    f.player->pos = 5000;
    f.c.on_audio_progress();
    f.c.paint_voice_card(row, pc, {10, 10, 300, 48}, f.icon);
    REQUIRE(f.c.voice_geom_at("$v") != nullptr);
    CHECK(f.c.voice_geom_at("$v")->speed_pill.w > 0);

    f.c.clear_geometry();
    CHECK(f.c.voice_geom_at("$v") == nullptr);
    CHECK(f.c.audio_geom_at("$v") == nullptr);
}

TEST_CASE("Voice scrub guards: no geometry, unknown duration, cache miss",
          "[media][voice][cards]")
{
    TmcCardsFixture f;
    auto row = tmc_row("$v", MessageRowData::Kind::Voice, 0);
    f.c.handle_voice_scrub_at(row, 10); // no geometry painted yet
    CHECK(f.player->plays == 0);

    auto pc = f.pc();
    f.c.paint_voice_card(row, pc, {10, 10, 300, 48}, f.icon);
    const auto* g = f.c.voice_geom_at("$v");
    REQUIRE(g != nullptr);
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x + 5); // duration unknown
    CHECK(f.player->plays == 0);

    row.duration_ms = 10000;
    f.bytes.clear(); // cache miss: only a repaint
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x + 5);
    CHECK(f.player->plays == 0);
    CHECK(f.repaints >= 1);

    // Scrub from another clip stops the playing one first.
    f.bytes = {1};
    f.c.handle_voice_play_click(tmc_row("$other", MessageRowData::Kind::Voice, 5000));
    CHECK(f.c.playing_event_id() == "$other");
    f.c.handle_voice_scrub_at(row, g->waveform_strip.x + 5);
    CHECK(f.c.playing_event_id() == "$v");

    TimelineMediaController bare; // no player/provider: every entry point is inert
    bare.handle_voice_scrub_at(row, 1);
    bare.handle_voice_speed_click();
    bare.handle_audio_scrub_at(row, 1);
    bare.handle_audio_play_click(row);
}

TEST_CASE("Voice speed pill cycles 1x -> 1.5x -> 2x -> 1x", "[media][voice][cards]")
{
    TmcCardsFixture f;
    f.c.handle_voice_speed_click();
    CHECK(f.player->rate == 1.5f);
    f.c.handle_voice_speed_click();
    CHECK(f.player->rate == 2.0f);
    f.c.handle_voice_speed_click();
    CHECK(f.player->rate == 1.0f);
    // A subsequent voice play uses the chosen rate; audio always plays at 1x.
    f.c.handle_voice_speed_click();
    f.c.handle_voice_play_click(tmc_row("$v", MessageRowData::Kind::Voice, 5000));
    CHECK(f.player->rate == 1.5f);
    f.c.handle_audio_play_click(tmc_row("$a", MessageRowData::Kind::Audio, 5000));
    CHECK(f.player->rate == 1.0f);
}

TEST_CASE("Audio card paint, play/pause toggle and linear scrub",
          "[media][audio][cards]")
{
    TmcCardsFixture f;
    auto row = tmc_row("$a", MessageRowData::Kind::Audio, 30000);
    row.file_name = "song.mp3";
    row.file_size = 3 * 1024 * 1024;
    auto pc = f.pc();
    f.c.paint_audio_card(row, pc, {10, 10, 320, 56}, f.icon);
    const auto* g = f.c.audio_geom_at("$a");
    REQUIRE(g != nullptr);
    CHECK(g->progress_track.w > 0);

    f.c.handle_audio_play_click(row);
    CHECK(f.player->plays == 1);
    f.c.handle_audio_play_click(row); // toggles to paused
    CHECK_FALSE(f.player->playing);
    f.c.handle_audio_play_click(row); // resumes
    CHECK(f.player->playing);

    f.player->pos = 12000;
    f.c.on_audio_progress();
    f.c.paint_audio_card(row, pc, {10, 10, 320, 56}, f.icon); // with progress

    f.c.handle_audio_scrub_at(row, g->progress_track.x + g->progress_track.w * 0.25f);
    CHECK(f.player->seeks.back() == 7500);

    // Scrub on a not-yet-loaded clip loads it first; unknown duration is a no-op.
    auto other = tmc_row("$b", MessageRowData::Kind::Audio, 0);
    f.c.paint_audio_card(other, pc, {10, 80, 320, 56}, f.icon);
    const auto* g2 = f.c.audio_geom_at("$b");
    REQUIRE(g2 != nullptr);
    const int plays = f.player->plays;
    f.c.handle_audio_scrub_at(other, g2->progress_track.x + 3);
    CHECK(f.player->plays == plays);
    other.duration_ms = 40000;
    f.c.handle_audio_scrub_at(other, g2->progress_track.x + g2->progress_track.w * 0.5f);
    CHECK(f.player->plays == plays + 1);
    CHECK(f.c.playing_event_id() == "$b");

    f.bytes.clear();
    auto third = tmc_row("$c", MessageRowData::Kind::Audio, 40000);
    f.c.paint_audio_card(third, pc, {10, 150, 320, 40}, f.icon);
    const auto* g3 = f.c.audio_geom_at("$c");
    REQUIRE(g3 != nullptr);
    f.c.handle_audio_scrub_at(third, g3->progress_track.x + 10); // cache miss
    CHECK(f.c.playing_event_id() == "$b");
    f.c.handle_audio_scrub_at(tmc_row("$nogeom", MessageRowData::Kind::Audio, 1000), 5);

    // Voice/audio click on a cold cache arms a pending audio play (rate 1).
    f.c.handle_audio_play_click(third);
    CHECK(f.c.has_pending_play());
    CHECK(f.c.playing_event_id().empty());
    f.bytes = {7};
    f.c.retry_pending_voice_play();
    CHECK(f.c.playing_event_id() == "$c");
}
