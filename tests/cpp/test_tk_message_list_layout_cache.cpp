#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/MessageListView.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

// Wraps a real CanvasFactory, forwarding every call but counting how many
// text layouts get built. Lets a test observe whether the message list
// re-shapes a body on each paint (no cache) or reuses a cached layout.
struct CountingFactory : tk::CanvasFactory
{
    tk::CanvasFactory& inner;
    int rich = 0;
    int plain = 0;
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
        return inner.build_rich_text(sp, s);
    }
};

MessageRowData make_rich(const std::string& id, const std::string& body)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender = "@alice:example.org";
    r.sender_name = "Alice";
    r.body = body;
    // A formatted_body forces the rich-text path, so build_rich_text counts
    // are attributable solely to the message body (no reactions/quotes here).
    r.formatted_body = body;
    return r;
}

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(600, 400);
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

} // namespace

TEST_CASE("MessageListView reuses the body layout across repeated renders",
          "[message_list][layout_cache]")
{
    Stage st;
    MessageListView v;
    v.set_messages({make_rich("$a", "hello world")}, false);

    st.run(v, {0, 0, 600, 400});
    const int after_first = st.cf.rich;
    REQUIRE(after_first >= 1); // the body was shaped at least once

    st.run(v, {0, 0, 600, 400});
    const int after_second = st.cf.rich;

    // A second render of unchanged content must not re-shape the body.
    CHECK(after_second == after_first);
}

TEST_CASE("MessageListView re-shapes the body when its content changes",
          "[message_list][layout_cache]")
{
    Stage st;
    MessageListView v;
    v.set_messages({make_rich("$a", "hello world")}, false);

    st.run(v, {0, 0, 600, 400});
    st.run(v, {0, 0, 600, 400}); // settle: now cached
    const int base = st.cf.rich;

    v.update_message(0, make_rich("$a", "different body text"));
    st.run(v, {0, 0, 600, 400});

    // Exactly one rebuild for the new content, then cached again.
    CHECK(st.cf.rich == base + 1);
}

TEST_CASE("MessageListView body layout cache is memory-bounded",
          "[message_list][layout_cache]")
{
    Stage st;
    MessageListView v;
    std::vector<MessageRowData> many;
    for (int i = 0; i < 400; ++i)
    {
        many.push_back(
            make_rich("$m" + std::to_string(i), "body number " + std::to_string(i)));
    }
    v.set_messages(std::move(many), false);
    st.run(v, {0, 0, 600, 400});

    // Measuring 400 rows must not retain 400 shaped layouts.
    CHECK(v.body_layout_cache_size_for_test() < 400u);
}
