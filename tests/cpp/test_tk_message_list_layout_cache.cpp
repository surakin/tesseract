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
    bool saw_image_span = false;
    std::string last_image_span_text; // the actual text fed to the backend
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
        for (const auto& span : sp)
        {
            if (span.is_image)
            {
                saw_image_span = true;
                last_image_span_text = span.text;
            }
        }
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

TEST_CASE("MessageListView paints a real Element-sent MSC2545 emoticon "
          "message as an image span, not literal shortcode text",
          "[message_list][layout_cache][img]")
{
    // Exact event content reported not to render: an <img data-mx-emoticon>
    // as the very first thing in formatted_body, no <p> wrapper, no leading
    // text — the case commit_block()'s leading-whitespace trim used to drop.
    Stage st;
    MessageListView v;
    MessageRowData m;
    m.kind = MessageRowData::Kind::Text;
    m.event_id = "$a";
    m.sender = "@surak:gnomos.org";
    m.sender_name = "surak";
    m.body = ":cacodemon: oh";
    m.formatted_body =
        "<img data-mx-emoticon "
        "src=\"mxc://gnomos.org/7237e619d21c4054078c8bf4c915574705d69081\" "
        "alt=\":cacodemon:\" title=\":cacodemon:\" height=\"32\"/> oh";
    v.set_messages({m}, false);

    st.run(v, {0, 0, 600, 400});

    REQUIRE(st.cf.rich >= 1);
    CHECK(st.cf.saw_image_span);
}

TEST_CASE("inserting a message collapses an existing read marker",
          "[message_list][layout_cache]")
{
    // Appending a content message flips the global suppress_read_marker_ flag,
    // which collapses any visible read marker to zero height. A targeted insert
    // alone would leave the marker (elsewhere in the list) at its stale height,
    // so this guards that the flag flip forces a full re-measure.
    Stage st;
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(make_rich("$a", "hello"));
    MessageRowData rm;
    rm.kind = MessageRowData::Kind::ReadMarker;
    rm.event_id = "$rm";
    msgs.push_back(rm);
    msgs.push_back(make_rich("$b", "world"));
    v.set_messages(std::move(msgs), false);
    st.run(v, {0, 0, 600, 400});

    REQUIRE(v.messages().size() == 3);
    REQUIRE(v.row_world_rect(1).h > 0.0f); // marker visible (content after it)

    v.insert_message(3, make_rich("$c", "again")); // append content row
    st.run(v, {0, 0, 600, 400});

    CHECK(v.row_world_rect(1).h == 0.0f); // marker collapsed by suppress flip
}

TEST_CASE("MessageListView retains the body layout across a room switch and back",
          "[message_list][layout_cache]")
{
    // Switching rooms (room_switch=true) must not discard the content-addressed
    // body layout cache: returning to a previously-viewed room should reuse the
    // already-shaped bodies instead of re-shaping every visible line.
    Stage st;
    MessageListView v;

    // Switch INTO room A and render — shapes "hello world" at least once.
    v.set_messages({make_rich("$a", "hello world")}, true);
    st.run(v, {0, 0, 600, 400});
    REQUIRE(st.cf.rich >= 1);

    // Switch to room B and render.
    v.set_messages({make_rich("$b", "a different room body")}, true);
    st.run(v, {0, 0, 600, 400});
    const int before_return = st.cf.rich;

    // Switch BACK to room A with identical content and render. With the cache
    // retained across switches the body is reused — no additional rich build.
    v.set_messages({make_rich("$a", "hello world")}, true);
    st.run(v, {0, 0, 600, 400});

    CHECK(st.cf.rich == before_return);
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
