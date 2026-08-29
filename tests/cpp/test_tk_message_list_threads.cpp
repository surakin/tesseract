#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/MessageListView.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

MessageRowData make_text(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender = "@alice:example.org";
    r.sender_name = "Alice";
    r.body = body;
    return r;
}

MessageRowData make_thread_root(const std::string& id,
                                std::uint64_t replies = 2)
{
    auto r = make_text(id, "root body");
    r.is_thread_root = true;
    r.thread_reply_count = replies;
    r.thread_latest_sender_name = "Bob";
    r.thread_latest_body = "Latest reply text";
    r.thread_latest_ts = 1234567890ULL;
    return r;
}

MessageRowData make_in_thread_reply(const std::string& id,
                                    const std::string& root)
{
    auto r = make_text(id, "reply");
    r.thread_root_id = root;
    return r;
}

struct TkMessageListThreadsStage
{
    std::unique_ptr<TestSurface> surface =
        TestSurface::create(600, 400);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("MessageListView::set_messages drops in-thread replies",
          "[message_list][threads]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(make_text("$a"));
    msgs.push_back(make_in_thread_reply("$b", "$root"));
    msgs.push_back(make_text("$c"));
    v.set_messages(std::move(msgs), false);
    REQUIRE(v.messages().size() == 2);
    CHECK(v.messages()[0].event_id == "$a");
    CHECK(v.messages()[1].event_id == "$c");
}

TEST_CASE("MessageListView: nav-loading overlay does not double-dim on top of "
          "an open thread's scrim",
          "[message_list][threads]")
{
    // Reproduces opening a thread whose root isn't currently loaded: that
    // starts a nav-loading fetch (subscribe_room_at) on the SAME
    // MessageListView that set_dimmed(true) already darkened (see
    // ShellBase::apply_thread_transition_). Before the fix, both overlays
    // painted their own independent rgba(0,0,0,60) scrim, stacking to a
    // darker result. Sampled away from center, where draw_nav_spinner_
    // paints its dots, so this only measures the scrim fill.
    //
    // Two separate stages/surfaces: TestSurface::read_pixel() flushes
    // (EndDraw) the D2D render target with no re-BeginDraw, so a second
    // paint()+read_pixel() on the same surface would be reading back an
    // unchanged bitmap regardless of what the second paint tried to draw.
    MessageListView v;
    v.set_post_delayed([](int, std::function<void()> cb) { cb(); }); // fire immediately

    TkMessageListThreadsStage st1;
    v.set_dimmed(true);
    st1.run(v, {0, 0, 600, 400});
    const tk::Color dimmed_only = st1.surface->read_pixel(20, 20);

    v.begin_nav_loading();
    REQUIRE(v.nav_spinner_visible_for_test());

    TkMessageListThreadsStage st2;
    st2.run(v, {0, 0, 600, 400});
    const tk::Color dimmed_plus_nav = st2.surface->read_pixel(20, 20);

    CHECK(dimmed_only.r == dimmed_plus_nav.r);
    CHECK(dimmed_only.g == dimmed_plus_nav.g);
    CHECK(dimmed_only.b == dimmed_plus_nav.b);
}

TEST_CASE("MessageListView::insert_message drops in-thread replies",
          "[message_list][threads]")
{
    MessageListView v;
    v.set_messages({make_text("$a")}, false);
    v.insert_message(1, make_in_thread_reply("$b", "$root"));
    REQUIRE(v.messages().size() == 1);
    CHECK(v.messages()[0].event_id == "$a");
}

TEST_CASE("MessageListView::append_message drops in-thread replies",
          "[message_list][threads]")
{
    MessageListView v;
    v.set_messages({make_text("$a")}, false);
    v.append_message(make_in_thread_reply("$b", "$root"));
    REQUIRE(v.messages().size() == 1);
    CHECK(v.messages()[0].event_id == "$a");
}

TEST_CASE("MessageListView::update_message ignores in-thread payload",
          "[message_list][threads]")
{
    MessageListView v;
    v.set_messages({make_text("$a", "before")}, false);
    auto reply = make_in_thread_reply("$a", "$root");
    reply.body = "after";
    v.update_message(0, std::move(reply));
    // The row should NOT have been replaced by the in-thread payload.
    REQUIRE(v.messages().size() == 1);
    CHECK(v.messages()[0].event_id == "$a");
    CHECK(v.messages()[0].body == "before");
    CHECK(v.messages()[0].thread_root_id.empty());
}

TEST_CASE("MessageListView::set_dimmed flips the dim flag",
          "[message_list][threads]")
{
    MessageListView v;
    CHECK_FALSE(v.dimmed());
    v.set_dimmed(true);
    CHECK(v.dimmed());
    v.set_dimmed(false);
    CHECK_FALSE(v.dimmed());
}

TEST_CASE("MessageListView::set_highlighted_event stores the id",
          "[message_list][threads]")
{
    MessageListView v;
    CHECK(v.highlighted_event().empty());
    v.set_highlighted_event("$root");
    CHECK(v.highlighted_event() == "$root");
    v.set_highlighted_event("");
    CHECK(v.highlighted_event().empty());
}

TEST_CASE(
    "MessageListView records a thread chip hit rect for thread-root rows",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;
    v.set_messages({make_thread_root("$root", 3)}, false);
    st.run(v, {0, 0, 600, 400});

    const auto& hits = v.chip_hit_rects_for_test();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].root_event_id == "$root");
    CHECK(hits[0].rect.w > 0.0f);
    CHECK(hits[0].rect.h > 0.0f);
}

TEST_CASE(
    "MessageListView::on_thread_preview_clicked fires for a chip hit",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;
    v.set_messages({make_thread_root("$root", 2)}, false);
    st.run(v, {0, 0, 600, 400});

    REQUIRE(v.chip_hit_rects_for_test().size() == 1);
    const tk::Rect chip = v.chip_hit_rects_for_test()[0].rect;

    std::string clicked;
    v.on_thread_preview_clicked = [&](const std::string& id)
    { clicked = id; };

    // chip rect is in world coords; on_pointer_down takes widget-local
    // coords. View bounds were {0,0,600,400} so local == world here.
    tk::Point centre{chip.x + chip.w * 0.5f, chip.y + chip.h * 0.5f};
    REQUIRE(v.on_pointer_down(centre));
    CHECK(clicked == "$root");
}

TEST_CASE(
    "MessageListView clears thread chip hit rects when scrolling to an event",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;

    std::vector<MessageRowData> msgs;
    msgs.push_back(make_thread_root("$root", 2));
    for (int i = 0; i < 24; ++i)
        msgs.push_back(make_text("$filler" + std::to_string(i), "filler"));
    msgs.push_back(make_text("$target", "target"));
    v.set_messages(std::move(msgs), false);
    st.run(v, {0, 0, 600, 140});

    REQUIRE(v.chip_hit_rects_for_test().size() == 1);
    const tk::Rect chip = v.chip_hit_rects_for_test()[0].rect;
    const tk::Point centre{chip.x + chip.w * 0.5f, chip.y + chip.h * 0.5f};

    REQUIRE(v.scroll_to_event_id("$target"));
    CHECK(v.chip_hit_rects_for_test().empty());

    std::string clicked;
    v.on_thread_preview_clicked = [&](const std::string& id)
    { clicked = id; };

    v.on_pointer_down(centre);
    CHECK(clicked.empty());
}

TEST_CASE(
    "MessageListView clears thread chip hit rects when wheel scrolling",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;

    std::vector<MessageRowData> msgs;
    msgs.push_back(make_thread_root("$root", 2));
    for (int i = 0; i < 24; ++i)
        msgs.push_back(make_text("$filler" + std::to_string(i), "filler"));
    v.set_messages(std::move(msgs), false);
    st.run(v, {0, 0, 600, 140});

    REQUIRE(v.chip_hit_rects_for_test().size() == 1);
    const tk::Rect chip = v.chip_hit_rects_for_test()[0].rect;
    const tk::Point centre{chip.x + chip.w * 0.5f, chip.y + chip.h * 0.5f};

    REQUIRE(v.on_wheel(centre, 0.0f, 80.0f));
    CHECK(v.chip_hit_rects_for_test().empty());

    std::string clicked;
    v.on_thread_preview_clicked = [&](const std::string& id)
    { clicked = id; };

    v.on_pointer_down(centre);
    CHECK(clicked.empty());
}

TEST_CASE(
    "MessageListView clears thread chip hit rects when deferred event scroll resolves",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;

    v.set_messages({make_thread_root("$root", 2)}, false);
    st.run(v, {0, 0, 600, 140});

    REQUIRE(v.chip_hit_rects_for_test().size() == 1);
    const tk::Rect chip = v.chip_hit_rects_for_test()[0].rect;
    const tk::Point centre{chip.x + chip.w * 0.5f, chip.y + chip.h * 0.5f};

    // Simulate the subscribe_room_at/reset path: the target was not in the
    // first window, the reset replaces the timeline, then the shell re-arms the
    // pending scroll before layout resolves it.
    std::vector<MessageRowData> reset_rows;
    for (int i = 0; i < 24; ++i)
        reset_rows.push_back(make_text("$page" + std::to_string(i), "page"));
    reset_rows.push_back(make_text("$target", "target"));
    v.set_messages(std::move(reset_rows), false);
    v.set_pending_scroll_event_id("$target");
    auto lc = st.layout_ctx();
    v.measure(lc, {600, 140});
    v.arrange(lc, {0, 0, 600, 140});

    CHECK(v.chip_hit_rects_for_test().empty());

    std::string clicked;
    v.on_thread_preview_clicked = [&](const std::string& id)
    { clicked = id; };

    v.on_pointer_down(centre);
    CHECK(clicked.empty());
}

TEST_CASE(
    "MessageListView does NOT emit a chip for thread roots with no replies",
    "[message_list][threads]")
{
    TkMessageListThreadsStage st;
    MessageListView v;
    v.set_messages({make_thread_root("$root", 0)}, false);
    st.run(v, {0, 0, 600, 400});
    CHECK(v.chip_hit_rects_for_test().empty());
}
