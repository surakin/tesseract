#include <catch2/catch_test_macros.hpp>

#include "tesseract/types.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/RoomListView.h"
#include "tk_test_surface.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using tesseract::RoomInfo;
using tesseract::views::RoomListView;

namespace
{

// Wraps a real CanvasFactory, counting how many rich vs. plain text layouts
// get built — lets a test observe which factory method the last-message
// preview routes through.
struct CountingFactory : tk::CanvasFactory
{
    tk::CanvasFactory& inner;
    int rich = 0;
    int plain = 0;
    // Height of the most recently built rich-text layout, and the tallest of
    // any built during a run() — used to confirm a single-line-ellipsis
    // name/preview stays one line tall instead of wrapping across several
    // (a wrapped QTextDocument / Pango layout measures ~2x a single line).
    float last_rich_height = 0.f;
    float tallest_rich_height = 0.f;
    explicit CountingFactory(tk::CanvasFactory& f) : inner(f) {}

    std::unique_ptr<tk::Image>
    decode_image(std::span<const std::uint8_t> b) override
    {
        return inner.decode_image(b);
    }
    std::unique_ptr<tk::Image>
    create_image_rgba(const std::uint8_t* p, int w, int h) override
    {
        return inner.create_image_rgba(p, w, h);
    }
    std::unique_ptr<tk::Image>
    scale_image(const tk::Image& s, int mw, int mh) override
    {
        return inner.scale_image(s, mw, mh);
    }
    std::unique_ptr<tk::AnimatedImage>
    decode_animated_image(std::span<const std::uint8_t> b, int mp) override
    {
        return inner.decode_animated_image(b, mp);
    }
    std::unique_ptr<tk::TextLayout>
    build_text(std::string_view u, const tk::TextStyle& s) override
    {
        ++plain;
        return inner.build_text(u, s);
    }
    std::unique_ptr<tk::TextLayout>
    build_rich_text(std::span<const tk::TextSpan> sp,
                    const tk::TextStyle& s) override
    {
        ++rich;
        auto layout = inner.build_rich_text(sp, s);
        if (layout)
        {
            last_rich_height = layout->measure().h;
            tallest_rich_height =
                std::max(tallest_rich_height, last_rich_height);
        }
        return layout;
    }
};

struct RoomListPreviewEmojiStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 240);
    CountingFactory cf{surface->factory()};

    void run(tk::Widget& root, tk::Rect bounds)
    {
        tk::LayoutCtx lc{cf, tk::Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        tk::PaintCtx pc{surface->canvas(), cf, tk::Theme::light()};
        root.paint(pc);
    }
};

RoomInfo room_with_preview(const std::string& id, const std::string& body)
{
    RoomInfo r;
    r.id = id;
    r.name = "Room " + id;
    r.last_activity_ts = 1'000;
    r.last_message_kind = "text";
    r.last_message_body = body;
    r.last_message_sender_name = "Alice";
    r.is_direct = true; // suppress the "sender: " prefix, isolate `body`
    return r;
}

constexpr tk::Rect kPreviewTestBounds{0, 0, 300, 240};

// Height of one line of `role` text through build_rich_text (a single space,
// same single-line-ellipsis style the row uses) — the yardstick a wrapped
// layout (~2x) fails against.
float single_line_ref_h(tk::CanvasFactory& f, tk::FontRole role)
{
    tk::TextStyle st{};
    st.role = role;
    st.trim = tk::TextTrim::Ellipsis;
    st.max_width = 200.0f;
    std::vector<tk::TextSpan> one(1);
    one[0].text = " ";
    auto lo = f.build_rich_text(one, st);
    return lo ? lo->measure().h : 0.0f;
}

} // namespace

TEST_CASE("RoomListView renders an emoji-only preview through build_rich_text",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    view.set_rooms({room_with_preview("$a", "\xF0\x9F\x98\x80")}); // 😀

    st.run(view, kPreviewTestBounds);
    CHECK(st.cf.rich >= 1);
}

TEST_CASE("RoomListView renders a mixed text+emoji preview through "
          "build_rich_text",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    view.set_rooms(
        {room_with_preview("$a", "hi \xF0\x9F\x98\x80 there")}); // "hi 😀 there"

    st.run(view, kPreviewTestBounds);
    CHECK(st.cf.rich >= 1);
}

TEST_CASE("RoomListView renders a plain-text preview without crashing",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    view.set_rooms({room_with_preview("$a", "hello world")});

    st.run(view, kPreviewTestBounds);
    CHECK(st.cf.rich >= 1); // now always routed through build_rich_text
}

TEST_CASE("RoomListView with no last message paints without a preview layout",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    RoomInfo r;
    r.id = "$a";
    r.name = "Empty Room";
    r.last_activity_ts = 1'000;
    view.set_rooms({r});

    st.run(view, kPreviewTestBounds); // must not crash with an empty preview
}

TEST_CASE("RoomListView truncates a long preview to a single line instead "
          "of wrapping",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    // Long enough to force wrapping across several lines if build_rich_text
    // doesn't honour single-line ellipsis truncation like build_text does.
    std::string long_body;
    for (int i = 0; i < 40; ++i)
    {
        long_body += "word ";
    }
    view.set_rooms({room_with_preview("$a", long_body)});

    const float ref_h =
        single_line_ref_h(st.surface->factory(), tk::FontRole::SidebarPreview);
    st.run(view, kPreviewTestBounds);
    REQUIRE(st.cf.rich >= 1);
    REQUIRE(st.cf.tallest_rich_height > 0);
    REQUIRE(ref_h > 0);
    // Every rich layout the row built stays one line; a wrap measures ~2x.
    CHECK(st.cf.tallest_rich_height < ref_h * 1.8f);
}

TEST_CASE("RoomListView truncates a long preview containing emoji to a "
          "single line",
          "[roomlist][emoji]")
{
    RoomListPreviewEmojiStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;
    std::string long_body;
    for (int i = 0; i < 30; ++i)
    {
        long_body += "hi \xF0\x9F\x98\x80 "; // "hi 😀 " repeated
    }
    view.set_rooms({room_with_preview("$a", long_body)});

    const float ref_h =
        single_line_ref_h(st.surface->factory(), tk::FontRole::SidebarPreview);
    st.run(view, kPreviewTestBounds);
    REQUIRE(st.cf.rich >= 1);
    REQUIRE(st.cf.tallest_rich_height > 0);
    REQUIRE(ref_h > 0);
    CHECK(st.cf.tallest_rich_height < ref_h * 1.8f);
}

