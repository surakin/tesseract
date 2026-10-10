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

TEST_CASE("ComposeBar add_pending_video sets Video kind and loading flag",
          "[tk][view][compose][media]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    bar.add_pending_video({0x00, 0x01}, "video/mp4", "clip.mp4");

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
    bar.add_pending_video({0x00}, "video/mp4", "clip.mp4");

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

TEST_CASE("ComposeBar add_pending_audio sets Audio kind and loading flag",
          "[tk][view][compose][media]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    bar.add_pending_audio({0x49, 0x44, 0x33}, "audio/mpeg", "track.mp3");

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
    bar.add_pending_audio({0x49}, "audio/mpeg", "track.mp3");

    MediaInfo info;
    info.pending_gen = bar.pending_gen();
    info.duration_ms = 42000;
    bar.update_pending_attachment(info);

    const auto* p = bar.pending_for_test();
    REQUIRE(p != nullptr);
    CHECK(p->loading == false);
    CHECK(p->duration_ms == 42000);
}

TEST_CASE("ComposeBar add_pending_image with is_animated=true stores flag",
          "[tk][view][compose][media]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    bar.add_pending_image({0x47, 0x49, 0x46}, "image/gif", "anim.gif", true);

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

    bar.add_pending_video({0x00}, "video/mp4", "clip.mp4");

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

TEST_CASE("ComposeBar update_pending_attachment discards result for a removed item",
          "[tk][view][compose][media]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

    bar.add_pending_video({0x00}, "video/mp4", "clip.mp4");
    std::uint32_t video_gen = bar.pending_gen(); // this item's stable identity
    // Simulate the user removing the attachment before extraction finishes
    // (the append+remove analog of the old single-slot "replace" scenario).
    bar.remove_pending(0);
    bar.add_pending_audio({0x49}, "audio/ogg", "voice.ogg");

    // Late result from the removed video's extraction.
    MediaInfo stale;
    stale.pending_gen = video_gen;
    stale.video_w = 1920;
    stale.video_h = 1080;
    stale.duration_ms = 9999;
    bar.update_pending_attachment(stale); // no-op: no item has gen == video_gen

    const auto* p = bar.pending_for_test(0);
    REQUIRE(p != nullptr);
    CHECK(p->kind == ComposeBar::PendingAttachment::Kind::Audio);
    CHECK(p->duration_ms == 0); // stale video result must not have been applied
}

TEST_CASE("ComposeBar multi-attachment: add/remove/cap/gallery dispatch",
          "[tk][view][compose][media][gallery]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

    bar.add_pending_image({0x89, 0x50, 0x4E, 0x47}, "image/png");
    bar.add_pending_file({0x01, 0x02}, "application/zip", "a.zip");
    REQUIRE(bar.pending_count() == 2);

    // remove_pending shifts later items down.
    bar.add_pending_file({0x03}, "application/zip", "b.zip");
    REQUIRE(bar.pending_count() == 3);
    bar.remove_pending(0); // drop the image
    REQUIRE(bar.pending_count() == 2);
    CHECK(bar.pending_for_test(0)->filename == "a.zip");
    CHECK(bar.pending_for_test(1)->filename == "b.zip");

    // Out-of-range remove is a no-op.
    bar.remove_pending(99);
    CHECK(bar.pending_count() == 2);

    // trigger_send() with 2+ items fires on_send_gallery, not the scalar callbacks.
    std::vector<ComposeBar::PendingAttachment> gallery_items;
    std::string gallery_caption;
    int gallery_fires = 0, file_fires = 0;
    bar.on_send_gallery = [&](std::vector<ComposeBar::PendingAttachment> items,
                              std::string caption, std::string)
    {
        gallery_items = std::move(items);
        gallery_caption = std::move(caption);
        ++gallery_fires;
    };
    bar.on_send_file = [&](std::vector<std::uint8_t>, std::string, std::string,
                           std::string, std::string) { ++file_fires; };
    bar.set_current_text("vacation");
    bar.trigger_send();

    CHECK(gallery_fires == 1);
    CHECK(file_fires == 0);
    CHECK(gallery_caption == "vacation");
    REQUIRE(gallery_items.size() == 2);
    CHECK(gallery_items[0].filename == "a.zip");
    CHECK(gallery_items[1].filename == "b.zip");
    CHECK_FALSE(bar.has_pending());
}

TEST_CASE("ComposeBar caps attachments at kMaxAttachments",
          "[tk][view][compose][media][gallery]")
{
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

    for (std::size_t i = 0; i < ComposeBar::kMaxAttachments + 5; ++i)
    {
        bar.add_pending_file({0x00}, "application/octet-stream", "f.bin");
    }
    CHECK(bar.pending_count() == ComposeBar::kMaxAttachments);
}

TEST_CASE("ComposeBar multi-attachment chip remove works even though the "
          "floating chip grid sits outside the widget's own bounds "
          "(Host::dispatch_pointer_up's inside_self is unreliable there)",
          "[tk][view][compose][media][gallery]")
{
    // Regression test: the multi-chip preview grid floats ABOVE
    // ComposeBar's normal bounds_ (like the single-image preview always
    // has), so a click there produces a *negative* local y. Host::
    // dispatch_pointer_up (host.cpp) computes inside_self from a raw
    // `ws.y >= 0 && ws.y < bounds().h` check against bounds() — which is
    // false for any point in the floating region, regardless of
    // ComposeBar's own contains_world() override. Chip removal must not
    // depend on that framework-supplied inside_self flag; it has its own
    // exact geometric check against chip_remove_rects_ and must rely on
    // that alone.
    ComposeBarMediaStage st;
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;

    bar.add_pending_image({0x89, 0x50, 0x4E, 0x47}, "image/png", "a.png");
    bar.add_pending_file({0x01}, "application/zip", "b.zip");
    REQUIRE(bar.pending_count() == 2);

    const tk::Rect bounds{0.0f, 400.0f, 640.0f, 56.0f};
    st.run(bar, bounds);

    const tk::Rect xr = bar.chip_remove_rect_for_test(0);
    REQUIRE(xr.w > 0.0f); // arrange() populated it
    REQUIRE(xr.y < bounds.y); // confirms the chip floats above bounds_

    const tk::Point world_click{xr.x + xr.w * 0.5f, xr.y + xr.h * 0.5f};
    const tk::Point local{world_click.x - bounds.x, world_click.y - bounds.y};
    REQUIRE(local.y < 0.0f); // negative local y — the crux of the bug

    REQUIRE(bar.on_pointer_down(local)); // claims the press

    // Host::dispatch_pointer_up would compute inside_self=false here (see
    // the comment above) — pass it explicitly to prove removal doesn't
    // depend on it.
    bar.on_pointer_up(local, /*inside_self=*/false);

    REQUIRE(bar.pending_count() == 1);
    CHECK(bar.pending_for_test(0)->filename == "b.zip"); // the image was removed
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

    bar.add_pending_audio({0x49}, "audio/ogg", "voice.ogg");
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
    bar.add_pending_image(gif_bytes, "image/gif", "anim.gif",
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
