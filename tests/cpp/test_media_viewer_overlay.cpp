#include <catch2/catch_test_macros.hpp>

#include "fake_media_players.h"
#include "media_viewer_test_items.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MediaViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/media_viewer_items.h"

#include <tesseract/media_source.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::test::FakeVideoPlayer;
using tesseract::views::MediaViewerItem;
using tesseract::views::MediaViewerOverlay;
using tesseract::views::MessageListView;
using tesseract::views::test::image_item;
using tesseract::views::test::video_item;

namespace
{

struct MvOverlayStage
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

} // namespace

// ── factories ─────────────────────────────────────────────────────────────

TEST_CASE("item_from_image_hit copies tokens, caption and dimensions",
          "[mediaviewer][items]")
{
    MessageListView::ImageHit hit;
    hit.event_id = "$i";
    hit.source = tesseract::MediaSource::plain("mxc://hs/full");
    hit.thumbnail = tesseract::MediaSource::plain("mxc://hs/thumb");
    hit.body = "cat.png";
    hit.natural_w = 400;
    hit.natural_h = 300;

    const auto item = tesseract::views::item_from_image_hit(hit);
    CHECK(item.kind == MediaViewerItem::Kind::Image);
    CHECK(item.event_id == "$i");
    CHECK(item.source == hit.source->fetch_token());
    CHECK(item.thumbnail == hit.thumbnail->fetch_token());
    CHECK(item.caption == "cat.png");
    CHECK(item.filename == "cat.png");
    CHECK(item.width == 400);
    CHECK(item.height == 300);
}

TEST_CASE("item_from_image_hit tolerates a missing thumbnail",
          "[mediaviewer][items]")
{
    MessageListView::ImageHit hit;
    hit.source = tesseract::MediaSource::plain("mxc://hs/full");
    const auto item = tesseract::views::item_from_image_hit(hit);
    CHECK(item.thumbnail.empty());
    CHECK(item.width == 0);
}

TEST_CASE("item_from_video_hit carries playback hints", "[mediaviewer][items]")
{
    MessageListView::VideoHit hit;
    hit.event_id = "$v";
    hit.source = tesseract::MediaSource::plain("mxc://hs/video");
    hit.thumbnail = tesseract::MediaSource::plain("mxc://hs/vthumb");
    hit.mime_type = "video/webm";
    hit.duration_ms = 4200;
    hit.natural_w = 640;
    hit.natural_h = 360;
    hit.loop = true;
    hit.no_audio = true;
    hit.hide_controls = true;

    const auto item = tesseract::views::item_from_video_hit(hit);
    CHECK(item.kind == MediaViewerItem::Kind::Video);
    CHECK(item.source == hit.source->fetch_token());
    CHECK(item.thumbnail == hit.thumbnail->fetch_token());
    CHECK(item.mime_type == "video/webm");
    CHECK(item.duration_ms == 4200);
    CHECK(item.width == 640);
    CHECK(item.height == 360);
    CHECK(item.loop);
    CHECK(item.no_audio);
    CHECK(item.hide_controls);
}

TEST_CASE("item_from_avatar uses the URL as source and thumbnail with unknown size",
          "[mediaviewer][items]")
{
    const auto item = tesseract::views::item_from_avatar("mxc://hs/av", "Alice");
    CHECK(item.kind == MediaViewerItem::Kind::Image);
    CHECK(item.source == "mxc://hs/av");
    CHECK(item.thumbnail == "mxc://hs/av");
    CHECK(item.caption == "Alice");
    CHECK(item.filename == "Alice");
    CHECK(item.width == 0);
    CHECK(item.height == 0);
}

// ── media_save_spec ───────────────────────────────────────────────────────

TEST_CASE("media_save_spec names images, falling back to 'image'",
          "[mediaviewer][items]")
{
    auto spec = tesseract::views::media_save_spec(image_item("mxc://x", "", "cat.png"));
    CHECK(spec.suggested_name == "cat.png");
    CHECK_FALSE(spec.filter_name.empty());
    CHECK_FALSE(spec.patterns.empty());

    spec = tesseract::views::media_save_spec(image_item("mxc://x"));
    CHECK(spec.suggested_name == "image");

    // No file name: the extension comes from the mime type (jpeg -> jpg).
    auto jpeg = image_item("mxc://x");
    jpeg.mime_type = "image/jpeg";
    CHECK(tesseract::views::media_save_spec(jpeg).suggested_name == "image.jpg");
    jpeg.mime_type = "image/svg+xml";
    CHECK(tesseract::views::media_save_spec(jpeg).suggested_name == "image.svg");
    jpeg.mime_type = "image/png";
    CHECK(tesseract::views::media_save_spec(jpeg).suggested_name == "image.png");
    // A real file name is never altered.
    jpeg.filename = "cat";
    CHECK(tesseract::views::media_save_spec(jpeg).suggested_name == "cat");
}

TEST_CASE("media_save_spec derives a video name from the mime subtype",
          "[mediaviewer][items]")
{
    auto spec = tesseract::views::media_save_spec(
        video_item("{}", "", "video/webm"));
    CHECK(spec.suggested_name == "video.webm");

    spec = tesseract::views::media_save_spec(video_item("{}", "", "video/quicktime"));
    CHECK(spec.suggested_name == "video.mov");

    spec = tesseract::views::media_save_spec(
        video_item("{}", "", "video/mp4; codecs=avc1.42E01E"));
    CHECK(spec.suggested_name == "video.mp4");

    spec = tesseract::views::media_save_spec(video_item("{}", "", "video/x-matroska"));
    CHECK(spec.suggested_name == "video.mkv");

    // A hostile subtype never reaches the file name.
    spec = tesseract::views::media_save_spec(video_item("{}", "", "video/../../etc"));
    CHECK(spec.suggested_name == "video.mp4");

    spec = tesseract::views::media_save_spec(video_item("{}", "", ""));
    CHECK(spec.suggested_name == "video.mp4");
    CHECK_FALSE(spec.patterns.empty());
}

TEST_CASE("media_save_spec for audio and files uses the file name without a filter",
          "[mediaviewer][items]")
{
    MediaViewerItem file;
    file.kind = MediaViewerItem::Kind::File;
    auto spec = tesseract::views::media_save_spec(file);
    CHECK(spec.suggested_name == "download");
    CHECK(spec.filter_name.empty());
    CHECK(spec.patterns.empty());

    file.filename = "report.pdf";
    CHECK(tesseract::views::media_save_spec(file).suggested_name == "report.pdf");

    MediaViewerItem audio;
    audio.kind = MediaViewerItem::Kind::Audio;
    CHECK(tesseract::views::media_save_spec(audio).suggested_name == "audio");
}

// ── one overlay, two pages ────────────────────────────────────────────────

TEST_CASE("switching image to video on one overlay swaps the page controls",
          "[mediaviewer][switch]")
{
    MvOverlayStage st;
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));

    overlay.open(image_item("mxc://example.org/img", "", "cap", 640, 360));
    st.run(overlay);
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());

    overlay.open(video_item("mxc://example.org/v", "", "video/mp4", 5000, 640, 360));
    st.run(overlay);
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());
    CHECK(overlay.play_btn_for_test()->visible());
    CHECK(overlay.speed_btn_for_test()->visible());

    // Back to an image: the video page deactivates (player stopped, buttons hidden).
    const int stops_before = fake->stop_count;
    overlay.open(image_item("mxc://example.org/img2", "", "", 640, 360));
    st.run(overlay);
    CHECK(fake->stop_count > stops_before);
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());
}

TEST_CASE("reopening an image resets zoom and pan", "[mediaviewer][switch]")
{
    MvOverlayStage st;
    MediaViewerOverlay overlay;
    overlay.open(image_item("mxc://example.org/big", "", "", 3200, 1800));
    st.run(overlay);
    const Rect fit = overlay.image_rect();

    REQUIRE(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f));
    st.run(overlay);
    REQUIRE(overlay.image_rect().w > fit.w);

    overlay.open(video_item("mxc://example.org/v", "", "video/mp4", 0, 640, 360));
    overlay.open(image_item("mxc://example.org/big", "", "", 3200, 1800));
    st.run(overlay);
    CHECK(overlay.image_rect().w == fit.w);
    CHECK(overlay.image_rect().h == fit.h);
}

TEST_CASE("on_save and on_copy receive the current item", "[mediaviewer][callbacks]")
{
    MediaViewerOverlay overlay;
    overlay.open(image_item("mxc://example.org/img", "", "cat.png", 100, 100));
    MediaViewerItem saved;
    overlay.on_save = [&](const MediaViewerItem& item) { saved = item; };

    overlay.save_btn_for_test()->click();
    CHECK(saved.source == "mxc://example.org/img");
    CHECK(saved.filename == "cat.png");
}

TEST_CASE("on_item_shown fires on open with the current load token",
          "[mediaviewer][callbacks]")
{
    MediaViewerOverlay overlay;
    int shown = 0;
    std::uint64_t seen_token = 0;
    std::string seen_source;
    overlay.on_item_shown = [&](const MediaViewerItem& item, std::uint64_t token)
    {
        ++shown;
        seen_token = token;
        seen_source = item.source;
    };
    overlay.open(video_item("mxc://example.org/v", "", "video/mp4", 0, 640, 360));
    CHECK(shown == 1);
    CHECK(seen_token == overlay.load_token());
    CHECK(seen_source == "mxc://example.org/v");
}

// ── load token ────────────────────────────────────────────────────────────

TEST_CASE("a stale load token is ignored, the current one plays",
          "[mediaviewer][token]")
{
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));

    overlay.open(video_item("mxc://example.org/v1", "", "video/mp4", 0, 640, 360));
    const std::uint64_t old_token = overlay.load_token();
    overlay.open(video_item("mxc://example.org/v2", "", "video/mp4", 0, 640, 360));
    CHECK(overlay.load_token() > old_token);

    const std::uint8_t byte = 1;
    overlay.load_bytes(old_token, &byte, 1);
    CHECK(fake->play_count == 0);

    overlay.load_bytes(overlay.load_token(), &byte, 1);
    CHECK(fake->play_count == 1);
}

TEST_CASE("closing the viewer invalidates in-flight deliveries",
          "[mediaviewer][token]")
{
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    fake->stream_supported = true;
    overlay.set_video_player(std::move(player));

    overlay.open(video_item("mxc://example.org/v", "", "video/mp4", 0, 640, 360));
    const std::uint64_t token = overlay.load_token();
    overlay.close();
    CHECK(overlay.load_token() > token);

    const std::uint8_t byte = 1;
    overlay.load_bytes(token, &byte, 1);
    overlay.begin_stream_or_buffer(token);
    overlay.feed_stream_chunk(token, &byte, 1);
    overlay.set_stream_length(token, 10);
    overlay.end_stream(token);
    overlay.fail_stream(token);
    CHECK(fake->play_count == 0);
    CHECK(fake->begin_stream_count == 0);
    CHECK(fake->fed_bytes == 0);
    CHECK(fake->end_stream_count == 0);
}

TEST_CASE("stale stream chunks do not feed a newer item", "[mediaviewer][token]")
{
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    fake->stream_supported = true;
    overlay.set_video_player(std::move(player));

    overlay.open(video_item("mxc://example.org/v1", "", "video/mp4", 0, 640, 360));
    const std::uint64_t old_token = overlay.load_token();
    overlay.open(video_item("mxc://example.org/v2", "", "video/mp4", 0, 640, 360));
    const std::uint64_t token = overlay.load_token();

    overlay.begin_stream_or_buffer(token);
    REQUIRE(fake->begin_stream_count == 1);

    // Preroll is held back until 256 KiB, so push enough through to flush.
    const std::vector<std::uint8_t> big(300 * 1024, 7);
    overlay.feed_stream_chunk(old_token, big.data(), big.size());
    CHECK(fake->fed_bytes == 0);
    overlay.feed_stream_chunk(token, big.data(), big.size());
    CHECK(fake->fed_bytes == big.size());
}

TEST_CASE("images are unaffected by video deliveries", "[mediaviewer][token]")
{
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));
    overlay.open(image_item("mxc://example.org/img", "", "", 640, 360));

    const std::uint8_t byte = 1;
    overlay.load_bytes(overlay.load_token(), &byte, 1);
    CHECK(fake->play_count == 0);
}

// ── memory accounting ─────────────────────────────────────────────────────

TEST_CASE("memory_bytes sums the player and the stream pre-roll buffer",
          "[mediaviewer][memory]")
{
    MediaViewerOverlay overlay;
    CHECK(overlay.memory_bytes() == 0);

    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    fake->memory = 1000;
    overlay.set_video_player(std::move(player));
    CHECK(overlay.memory_bytes() == 1000);

    overlay.open(video_item("mxc://example.org/v", "", "video/mp4", 0, 640, 360));
    overlay.begin_stream_or_buffer(overlay.load_token()); // buffering fallback
    const std::vector<std::uint8_t> chunk(5000, 1);
    overlay.feed_stream_chunk(overlay.load_token(), chunk.data(), chunk.size());
    CHECK(overlay.memory_bytes() >= 1000 + 5000);
}
