// Audio and file pages of MediaViewerOverlay, the viewer-wide on_playback_started
// hook and mixed-kind sequences.

#include <catch2/catch_test_macros.hpp>

#include "fake_media_players.h"
#include "media_viewer_test_items.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::test::FakeAudioPlayer;
using tesseract::test::FakeVideoPlayer;
using tesseract::views::MediaViewerItem;
using tesseract::views::MediaViewerOverlay;
using tesseract::views::test::image_item;
using tesseract::views::test::video_item;

namespace
{

struct AfStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(600, 400);

    void run(Widget& root, Rect bounds = {0, 0, 600, 400})
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

KeyEvent af_key(Key k)
{
    KeyEvent e{};
    e.key = k;
    return e;
}

MediaViewerItem audio_item(std::string source = "mxc://x/aud", std::string filename = "song.ogg",
                           std::string mime = "audio/ogg", std::uint64_t duration_ms = 8000)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Audio;
    item.source = std::move(source);
    item.filename = std::move(filename);
    item.mime_type = std::move(mime);
    item.duration_ms = duration_ms;
    item.waveform = {100, 400, 900, 300, 50};
    return item;
}

MediaViewerItem file_item(std::string source = "mxc://x/file", std::string filename = "report.pdf")
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::File;
    item.source = std::move(source);
    item.filename = std::move(filename);
    item.mime_type = "application/pdf";
    item.file_size = 2048;
    return item;
}

void click_btn(MediaViewerOverlay& overlay, Button* btn)
{
    REQUIRE(btn->visible());
    const Rect r = btn->bounds();
    const Point p{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    Widget* claimed = overlay.dispatch_pointer_down(p);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(p), true);
}

struct AudioFx
{
    AfStage st;
    MediaViewerOverlay overlay;
    FakeAudioPlayer* fake = nullptr;
    int started = 0;

    AudioFx()
    {
        auto p = std::make_unique<FakeAudioPlayer>();
        fake = p.get();
        overlay.set_audio_player(std::move(p));
        overlay.on_playback_started = [this] { ++started; };
    }
};

} // namespace

// ── audio page ────────────────────────────────────────────────────────────

TEST_CASE("audio page loads, plays and toggles with Space", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open(audio_item());
    f.st.run(f.overlay);
    CHECK(f.overlay.audio_loading_for_test());
    CHECK_FALSE(f.overlay.audio_play_btn_for_test()->visible());
    CHECK_FALSE(f.overlay.copy_btn_for_test()->visible());

    // Nothing to toggle until the bytes arrive.
    CHECK_FALSE(f.overlay.on_key_down(af_key(Key::Space)));
    CHECK(f.fake->play_count == 0);

    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2, 3, 4});
    f.st.run(f.overlay);
    CHECK_FALSE(f.overlay.audio_loading_for_test());
    CHECK(f.fake->play_count == 1);
    CHECK(f.fake->last_size == 4);
    CHECK(f.fake->last_mime == "audio/ogg");
    CHECK(f.started == 1);
    CHECK(f.overlay.audio_player_for_test() == f.fake);
    CHECK(f.overlay.audio_play_btn_for_test()->visible());
    CHECK(f.overlay.audio_play_btn_for_test()->access_name() == "Pause");
    CHECK(f.overlay.memory_bytes() >= 4);

    CHECK(f.overlay.on_key_down(af_key(Key::Space)));
    CHECK(f.fake->pause_count == 1);
    f.st.run(f.overlay);
    CHECK(f.overlay.audio_play_btn_for_test()->access_name() == "Play");

    CHECK(f.overlay.on_key_down(af_key(Key::Space)));
    CHECK(f.fake->resume_count == 1);
    CHECK(f.started == 2);

    CHECK(f.overlay.on_key_down(af_key(Key::Home)));
    CHECK(f.fake->last_seek == 0);
    CHECK(f.fake->seek_count == 1);
}

TEST_CASE("audio play button restarts a finished clip", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open(audio_item());
    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2});
    f.st.run(f.overlay);
    f.fake->playing_ = false;
    f.fake->ended = true;
    f.fake->position = 8000;
    f.st.run(f.overlay);
    CHECK(f.overlay.audio_play_btn_for_test()->access_name() == "Play");
    click_btn(f.overlay, f.overlay.audio_play_btn_for_test());
    CHECK(f.fake->last_seek == 0);
    CHECK(f.fake->resume_count == 1);
}

TEST_CASE("audio page: Left and Right navigate instead of being consumed",
          "[mediaviewer][audio][sequence]")
{
    AudioFx f;
    f.overlay.open_sequence({audio_item(), image_item("mxc://x/i", "", "cap", 640, 360)}, 0);
    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2});
    f.st.run(f.overlay);
    const int stops = f.fake->stop_count;

    CHECK(f.overlay.on_key_down(af_key(Key::Right)));
    CHECK(f.overlay.index() == 1);
    CHECK(f.fake->stop_count > stops); // deactivate stops playback
    CHECK_FALSE(f.fake->playing_);

    CHECK(f.overlay.on_key_down(af_key(Key::Left)));
    CHECK(f.overlay.index() == 0);
}

TEST_CASE("closing the viewer stops audio playback", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open(audio_item());
    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2});
    REQUIRE(f.fake->playing_);
    f.overlay.close();
    CHECK_FALSE(f.fake->playing_);
    CHECK(f.fake->stop_count >= 1);
}

TEST_CASE("audio bytes carrying a stale token are ignored", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open_sequence({audio_item("mxc://x/a1"), audio_item("mxc://x/a2")}, 0);
    const auto old_token = f.overlay.load_token();
    f.overlay.on_key_down(af_key(Key::PageDown));
    REQUIRE(f.overlay.load_token() > old_token);

    f.overlay.load_audio_bytes(old_token, {1, 2, 3});
    CHECK(f.fake->play_count == 0);
    CHECK(f.overlay.audio_loading_for_test());

    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2, 3});
    CHECK(f.fake->play_count == 1);

    // Closed: the next token bump drops late arrivals.
    const auto token = f.overlay.load_token();
    f.overlay.close();
    f.overlay.load_audio_bytes(token, {9});
    CHECK(f.fake->play_count == 1);
}

TEST_CASE("an empty audio fetch shows an error and never plays", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open(audio_item());
    f.overlay.load_audio_bytes(f.overlay.load_token(), {});
    f.st.run(f.overlay);
    CHECK(f.overlay.audio_error_for_test());
    CHECK_FALSE(f.overlay.audio_loading_for_test());
    CHECK(f.fake->play_count == 0);
    CHECK_FALSE(f.overlay.audio_play_btn_for_test()->visible());
}

TEST_CASE("a playback failure after loading shows the error state", "[mediaviewer][audio]")
{
    AudioFx f;
    f.overlay.open(audio_item());
    f.overlay.load_audio_bytes(f.overlay.load_token(), {1, 2, 3, 4});
    f.st.run(f.overlay);
    REQUIRE_FALSE(f.overlay.audio_error_for_test());
    REQUIRE(f.overlay.audio_play_btn_for_test()->visible());

    f.fake->fail();
    f.st.run(f.overlay);
    CHECK(f.overlay.audio_error_for_test());
    CHECK_FALSE(f.overlay.audio_loading_for_test());
    CHECK_FALSE(f.overlay.audio_play_btn_for_test()->visible());
    // Space no longer toggles a dead player.
    CHECK_FALSE(f.overlay.on_key_down(af_key(Key::Space)));

    // Stepping away and back clears the error for the next clip.
    f.overlay.open(audio_item());
    CHECK_FALSE(f.overlay.audio_error_for_test());
}

TEST_CASE("without an audio backend the page reports it and Save still works",
          "[mediaviewer][audio]")
{
    AfStage st;
    MediaViewerOverlay overlay;
    std::vector<MediaViewerItem> saved;
    overlay.on_save = [&](const MediaViewerItem& i) { saved.push_back(i); };

    overlay.open(audio_item());
    st.run(overlay);
    CHECK_FALSE(overlay.has_audio_player());
    CHECK_FALSE(overlay.audio_loading_for_test()); // straight to the message
    CHECK_FALSE(overlay.audio_play_btn_for_test()->visible());

    overlay.load_audio_bytes(overlay.load_token(), {1, 2, 3}); // must not crash
    st.run(overlay);

    click_btn(overlay, overlay.save_btn_for_test());
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].kind == MediaViewerItem::Kind::Audio);
    CHECK(saved[0].filename == "song.ogg");
}

TEST_CASE("on_playback_started fires when a video starts playing", "[mediaviewer][video]")
{
    AfStage st;
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));
    int started = 0;
    overlay.on_playback_started = [&] { ++started; };

    overlay.open(video_item("{}", "", "video/mp4", 5000, 640, 360));
    st.run(overlay);
    CHECK(started == 0);
    const std::uint8_t bytes[] = {1, 2, 3};
    overlay.load_bytes(overlay.load_token(), bytes, sizeof(bytes));
    CHECK(fake->play_count == 1);
    CHECK(started == 1);

    // A stale token never reaches the page, so never fires.
    overlay.load_bytes(overlay.load_token() - 1, bytes, sizeof(bytes));
    CHECK(started == 1);

    click_btn(overlay, overlay.play_btn_for_test()); // pause
    click_btn(overlay, overlay.play_btn_for_test()); // resume
    CHECK(started == 2);
}

// ── file page ─────────────────────────────────────────────────────────────

TEST_CASE("file page shows a labelled Save button and no copy button", "[mediaviewer][file]")
{
    AfStage st;
    MediaViewerOverlay overlay;
    std::vector<MediaViewerItem> saved;
    overlay.on_save = [&](const MediaViewerItem& i) { saved.push_back(i); };

    overlay.open(file_item());
    st.run(overlay);
    REQUIRE(overlay.file_save_btn_for_test()->visible());
    CHECK(overlay.file_save_btn_for_test()->access_name() == "Save");
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());

    click_btn(overlay, overlay.file_save_btn_for_test());
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].kind == MediaViewerItem::Kind::File);
    CHECK(saved[0].filename == "report.pdf");
}

TEST_CASE("file page: Left and Right navigate", "[mediaviewer][file][sequence]")
{
    AfStage st;
    MediaViewerOverlay overlay;
    overlay.open_sequence({file_item("mxc://x/f1", "a.pdf"), file_item("mxc://x/f2", "b.pdf")}, 0);
    st.run(overlay);
    CHECK(overlay.on_key_down(af_key(Key::Right)));
    CHECK(overlay.index() == 1);
    CHECK(overlay.on_key_down(af_key(Key::Left)));
    CHECK(overlay.index() == 0);
}

// ── mixed sequence ────────────────────────────────────────────────────────

TEST_CASE("a mixed sequence swaps the right buttons at every step and wraps",
          "[mediaviewer][sequence][file][audio]")
{
    AfStage st;
    MediaViewerOverlay overlay;
    overlay.set_video_player(std::make_unique<FakeVideoPlayer>());
    overlay.set_audio_player(std::make_unique<FakeAudioPlayer>());

    overlay.open_sequence({image_item("mxc://x/i", "", "cap", 640, 360),
                           video_item("{}", "", "video/mp4", 4000, 640, 360), audio_item(),
                           file_item()},
                          0);
    st.run(overlay);

    // image
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.file_save_btn_for_test()->visible());
    CHECK_FALSE(overlay.audio_play_btn_for_test()->visible());
    CHECK(overlay.prev_btn_for_test()->visible());
    CHECK(overlay.counter_text_for_test() == "1 / 4");

    // video
    overlay.next_btn_for_test()->click();
    st.run(overlay);
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());
    CHECK(overlay.play_btn_for_test()->visible());
    CHECK(overlay.speed_btn_for_test()->visible());
    CHECK_FALSE(overlay.file_save_btn_for_test()->visible());

    // audio: the loaded clip's transport shows, the video's is hidden again
    overlay.on_key_down(af_key(Key::PageDown));
    overlay.load_audio_bytes(overlay.load_token(), {1, 2});
    st.run(overlay);
    CHECK(overlay.current_item().kind == MediaViewerItem::Kind::Audio);
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.file_save_btn_for_test()->visible());
    CHECK(overlay.audio_play_btn_for_test()->visible());
    CHECK(overlay.audio_speed_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());

    // file
    overlay.on_key_down(af_key(Key::PageDown));
    st.run(overlay);
    CHECK(overlay.current_item().kind == MediaViewerItem::Kind::File);
    CHECK(overlay.file_save_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());
    CHECK_FALSE(overlay.audio_play_btn_for_test()->visible());
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());

    // wrap back to the image
    overlay.on_key_down(af_key(Key::PageDown));
    st.run(overlay);
    CHECK(overlay.index() == 0);
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.file_save_btn_for_test()->visible());
}

// ── save specs ────────────────────────────────────────────────────────────

TEST_CASE("media_save_spec for audio falls back filename, caption, then a mime extension",
          "[mediaviewer][items]")
{
    MediaViewerItem a;
    a.kind = MediaViewerItem::Kind::Audio;
    a.mime_type = "audio/ogg";
    auto spec = tesseract::views::media_save_spec(a);
    CHECK(spec.suggested_name == "audio.ogg");
    CHECK_FALSE(spec.title.empty());

    a.mime_type = "audio/mpeg";
    CHECK(tesseract::views::media_save_spec(a).suggested_name == "audio.mp3");
    a.mime_type = "";
    CHECK(tesseract::views::media_save_spec(a).suggested_name == "audio");

    a.caption = "my song";
    CHECK(tesseract::views::media_save_spec(a).suggested_name == "my song");
    a.filename = "song.flac";
    CHECK(tesseract::views::media_save_spec(a).suggested_name == "song.flac");
}

TEST_CASE("media_save_spec for files falls back filename, caption, then 'download'",
          "[mediaviewer][items]")
{
    MediaViewerItem f;
    f.kind = MediaViewerItem::Kind::File;
    CHECK(tesseract::views::media_save_spec(f).suggested_name == "download");
    f.caption = "the report";
    CHECK(tesseract::views::media_save_spec(f).suggested_name == "the report");
    f.filename = "r.pdf";
    auto spec = tesseract::views::media_save_spec(f);
    CHECK(spec.suggested_name == "r.pdf");
    CHECK(spec.filter_name.empty());
    CHECK(spec.patterns.empty());
}
