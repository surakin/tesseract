#include <catch2/catch_test_macros.hpp>

#include "views/ComposeBar.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using tesseract::views::ComposeBar;
using tesseract::views::MediaInfo;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 200);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
    }
};

} // namespace

TEST_CASE("ComposeBar set_pending_video sets Video kind and loading flag",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;
    bar.set_pending_video({0x00, 0x01}, "video/mp4", "clip.mp4");

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->kind == ComposeBar::PendingAttachment::Kind::Video);
    CHECK(p->loading == true);
    CHECK(p->mime == "video/mp4");
    CHECK(p->filename == "clip.mp4");
    CHECK(p->duration_ms == 0);
    CHECK(bar.has_pending());
}

TEST_CASE("ComposeBar update_pending_attachment fills video metadata",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;
    bar.set_pending_video({0x00}, "video/mp4", "clip.mp4");

    MediaInfo info;
    info.pending_gen = bar.pending_gen(); // capture gen immediately after set
    info.video_w = 1280;
    info.video_h = 720;
    info.duration_ms = 5000;
    info.thumb_w = 320;
    info.thumb_h = 180;
    info.thumb_bytes = {0xFF, 0xD8, 0xFF}; // minimal JPEG header
    bar.update_pending_attachment(info);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->loading == false);
    CHECK(p->width == 1280);
    CHECK(p->height == 720);
    CHECK(p->duration_ms == 5000);
    CHECK(p->thumb_width == 320);
    CHECK(p->thumb_height == 180);
    CHECK(!p->thumb_bytes_raw.empty());
}

TEST_CASE("ComposeBar set_pending_audio sets Audio kind and loading flag",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;
    bar.set_pending_audio({0x49, 0x44, 0x33}, "audio/mpeg", "track.mp3");

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->kind == ComposeBar::PendingAttachment::Kind::Audio);
    CHECK(p->loading == true);
    CHECK(p->duration_ms == 0);
    CHECK(bar.has_pending());
}

TEST_CASE("ComposeBar update_pending_attachment fills audio duration",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;
    bar.set_pending_audio({0x49}, "audio/mpeg", "track.mp3");

    MediaInfo info;
    info.pending_gen = bar.pending_gen();
    info.duration_ms = 42000;
    bar.update_pending_attachment(info);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->loading == false);
    CHECK(p->duration_ms == 42000);
}

TEST_CASE("ComposeBar set_pending_image with is_animated=true stores flag",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;
    bar.set_pending_image({0x47, 0x49, 0x46}, "image/gif", "anim.gif", true);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->kind == ComposeBar::PendingAttachment::Kind::Image);
    CHECK(p->is_animated == true);
    CHECK(p->loading == false);
}

TEST_CASE("ComposeBar on_send_video fires with correct metadata",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;

    std::string sent_mime;
    std::uint32_t sent_w = 0, sent_th = 0;
    std::uint64_t sent_dur = 0;
    bar.on_send_video = [&](std::vector<std::uint8_t>, std::string mime,
                            std::string, std::string,
                            std::uint32_t w, std::uint32_t /*h*/,
                            std::vector<std::uint8_t>, std::uint32_t /*tw*/,
                            std::uint32_t th, std::uint64_t dur, std::string)
    {
        sent_mime = std::move(mime);
        sent_w = w;
        sent_th = th;
        sent_dur = dur;
    };

    bar.set_pending_video({0x00}, "video/mp4", "clip.mp4");

    MediaInfo info;
    info.pending_gen = bar.pending_gen();
    info.video_w = 1920;
    info.video_h = 1080;
    info.thumb_w = 480;
    info.thumb_h = 270;
    info.thumb_bytes = {0xFF, 0xD8};
    info.duration_ms = 8000;
    bar.update_pending_attachment(info);

    bar.set_current_text("");
    bar.trigger_send();

    CHECK(sent_mime == "video/mp4");
    CHECK(sent_w == 1920);
    CHECK(sent_th == 270);
    CHECK(sent_dur == 8000);
}

TEST_CASE("ComposeBar update_pending_attachment discards stale gen",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;

    bar.set_pending_video({0x00}, "video/mp4", "clip.mp4");
    // Simulate user replacing the attachment before extraction finishes.
    bar.set_pending_audio({0x49}, "audio/ogg", "voice.ogg");

    // Result from the old video extraction (gen is one behind).
    MediaInfo stale;
    stale.pending_gen = bar.pending_gen() - 1;
    stale.video_w = 1920;
    stale.video_h = 1080;
    stale.duration_ms = 9999;
    bar.update_pending_attachment(stale);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->kind == ComposeBar::PendingAttachment::Kind::Audio);
    CHECK(p->duration_ms == 0); // stale video result must not have been applied
}

TEST_CASE("ComposeBar on_send_audio fires with duration",
          "[tk][view][compose][media]")
{
    Stage st;
    ComposeBar bar;

    std::uint64_t sent_dur = 0;
    std::string sent_mime;
    bar.on_send_audio = [&](std::vector<std::uint8_t>, std::string mime,
                            std::string, std::string,
                            std::uint64_t dur, std::string)
    {
        sent_mime = std::move(mime);
        sent_dur = dur;
    };

    bar.set_pending_audio({0x49}, "audio/ogg", "voice.ogg");
    MediaInfo info;
    info.pending_gen = bar.pending_gen();
    info.duration_ms = 30500;
    bar.update_pending_attachment(info);

    bar.set_current_text("");
    bar.trigger_send();

    CHECK(sent_mime == "audio/ogg");
    CHECK(sent_dur == 30500);
}
