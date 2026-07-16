#include <catch2/catch_test_macros.hpp>

#include "views/ComposeBar.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using tesseract::views::ComposeBar;
using tesseract::views::MediaInfo;

namespace
{

struct ComposeBarMediaStage
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
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
};

} // namespace

// 2-frame 1×1 GIF89a — 100 ms per frame.
static constexpr std::uint8_t kMinAnimGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61,
    0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00,
    0xFF, 0x00, 0x00,  0x00, 0x00, 0xFF,
    0x21, 0xF9, 0x04, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x02, 0x02, 0x44, 0x01, 0x00,
    0x21, 0xF9, 0x04, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x02, 0x02, 0x4C, 0x01, 0x00,
    0x3B
};

TEST_CASE("ComposeBar set_pending_video sets Video kind and loading flag",
          "[tk][view][compose][media]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

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
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

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

TEST_CASE("AnimatedImage reports correct frame_count and dimensions",
          "[tk][anim]")
{
    ComposeBarMediaStage st;
    const std::uint8_t px[4] = {255, 0, 0, 255};
    auto f0 = st.surface->factory().create_image_rgba(px, 3, 5);
    auto f1 = st.surface->factory().create_image_rgba(px, 3, 5);
    if (!f0 || !f1)
        return; // skip: backend does not implement create_image_rgba

    std::vector<std::unique_ptr<tk::Image>> frames;
    frames.push_back(std::move(f0));
    frames.push_back(std::move(f1));

    tk::AnimatedImage anim(std::move(frames), {100, 200});
    CHECK(anim.frame_count() == 2);
    CHECK(anim.width() == 3);
    CHECK(anim.height() == 5);
    CHECK(anim.ms_until_next_frame() > 0);
    CHECK(anim.ms_until_next_frame() <= 100);
    const tk::Image* f = &anim.current_frame();
    CHECK(f != nullptr);
}

TEST_CASE("CanvasFactory::decode_animated_image default returns nullptr",
          "[tk][anim]")
{
    ComposeBarMediaStage st;
    auto anim = st.surface->factory().decode_animated_image(
        std::span<const std::uint8_t>{}, 384);
    CHECK(anim == nullptr);
}

TEST_CASE("decode_animated_image returns 2-frame AnimatedImage for minimal GIF",
          "[tk][anim][decode]")
{
    ComposeBarMediaStage st;
    const std::span<const std::uint8_t> gif_span{kMinAnimGif};
    auto anim = st.surface->factory().decode_animated_image(gif_span, 384);
    if (!anim)
        return; // skip: backend does not implement decode_animated_image
    CHECK(anim->frame_count() == 2);
    CHECK(anim->width() == 1);
    CHECK(anim->height() == 1);
    CHECK(anim->ms_until_next_frame() > 0);
    CHECK(anim->ms_until_next_frame() <= 200);
}

TEST_CASE("decode_animated_image returns nullptr for empty bytes",
          "[tk][anim][decode]")
{
    ComposeBarMediaStage st;
    auto anim = st.surface->factory().decode_animated_image({}, 384);
    CHECK(anim == nullptr);
}

TEST_CASE("ComposeBar animated pending image sets anim_preview and fires repaint callback",
          "[tk][view][compose][anim]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

    int repaint_delay = -1;
    bar.on_request_anim_repaint_ = [&](int d) { repaint_delay = d; };

    std::vector<std::uint8_t> gif_bytes(std::begin(kMinAnimGif),
                                        std::end(kMinAnimGif));
    bar.set_pending_image(gif_bytes, "image/gif", "anim.gif",
                         /*is_animated=*/true);

    auto lc = st.layout_ctx();
    bar.measure(lc, {640.0f, 200.0f});
    bar.arrange(lc, {0.0f, 0.0f, 640.0f, 200.0f});
    auto pc = st.paint_ctx();
    bar.paint(pc);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    if (!p->anim_preview)
        return; // skip: backend does not implement decode_animated_image
    CHECK(repaint_delay > 0);
    CHECK(repaint_delay <= 200);
}
