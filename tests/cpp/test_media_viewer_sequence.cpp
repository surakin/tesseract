// MediaViewerOverlay sequences (gallery prev/next), the keys that drive them,
// and the gallery -> viewer item helpers.

#include <catch2/catch_test_macros.hpp>

#include "fake_media_players.h"
#include "media_viewer_test_items.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MediaViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/media_viewer_items.h"
#include "views/shortcut_registry.h"

#include <tesseract/media_source.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace tk;
using tesseract::test::FakeVideoPlayer;
using tesseract::views::MediaViewerItem;
using tesseract::views::MediaViewerOverlay;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::test::image_item;
using tesseract::views::test::video_item;

namespace
{

struct SeqStage
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

KeyEvent seq_key(Key k)
{
    KeyEvent e{};
    e.key = k;
    return e;
}

std::vector<MediaViewerItem> three_images()
{
    return {image_item("mxc://x/a", "", "a", 3200, 1800),
            image_item("mxc://x/b", "", "b", 640, 360),
            image_item("mxc://x/c", "", "c", 640, 360)};
}

void click_at(MediaViewerOverlay& overlay, Point p)
{
    Widget* claimed = overlay.dispatch_pointer_down(p);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(p), true);
}

} // namespace

// ── nav buttons and counter ───────────────────────────────────────────────

TEST_CASE("a single item shows neither nav buttons nor a counter",
          "[mediaviewer][sequence]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open(image_item("mxc://x/a", "", "a", 640, 360));
    st.run(overlay);
    CHECK(overlay.count() == 1);
    CHECK_FALSE(overlay.prev_btn_for_test()->visible());
    CHECK_FALSE(overlay.next_btn_for_test()->visible());
    CHECK(overlay.counter_text_for_test().empty());
}

TEST_CASE("a sequence shows nav buttons, a counter and accessible names",
          "[mediaviewer][sequence]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open_sequence(three_images(), 1);
    st.run(overlay);
    CHECK(overlay.count() == 3);
    CHECK(overlay.index() == 1);
    CHECK(overlay.prev_btn_for_test()->visible());
    CHECK(overlay.next_btn_for_test()->visible());
    CHECK(overlay.counter_text_for_test() == "2 / 3");
    CHECK(overlay.prev_btn_for_test()->access_name() == "Previous");
    CHECK(overlay.next_btn_for_test()->access_name() == "Next");
    // Vertically centred, inset 16px from each edge.
    CHECK(overlay.prev_btn_for_test()->bounds().x == 16.0f);
    CHECK(overlay.next_btn_for_test()->bounds().x + 44.0f == 600.0f - 16.0f);
}

TEST_CASE("a sequence inset keeps the image clear of the nav buttons",
          "[mediaviewer][sequence]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open(image_item("mxc://x/a", "", "", 3200, 1800));
    st.run(overlay);
    const Rect single = overlay.image_rect();

    overlay.open_sequence({image_item("mxc://x/a", "", "", 3200, 1800),
                           image_item("mxc://x/b", "", "", 3200, 1800)},
                          0);
    st.run(overlay);
    const Rect seq = overlay.image_rect();
    CHECK(seq.w < single.w);
    CHECK(seq.x >= overlay.prev_btn_for_test()->bounds().x +
                       overlay.prev_btn_for_test()->bounds().w);
    CHECK(seq.x + seq.w <= overlay.next_btn_for_test()->bounds().x);
}

TEST_CASE("clicking the next and previous buttons steps, wrapping around",
          "[mediaviewer][sequence]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open_sequence(three_images(), 0);
    st.run(overlay);

    // Next button: right edge, vertically centred.
    click_at(overlay, {600.0f - 16.0f - 22.0f, 200.0f});
    CHECK(overlay.index() == 1);
    st.run(overlay);

    // Previous button from index 1, then again from 0 wraps to the last.
    click_at(overlay, {16.0f + 22.0f, 200.0f});
    CHECK(overlay.index() == 0);
    st.run(overlay);
    click_at(overlay, {16.0f + 22.0f, 200.0f});
    CHECK(overlay.index() == 2);
    st.run(overlay);
    click_at(overlay, {600.0f - 16.0f - 22.0f, 200.0f});
    CHECK(overlay.index() == 0);
}

// ── keys ──────────────────────────────────────────────────────────────────

TEST_CASE("PageDown / PageUp step through the sequence", "[mediaviewer][sequence][keys]")
{
    MediaViewerOverlay overlay;
    overlay.open_sequence(three_images(), 0);
    CHECK(overlay.on_key_down(seq_key(Key::PageDown)));
    CHECK(overlay.index() == 1);
    CHECK(overlay.on_key_down(seq_key(Key::PageUp)));
    CHECK(overlay.index() == 0);
    CHECK(overlay.on_key_down(seq_key(Key::PageUp))); // wraps
    CHECK(overlay.index() == 2);
}

TEST_CASE("a single item ignores the navigation keys", "[mediaviewer][sequence][keys]")
{
    MediaViewerOverlay overlay;
    overlay.open(image_item("mxc://x/a", "", "", 640, 360));
    CHECK_FALSE(overlay.on_key_down(seq_key(Key::PageDown)));
    CHECK_FALSE(overlay.on_key_down(seq_key(Key::Right)));
    CHECK(overlay.index() == 0);
}

TEST_CASE("arrows navigate an image at fit and pan it when zoomed",
          "[mediaviewer][sequence][keys]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open_sequence(three_images(), 1); // 640x360: whole image visible
    st.run(overlay);
    CHECK(overlay.on_key_down(seq_key(Key::Right)));
    CHECK(overlay.index() == 2);
    st.run(overlay); // fit the newly shown image before the next key
    CHECK(overlay.on_key_down(seq_key(Key::Left)));
    CHECK(overlay.index() == 1);

    // Item 0 is large: zoom in, then Left pans instead of navigating.
    overlay.open_sequence(three_images(), 0);
    st.run(overlay);
    for (int i = 0; i < 6; ++i)
    {
        REQUIRE(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -1.0f));
        st.run(overlay);
    }
    const Rect before = overlay.image_rect();
    CHECK(overlay.on_key_down(seq_key(Key::Left)));
    st.run(overlay);
    CHECK(overlay.index() == 0);
    CHECK(overlay.image_rect().x != before.x);
}

TEST_CASE("video: arrows seek (do not navigate) but PageDown navigates",
          "[mediaviewer][sequence][keys]")
{
    MediaViewerOverlay overlay;
    overlay.set_video_player(std::make_unique<FakeVideoPlayer>());
    overlay.open_sequence({video_item("{}", "", "video/mp4", 5000, 640, 360),
                           image_item("mxc://x/b", "", "", 640, 360)},
                          0);
    // Once loaded (duration known) the arrows seek instead of navigating.
    overlay.load_bytes(overlay.load_token(), reinterpret_cast<const std::uint8_t*>("x"), 1);
    CHECK(overlay.on_key_down(seq_key(Key::Right)));
    CHECK(overlay.on_key_down(seq_key(Key::Left)));
    CHECK(overlay.index() == 0);
    CHECK(overlay.on_key_down(seq_key(Key::PageDown)));
    CHECK(overlay.index() == 1);
}

TEST_CASE("video: arrows navigate while the video is still loading or failed",
          "[mediaviewer][sequence][keys]")
{
    MediaViewerOverlay overlay;
    overlay.set_video_player(std::make_unique<FakeVideoPlayer>());
    overlay.open_sequence({image_item("mxc://x/a", "", "", 640, 360),
                           video_item("{}", "", "video/mp4", 5000, 640, 360),
                           image_item("mxc://x/c", "", "", 640, 360)},
                          1);
    REQUIRE(overlay.is_loading());
    // Still loading: nothing to seek, so Right steps to the next item.
    CHECK(overlay.on_key_down(seq_key(Key::Right)));
    CHECK(overlay.index() == 2);
    CHECK(overlay.on_key_down(seq_key(Key::Left)));
    CHECK(overlay.index() == 1);
    // Failed fetch: same.
    overlay.fail_stream(overlay.load_token());
    CHECK(overlay.on_key_down(seq_key(Key::Left)));
    CHECK(overlay.index() == 0);
}

TEST_CASE("re-opening while full-screen tells the shell to leave full-screen",
          "[mediaviewer][sequence][fullscreen]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    std::vector<bool> requests;
    overlay.on_request_fullscreen = [&](bool on) { requests.push_back(on); };
    overlay.open_sequence(three_images(), 0);
    st.run(overlay);
    click_at(overlay, {454.0f, 26.0f}); // full-screen button
    REQUIRE(requests == std::vector<bool>{true});

    overlay.open(image_item("mxc://x/z", "", "", 640, 360));
    CHECK(requests == std::vector<bool>{true, false});
    CHECK(overlay.is_open());
    // A further open in windowed mode requests nothing.
    overlay.open(image_item("mxc://x/y", "", "", 640, 360));
    CHECK(requests.size() == 2);
}

TEST_CASE("shortcut registry binds the gallery navigation keys",
          "[mediaviewer][sequence][shortcuts]")
{
    using tesseract::views::ShortcutId;
    using tesseract::views::matches;
    CHECK(matches(ShortcutId::MediaNext, seq_key(Key::PageDown)));
    CHECK(matches(ShortcutId::MediaNext, seq_key(Key::Right)));
    CHECK(matches(ShortcutId::MediaPrev, seq_key(Key::PageUp)));
    CHECK(matches(ShortcutId::MediaPrev, seq_key(Key::Left)));
    CHECK_FALSE(matches(ShortcutId::MediaNext, seq_key(Key::PageUp)));
    CHECK(static_cast<std::size_t>(ShortcutId::MediaNext) + 1 ==
          tesseract::views::kShortcutIdCount);
}

// ── item switching ────────────────────────────────────────────────────────

TEST_CASE("on_item_shown fires on every step with strictly increasing tokens",
          "[mediaviewer][sequence][token]")
{
    MediaViewerOverlay overlay;
    std::vector<std::uint64_t> tokens;
    std::vector<std::string> sources;
    overlay.on_item_shown = [&](const MediaViewerItem& item, std::uint64_t token)
    {
        tokens.push_back(token);
        sources.push_back(item.source);
    };
    overlay.open_sequence(three_images(), 0);
    overlay.on_key_down(seq_key(Key::PageDown));
    overlay.on_key_down(seq_key(Key::PageDown));
    overlay.on_key_down(seq_key(Key::PageUp));
    REQUIRE(tokens.size() == 4);
    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        CHECK(tokens[i] > tokens[i - 1]);
    }
    CHECK(sources[1] == "mxc://x/b");
    CHECK(sources[2] == "mxc://x/c");
    CHECK(sources[3] == "mxc://x/b");
}

TEST_CASE("a video feed carrying the pre-navigation token is ignored",
          "[mediaviewer][sequence][token]")
{
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));
    overlay.open_sequence({video_item("{}", "", "video/mp4", 0, 640, 360),
                           video_item("{}", "", "video/mp4", 0, 640, 360)},
                          0);
    const std::uint64_t stale = overlay.load_token();
    overlay.on_key_down(seq_key(Key::PageDown));

    const std::uint8_t byte = 1;
    overlay.load_bytes(stale, &byte, 1);
    CHECK(fake->play_count == 0);
    overlay.load_bytes(overlay.load_token(), &byte, 1);
    CHECK(fake->play_count == 1);
}

TEST_CASE("a mixed sequence swaps the page controls at every step",
          "[mediaviewer][sequence][switch]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    auto player = std::make_unique<FakeVideoPlayer>();
    auto* fake = player.get();
    overlay.set_video_player(std::move(player));
    overlay.open_sequence({image_item("mxc://x/a", "", "", 640, 360),
                           video_item("{}", "", "video/mp4", 5000, 640, 360),
                           image_item("mxc://x/c", "", "", 640, 360)},
                          0);
    st.run(overlay);
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());

    overlay.on_key_down(seq_key(Key::PageDown));
    st.run(overlay);
    CHECK_FALSE(overlay.copy_btn_for_test()->visible());
    CHECK(overlay.play_btn_for_test()->visible());
    CHECK(overlay.speed_btn_for_test()->visible());

    const int stops = fake->stop_count;
    overlay.on_key_down(seq_key(Key::PageDown));
    st.run(overlay);
    CHECK(fake->stop_count > stops);
    CHECK(overlay.copy_btn_for_test()->visible());
    CHECK_FALSE(overlay.play_btn_for_test()->visible());
    CHECK_FALSE(overlay.speed_btn_for_test()->visible());
    // Nav buttons stay up throughout.
    CHECK(overlay.prev_btn_for_test()->visible());
    CHECK(overlay.next_btn_for_test()->visible());
}

// ── full-screen ───────────────────────────────────────────────────────────

TEST_CASE("full-screen survives navigation without re-requesting it",
          "[mediaviewer][sequence][fullscreen]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    int calls = 0;
    bool last = false;
    overlay.on_request_fullscreen = [&](bool on)
    {
        ++calls;
        last = on;
    };
    overlay.open_sequence(three_images(), 0);
    st.run(overlay);

    click_at(overlay, {454.0f, 26.0f}); // full-screen button
    REQUIRE(calls == 1);
    REQUIRE(last);

    overlay.on_key_down(seq_key(Key::PageDown));
    overlay.on_key_down(seq_key(Key::PageDown));
    CHECK(calls == 1);
    CHECK(last);
    st.run(overlay);
    // Full-screen: no nav gutter, the whole window is the content area.
    CHECK(overlay.is_open());
}

TEST_CASE("hovering a nav button keeps the full-screen chrome up",
          "[mediaviewer][sequence][fullscreen]")
{
    SeqStage st;
    MediaViewerOverlay overlay;
    overlay.open_sequence(three_images(), 0);
    st.run(overlay);
    click_at(overlay, {454.0f, 26.0f}); // enter full-screen
    st.run(overlay);
    REQUIRE(overlay.next_btn_for_test()->visible());

    overlay.next_btn_for_test()->set_hovered(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2700));
    st.run(overlay);
    CHECK(overlay.next_btn_for_test()->visible());

    overlay.next_btn_for_test()->set_hovered(false);
    st.run(overlay);
    CHECK_FALSE(overlay.next_btn_for_test()->visible());
    CHECK_FALSE(overlay.prev_btn_for_test()->visible());
}

// ── gallery -> viewer items ───────────────────────────────────────────────

namespace
{
MessageRowData::GalleryItemRow gal_row(MessageRowData::GalleryItemRow::Kind kind,
                                       std::string body, std::string filename,
                                       const char* mxc)
{
    MessageRowData::GalleryItemRow r;
    r.kind = kind;
    r.body = std::move(body);
    r.filename = std::move(filename);
    r.source = tesseract::MediaSource::plain(mxc);
    return r;
}
} // namespace

TEST_CASE("items_from_gallery maps kinds and fields", "[mediaviewer][items][gallery]")
{
    using Row = MessageRowData::GalleryItemRow;
    MessageListView::GalleryHit hit;
    hit.event_id = "$g";
    hit.caption = "shared caption";

    Row img = gal_row(Row::Kind::Image, "a lovely beach", "beach.jpg", "mxc://x/img");
    img.media_w = 800;
    img.media_h = 600;
    img.mime_type = "image/jpeg";
    img.file_size = 1234;
    Row vid = gal_row(Row::Kind::Video, "", "clip.mp4", "mxc://x/vid");
    vid.thumbnail = tesseract::MediaSource::plain("mxc://x/vthumb");
    vid.duration_ms = 4200;
    vid.mime_type = "video/mp4";
    Row aud = gal_row(Row::Kind::Audio, "clip.ogg", "clip.ogg", "mxc://x/aud");
    Row file = gal_row(Row::Kind::File, "report.pdf", "report.pdf", "mxc://x/file");
    hit.items = {img, vid, aud, file};

    const auto items = tesseract::views::items_from_gallery(hit);
    REQUIRE(items.size() == 4);

    CHECK(items[0].kind == MediaViewerItem::Kind::Image);
    CHECK(items[0].event_id == "$g");
    CHECK(items[0].source == img.source->fetch_token());
    CHECK(items[0].thumbnail.empty());
    CHECK(items[0].caption == "a lovely beach"); // own body wins
    CHECK(items[0].filename == "beach.jpg");
    CHECK(items[0].mime_type == "image/jpeg");
    CHECK(items[0].width == 800);
    CHECK(items[0].height == 600);
    CHECK(items[0].file_size == 1234);
    CHECK_FALSE(items[0].loop);
    CHECK_FALSE(items[0].no_audio);
    CHECK_FALSE(items[0].hide_controls);

    CHECK(items[1].kind == MediaViewerItem::Kind::Video);
    CHECK(items[1].thumbnail == vid.thumbnail->fetch_token());
    CHECK(items[1].caption == "shared caption"); // empty body -> shared caption
    CHECK(items[1].duration_ms == 4200);

    CHECK(items[2].kind == MediaViewerItem::Kind::Audio);
    CHECK(items[2].caption == "shared caption"); // body == filename -> shared caption
    CHECK(items[3].kind == MediaViewerItem::Kind::File);
    CHECK(items[3].caption == "shared caption");
}

TEST_CASE("items_from_gallery keeps every kind in cell order",
          "[mediaviewer][items][gallery]")
{
    using Row = MessageRowData::GalleryItemRow;
    MessageListView::GalleryHit hit;
    hit.event_id = "$g";
    hit.items = {gal_row(Row::Kind::File, "", "a.pdf", "mxc://x/f"),
                 gal_row(Row::Kind::Image, "", "", "mxc://x/i"),
                 gal_row(Row::Kind::Audio, "", "a.ogg", "mxc://x/a"),
                 gal_row(Row::Kind::Video, "", "v.mp4", "mxc://x/v")};

    const auto items = tesseract::views::items_from_gallery(hit);
    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == MediaViewerItem::Kind::File);
    CHECK(items[1].kind == MediaViewerItem::Kind::Image);
    CHECK(items[2].kind == MediaViewerItem::Kind::Audio);
    CHECK(items[3].kind == MediaViewerItem::Kind::Video);
}
