#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::ComposeBar;

namespace
{

struct TkComposeBarStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 200);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

Button* find_button_compose_bar(Widget& w, const std::string& label)
{
    for (auto& ch : w.children())
    {
        if (auto* b = dynamic_cast<Button*>(ch.get()))
        {
            if (b->label() == label)
            {
                return b;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE(
    "ComposeBar reserves a text-area rect between the emoji and send buttons",
    "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});
    Rect r = bar.text_area_rect();
    CHECK(r.w > 100.0f);
    CHECK(r.h > 0.0f);
    // Sits inside the bar (not flush with the left/right edges).
    // Emoji/sticker/send buttons are on the right; text area starts at
    // card_left + kPadX = bounds.x + 2*kPadX = 16.
    CHECK(r.x > 0.0f);
    CHECK(r.x + r.w < 640.0f - 30.0f);
}

TEST_CASE("ComposeBar without a Host has no text_area", "[tk][view][compose]")
{
    ComposeBar bar;
    CHECK(bar.text_area() == nullptr);
}

TEST_CASE("ComposeBar with a Host self-owns a text_area positioned at "
          "text_area_rect",
          "[tk][view][compose]")
{
    StubHost host;
    TkComposeBarStage st;
    ComposeBar bar(&host);
    REQUIRE(bar.text_area() != nullptr);
    REQUIRE(host.areas_created.size() == 1);

    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    CHECK(bar.text_area()->visible());
    Rect field_bounds = bar.text_area()->bounds();
    Rect rect = bar.text_area_rect();
    CHECK(field_bounds.x == rect.x);
    CHECK(field_bounds.w == rect.w);
}

TEST_CASE("ComposeBar's text_area routes image paste into set_pending_image",
          "[tk][view][compose]")
{
    StubHost host;
    TkComposeBarStage st;
    ComposeBar bar(&host);
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    REQUIRE(host.areas_created.size() == 1);
    REQUIRE(host.areas_created[0]->on_image_paste);
    host.areas_created[0]->on_image_paste({1, 2, 3}, "image/png");

    CHECK(bar.has_pending());
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind ==
          ComposeBar::PendingAttachment::Kind::Image);
}

TEST_CASE("ComposeBar with recording=true hides its self-owned text_area",
          "[tk][view][compose]")
{
    StubHost host;
    TkComposeBarStage st;
    ComposeBar bar(&host);
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});
    REQUIRE(bar.text_area()->visible());

    bar.set_recording(true);
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});
    CHECK_FALSE(bar.text_area()->visible());
    CHECK(bar.text_area_rect().empty());
}

TEST_CASE("ComposeBar starts with the send button disabled until text appears",
          "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    CHECK_FALSE(send->enabled());

    bar.set_current_text("hello");
    CHECK(send->enabled());

    // Whitespace-only counts as empty.
    bar.set_current_text("   \n\t");
    CHECK_FALSE(send->enabled());
}

TEST_CASE("ComposeBar send-button click fires on_send with the current text",
          "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    std::string got;
    bar.on_send = [&](const std::string& t)
    {
        got = t;
    };
    bar.set_current_text("hi there");
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();
    CHECK(got == "hi there");
}

TEST_CASE("ComposeBar emoji-button click fires on_emoji", "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    int hits = 0;
    bar.on_emoji = [&](tk::Rect)
    {
        ++hits;
    };
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    Button* emoji = nullptr;
    for (auto& ch : bar.children())
    {
        if (auto* b = dynamic_cast<Button*>(ch.get()))
        {
            if (b != find_button_compose_bar(bar, "Send"))
            {
                emoji = b;
                break;
            }
        }
    }
    REQUIRE(emoji);
    emoji->click();
    CHECK(hits == 1);
}

TEST_CASE("ComposeBar natural_height grows with the text-area content height",
          "[tk][view][compose]")
{
    ComposeBar bar;
    CHECK(bar.natural_height() == ComposeBar::kMinHeight);

    bar.set_text_area_natural_height(200.0f);
    // Clamps to kMaxHeight; padding pushes it to the ceiling.
    CHECK(bar.natural_height() == ComposeBar::kMaxHeight);

    bar.set_text_area_natural_height(0.0f);
    CHECK(bar.natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar pending image floats above bar and enables send "
          "without text",
          "[tk][view][compose]")
{
    ComposeBar bar;
    const float baseline = bar.natural_height();
    CHECK_FALSE(bar.has_pending());

    int size_changes = 0;
    bar.on_size_changed = [&]
    {
        ++size_changes;
    };

    // Tiny 1×1 PNG generated by hand (deflate level 0). Decoder-agnostic
    // enough for QImage / GdkPixbufLoader / WIC / ImageIO.
    static const unsigned char kPng1x1[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    bar.set_pending_image(
        std::vector<std::uint8_t>(std::begin(kPng1x1), std::end(kPng1x1)),
        "image/png");
    CHECK(bar.has_pending());
    // Image previews float above the bar bounds — bar height stays unchanged.
    CHECK(bar.natural_height() == baseline);
    CHECK(size_changes == 1);

    // Send button enables even when the text field is empty.
    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    CHECK(send->enabled());

    bar.clear_pending();
    CHECK_FALSE(bar.has_pending());
    CHECK(bar.natural_height() == baseline);
    CHECK(size_changes == 2);
    CHECK_FALSE(send->enabled());
}

TEST_CASE("ComposeBar send with pending image fires on_send_image and clears "
          "the attachment",
          "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;

    bool image_fired = false;
    std::string got_mime, got_caption, got_filename;
    bar.on_send_image = [&](std::vector<std::uint8_t> bytes, std::string mime,
                            std::string filename, std::string caption,
                            std::uint32_t, std::uint32_t, bool, std::string)
    {
        image_fired = true;
        got_mime = std::move(mime);
        got_caption = std::move(caption);
        got_filename = std::move(filename);
        (void)bytes;
    };
    int plain_send = 0;
    bar.on_send = [&](const std::string&)
    {
        ++plain_send;
    };

    static const unsigned char kPng1x1[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    bar.set_pending_image(
        std::vector<std::uint8_t>(std::begin(kPng1x1), std::end(kPng1x1)),
        "image/png");
    bar.set_current_text("look at this");

    st.run(bar, {0, 0, 640, bar.natural_height()});

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();

    CHECK(image_fired);
    CHECK(got_mime == "image/png");
    CHECK(got_caption == "look at this");
    CHECK(got_filename.rfind("clipboard-", 0) == 0);
    CHECK(plain_send == 0);
    CHECK_FALSE(bar.has_pending());
}

TEST_CASE("ComposeBar set_pending_image preserves an explicit filename",
          "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;

    std::string got_filename;
    bar.on_send_image = [&](std::vector<std::uint8_t> bytes,
                            std::string /*mime*/, std::string filename,
                            std::string /*caption*/, std::uint32_t,
                            std::uint32_t, bool, std::string)
    {
        got_filename = std::move(filename);
        (void)bytes;
    };

    // Drop path: caller passes the original filename. The widget must
    // forward it verbatim instead of synthesising a "clipboard-…" name.
    bar.set_pending_image(std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47},
                          "image/png", "my-cat.png");

    st.run(bar, {0, 0, 640, bar.natural_height()});
    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    send->click();

    CHECK(got_filename == "my-cat.png");
}

TEST_CASE("ComposeBar second pending image replaces the first",
          "[tk][view][compose]")
{
    ComposeBar bar;
    bar.set_pending_image(std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47},
                          "image/png");
    REQUIRE(bar.has_pending());
    int changes = 0;
    bar.on_size_changed = [&]
    {
        ++changes;
    };
    bar.set_pending_image(std::vector<std::uint8_t>{0xFF, 0xD8, 0xFF, 0xE0},
                          "image/jpeg");
    CHECK(bar.has_pending());
    // Replacement still fires on_size_changed even though height bucket
    // stays the same, so the host can refresh its envelope.
    CHECK(changes == 1);
}

TEST_CASE("ComposeBar set_pending_file shows the file chip and enables send "
          "without text",
          "[tk][view][compose][file]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    const float baseline = bar.natural_height();
    CHECK_FALSE(bar.has_pending());

    int size_changes = 0;
    bar.on_size_changed = [&]
    {
        ++size_changes;
    };

    std::vector<std::uint8_t> payload(128, 0xAB);
    bar.set_pending_file(std::move(payload), "application/pdf", "report.pdf");
    CHECK(bar.has_pending());
    // File band is shorter than the image band, but still grows past baseline.
    CHECK(bar.natural_height() > baseline);
    CHECK(bar.natural_height() < baseline + ComposeBar::kPreviewBandH);
    CHECK(size_changes == 1);

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    CHECK(send->enabled());

    // The chip layout is built lazily in arrange(); ensure that path runs
    // without crashing the test surface.
    st.run(bar, {0, 0, 640, bar.natural_height()});
}

TEST_CASE("ComposeBar send with pending file fires on_send_file with caption",
          "[tk][view][compose][file]")
{
    TkComposeBarStage st;
    ComposeBar bar;

    bool fired = false;
    std::string got_mime, got_filename, got_caption;
    std::size_t got_size = 0;
    bar.on_send_file = [&](std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename, std::string caption,
                           std::string)
    {
        fired = true;
        got_size = bytes.size();
        got_mime = std::move(mime);
        got_filename = std::move(filename);
        got_caption = std::move(caption);
    };
    int image_fires = 0;
    bar.on_send_image = [&](std::vector<std::uint8_t>, std::string, std::string,
                            std::string, std::uint32_t, std::uint32_t, bool,
                            std::string)
    {
        ++image_fires;
    };
    int plain_send = 0;
    bar.on_send = [&](const std::string&)
    {
        ++plain_send;
    };

    std::vector<std::uint8_t> payload(2048, 0x42);
    bar.set_pending_file(std::move(payload), "application/octet-stream",
                         "blob.bin");
    bar.set_current_text("here you go");
    st.run(bar, {0, 0, 640, bar.natural_height()});

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();

    CHECK(fired);
    CHECK(got_mime == "application/octet-stream");
    CHECK(got_filename == "blob.bin");
    CHECK(got_caption == "here you go");
    CHECK(got_size == 2048);
    CHECK(image_fires == 0);
    CHECK(plain_send == 0);
    CHECK_FALSE(bar.has_pending());
}

TEST_CASE(
    "ComposeBar pending image followed by pending file replaces the attachment",
    "[tk][view][compose][file]")
{
    ComposeBar bar;
    bar.set_pending_image(std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47},
                          "image/png");
    REQUIRE(bar.has_pending());
    // image band heights to compare deltas against
    const float h_with_image = bar.natural_height();

    bar.set_pending_file(std::vector<std::uint8_t>(64, 0x11), "application/zip",
                         "archive.zip");
    REQUIRE(bar.has_pending());
    // Image previews float above (no height change); file chips live inside
    // the bar, so replacing an image with a file grows natural_height.
    CHECK(bar.natural_height() > h_with_image);
}

TEST_CASE("ComposeBar clear_pending discards a pending file too",
          "[tk][view][compose][file]")
{
    ComposeBar bar;
    const float baseline = bar.natural_height();
    bar.set_pending_file(std::vector<std::uint8_t>(16, 0x00),
                         "application/json", "data.json");
    REQUIRE(bar.has_pending());
    bar.clear_pending();
    CHECK_FALSE(bar.has_pending());
    CHECK(bar.natural_height() == baseline);
}

TEST_CASE(
    "ComposeBar set_enabled(false) gates the send button regardless of text",
    "[tk][view][compose]")
{
    TkComposeBarStage st;
    ComposeBar bar;
    bar.set_current_text("ready");
    bar.set_enabled(false);
    st.run(bar, {0, 0, 640, ComposeBar::kMinHeight});

    Button* send = find_button_compose_bar(bar, "Send");
    REQUIRE(send);
    CHECK_FALSE(send->enabled());

    bar.set_enabled(true);
    CHECK(send->enabled());
}

TEST_CASE("ComposeBar mic unavailable: natural_height unchanged, mic_btn hidden",
          "[tk][view][compose][voice]")
{
    TkComposeBarStage st;
    auto cb = std::make_unique<ComposeBar>();
    float baseline = cb->natural_height();
    cb->set_mic_available(false);
    st.run(*cb, {0, 0, 640, 200});
    REQUIRE(!cb->mic_available());
    // Height must not change just because the mic button is hidden.
    REQUIRE(cb->natural_height() == baseline);
}

TEST_CASE("ComposeBar recording: natural_height unchanged on set_recording",
          "[tk][view][compose][voice]")
{
    TkComposeBarStage st;
    auto cb = std::make_unique<ComposeBar>();
    float idle_height = cb->natural_height();
    cb->set_recording(true);
    st.run(*cb, {0, 0, 640, 200});
    REQUIRE(cb->natural_height() == idle_height);
}

TEST_CASE("ComposeBar recording: push_amplitude no-op before set_recording",
          "[tk][view][compose][voice]")
{
    TkComposeBarStage st;
    auto cb = std::make_unique<ComposeBar>();
    for (int i = 0; i < 10; ++i)
        cb->push_amplitude(512);
    REQUIRE(cb->natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar recording: push_amplitude accepted during recording",
          "[tk][view][compose][voice]")
{
    TkComposeBarStage st;
    auto cb = std::make_unique<ComposeBar>();
    cb->set_recording(true);
    for (int i = 0; i < 10; ++i)
        cb->push_amplitude(static_cast<std::uint16_t>(i * 100));
    REQUIRE(cb->natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar recording: set_recording false resets state",
          "[tk][view][compose][voice]")
{
    TkComposeBarStage st;
    auto cb = std::make_unique<ComposeBar>();
    cb->set_recording(true);
    for (int i = 0; i < 5; ++i)
        cb->push_amplitude(800);
    cb->set_recording(false);
    REQUIRE(!cb->is_recording());
    cb->push_amplitude(999); // must not crash
}
