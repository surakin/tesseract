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
    bool saw_bold_span = false;
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
            if (span.bold)
                saw_bold_span = true;
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

struct TkMessageListLayoutCacheStage
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
    TkMessageListLayoutCacheStage st;
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
    TkMessageListLayoutCacheStage st;
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
    TkMessageListLayoutCacheStage st;
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
    TkMessageListLayoutCacheStage st;
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
    TkMessageListLayoutCacheStage st;
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
    TkMessageListLayoutCacheStage st;
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

namespace
{
MessageRowData make_table_row(const std::string& id,
                              const std::string& formatted)
{
    MessageRowData r;
    r.kind           = MessageRowData::Kind::Text;
    r.event_id       = id;
    r.sender         = "@alice:example.org";
    r.sender_name    = "Alice";
    r.body           = "table";
    r.formatted_body = formatted;
    return r;
}

constexpr const char* kTableHtml =
    "<table><thead><tr><th>Name</th><th>Role</th></tr></thead>"
    "<tbody><tr><td>Alice</td><td>Admin</td></tr>"
    "<tr><td>Bob</td><td>Moderator</td></tr></tbody></table>";
} // namespace

TEST_CASE("MessageListView renders a Markdown table without crashing",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    v.set_messages({make_table_row("$t", kTableHtml)}, false);
    st.run(v, {0, 0, 600, 400});
    st.run(v, {0, 0, 600, 400});

    // A 3-row table row is clearly taller than a single line of body text.
    TkMessageListLayoutCacheStage plain;
    MessageListView pv;
    pv.set_messages({make_rich("$p", "one line")}, false);
    plain.run(pv, {0, 0, 600, 400});

    CHECK(v.row_world_rect(0).h > pv.row_world_rect(0).h * 1.6f);
}

TEST_CASE("MessageListView reuses a table body layout across renders",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    v.set_messages({make_table_row("$t", kTableHtml)}, false);

    st.run(v, {0, 0, 600, 400});
    const int after_first = st.cf.rich;
    REQUIRE(after_first >= 4); // 4 cells shaped at least once

    st.run(v, {0, 0, 600, 400});
    // The block-structure cache must serve the table unchanged — no re-shape.
    CHECK(st.cf.rich == after_first);
}

TEST_CASE("MessageListView re-shapes a table when its content changes",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    v.set_messages({make_table_row("$t", kTableHtml)}, false);
    st.run(v, {0, 0, 600, 400});
    st.run(v, {0, 0, 600, 400});
    const int base = st.cf.rich;

    v.update_message(
        0, make_table_row(
               "$t",
               "<table><tr><td>only</td><td>one</td></tr></table>"));
    st.run(v, {0, 0, 600, 400});

    CHECK(st.cf.rich > base); // rebuilt
}

TEST_CASE("MessageListView copy_selection spanning a table does not crash",
          "[message_list][layout_cache][table]")
{
    // A table (and any block-structure body) leaves LinkLayout::layout null;
    // copy_selection must fall back to `plain` instead of dereferencing it.
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    std::string clip;
    v.on_set_clipboard = [&](std::string_view s) { clip = std::string(s); };
    v.set_messages({make_rich("$a", "alpha"),
                    make_table_row("$t", kTableHtml),
                    make_rich("$z", "omega")},
                   false);
    st.run(v, {0, 0, 600, 400});

    const tk::Rect r0 = v.row_world_rect(0);
    const tk::Rect r2 = v.row_world_rect(2);
    // Anchor a selection in the first message, drag the head into the third
    // so the ordered range covers the table row in the middle.
    bool anchored = false;
    for (float dy = 6.0f; dy < r0.h && !anchored; dy += 3.0f)
    {
        v.on_pointer_down({r0.x + 60.0f, r0.y + dy});
        anchored = v.on_pointer_down({r0.x + 60.0f, r0.y + dy}); // 2nd = word sel
    }
    REQUIRE(anchored);
    for (float dy = r2.h - 4.0f; dy > 0.0f; dy -= 3.0f)
    {
        v.on_pointer_drag({r2.x + 60.0f, r2.y + dy});
        if (v.has_selection())
            break;
    }
    REQUIRE(v.has_selection());

    v.copy_selection(); // must not crash on the null-layout table message
    CHECK(clip.find("alpha") != std::string::npos);
    CHECK(clip.find('|') != std::string::npos); // table GFM
    CHECK(clip.find("omega") != std::string::npos);
}

TEST_CASE("MessageListView double-click selects a word inside a table cell",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    std::string clip;
    v.on_set_clipboard = [&](std::string_view s) { clip = std::string(s); };
    v.set_messages({make_table_row("$t", kTableHtml)}, false);
    st.run(v, {0, 0, 600, 400});

    const tk::Rect rr = v.row_world_rect(0);
    static const std::string words[] = {"Name", "Role",  "Alice",
                                        "Admin", "Bob",   "Moderator"};
    bool ok = false;
    for (float dy = 4.0f; dy < rr.h && !ok; dy += 2.0f)
        for (float dx = 40.0f; dx < rr.x + rr.w * 0.5f && !ok; dx += 3.0f)
        {
            v.on_pointer_down({rr.x + dx, rr.y + dy});
            if (!v.on_pointer_down({rr.x + dx, rr.y + dy})) // 2nd click
                continue;
            if (!v.has_selection())
                continue;
            clip.clear();
            v.copy_selection();
            for (const auto& w : words)
                if (clip == w)
                    ok = true;
        }
    CHECK(ok);
}

TEST_CASE("MessageListView selects a rectangular block across table cells",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    std::string clip;
    v.on_set_clipboard = [&](std::string_view s) { clip = std::string(s); };
    v.set_messages({make_table_row("$t", kTableHtml)}, false);
    st.run(v, {0, 0, 600, 400});

    const tk::Rect rr = v.row_world_rect(0);
    // Anchor near the top-left of the grid, drag toward the bottom-right so
    // the block covers more than one cell.
    bool ok = false;
    for (float dy = 4.0f; dy < rr.h * 0.5f && !ok; dy += 3.0f)
        for (float dx = 40.0f; dx < rr.w * 0.4f && !ok; dx += 3.0f)
        {
            if (!v.on_pointer_down({rr.x + dx, rr.y + dy}))
                continue;
            v.on_pointer_drag({rr.x + rr.w - 6.0f, rr.y + rr.h - 4.0f});
            if (!v.has_selection())
            {
                v.on_pointer_up({rr.x, rr.y + rr.h + 50.0f}, false);
                continue;
            }
            clip.clear();
            v.copy_selection();
            // TSV: tab between columns, newline between rows, no GFM pipes.
            ok = clip.find('|') == std::string::npos &&
                 clip.find('\t') != std::string::npos &&
                 clip.find('\n') != std::string::npos &&
                 clip.find("Name") != std::string::npos &&
                 clip.find("Moderator") != std::string::npos;
        }
    CHECK(ok);
}

TEST_CASE("MessageListView renders the reply-quote body with inline formatting",
          "[message_list][layout_cache][reply]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    MessageRowData m;
    m.kind                       = MessageRowData::Kind::Text;
    m.event_id                   = "$r";
    m.sender                     = "@alice:example.org";
    m.sender_name                = "Alice";
    m.body                       = "sure"; // plain main body — no rich build
    m.in_reply_to_id             = "$orig";
    m.in_reply_to_sender_name    = "Bob";
    m.in_reply_to_formatted_body = "<strong>Ship it</strong> by <code>fri</code>";
    v.set_messages({m}, false);
    st.run(v, {0, 0, 600, 400});

    CHECK(st.cf.saw_bold_span); // the quote body rendered as rich text
}

TEST_CASE("MessageListView reply-quote falls back to plain body without HTML",
          "[message_list][layout_cache][reply]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    MessageRowData m;
    m.kind                    = MessageRowData::Kind::Text;
    m.event_id                = "$r";
    m.sender                  = "@alice:example.org";
    m.sender_name             = "Alice";
    m.body                    = "sure";
    m.in_reply_to_id          = "$orig";
    m.in_reply_to_sender_name = "Bob";
    m.in_reply_to_body        = "**Ship it** by `fri`";
    v.set_messages({m}, false);
    st.run(v, {0, 0, 600, 400});

    CHECK_FALSE(st.cf.saw_bold_span);
}

TEST_CASE("MessageListView hit-tests a hyperlink inside a table cell",
          "[message_list][layout_cache][table]")
{
    TkMessageListLayoutCacheStage st;
    MessageListView v;
    std::string hovered;
    v.on_link_hovered = [&](const std::string& u) { hovered = u; };
    v.set_messages(
        {make_table_row("$t",
                        "<table><tr>"
                        "<td><a href=\"https://example.com\">site</a></td>"
                        "<td>plain</td></tr></table>")},
        false);
    st.run(v, {0, 0, 600, 400});

    const tk::Rect rr = v.row_world_rect(0);
    // Establish the hovered row, then re-paint so hovered_row_geom_ is
    // captured (the inline-link hit-test keys off it).
    v.on_pointer_move({rr.x + rr.w * 0.5f, rr.y + rr.h * 0.5f});
    st.run(v, {0, 0, 600, 400});

    bool saw = false;
    for (float dy = 2.0f; dy < rr.h && !saw; dy += 2.0f)
        for (float dx = 2.0f; dx < rr.w && !saw; dx += 2.0f)
        {
            v.on_pointer_move({rr.x + dx, rr.y + dy});
            if (hovered == "https://example.com")
                saw = true;
        }
    CHECK(saw);
}
