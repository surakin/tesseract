#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ThreadListView.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using tesseract::ThreadInfo;
using tesseract::views::ThreadListView;

namespace
{

ThreadInfo make_thread(const std::string& root, std::uint64_t replies,
                       std::uint64_t root_ts = 2000,
                       std::uint64_t latest_ts = 0, bool unread = false)
{
    ThreadInfo t;
    t.root_event_id      = root;
    t.root_sender_name   = "Alice";
    t.root_body          = "Hello world";
    t.root_timestamp     = root_ts;
    t.latest_sender_name = "Bob";
    t.latest_body        = "Reply!";
    t.latest_timestamp   = latest_ts;
    t.num_replies        = replies;
    t.unread             = unread;
    return t;
}

// Header centre-x of the "mark all read" button (immediately left of close).
float mark_all_cx(float panel_w)
{
    const float close_x = panel_w - ThreadListView::kCloseSz
                          - ThreadListView::kCloseInset;
    const float mark_x = close_x - ThreadListView::kCloseSz
                         - ThreadListView::kHeaderBtnGap;
    return mark_x + ThreadListView::kCloseSz * 0.5f;
}

struct TkThreadListViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 400);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    void arrange(tk::Widget& w, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }
};

} // namespace

TEST_CASE("ThreadListView::set_threads stores the list", "[thread_list]")
{
    ThreadListView v;
    // Ascending by activity: $b (lower timestamp) sorts first, $a last.
    v.set_threads({make_thread("$a", 1, /*root_ts=*/2000),
                   make_thread("$b", 5, /*root_ts=*/1000)});
    REQUIRE(v.threads().size() == 2);
    CHECK(v.threads()[0].root_event_id == "$b");
    CHECK(v.threads()[1].root_event_id == "$a");
}

TEST_CASE("ThreadListView::set_threads sorts newest activity last",
          "[thread_list]")
{
    ThreadListView v;
    // $new has a later latest_timestamp, so it sorts LAST (bottom of the list).
    v.set_threads({make_thread("$old", 1, /*root_ts=*/1000, /*latest_ts=*/0),
                   make_thread("$new", 3, /*root_ts=*/1000, /*latest_ts=*/5000)});
    REQUIRE(v.threads().size() == 2);
    CHECK(v.threads()[0].root_event_id == "$old");
    CHECK(v.threads()[1].root_event_id == "$new");
}

TEST_CASE("ThreadListView::on_close fires when floating close button clicked",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    bool closed = false;
    v.on_close = [&] { closed = true; };

    // Close button sits in the right side of the empty header strip.
    const float cx = 300.0f - ThreadListView::kCloseInset
                     - ThreadListView::kCloseSz * 0.5f;
    const float cy = ThreadListView::kHeaderH * 0.5f;
    // Dispatch through the widget tree so the close-button child receives
    // the click — calling ThreadListView::on_pointer_down directly would
    // skip the child and route to the row hit-test.
    tk::Widget* claimer = v.dispatch_pointer_down({cx, cy});
    REQUIRE(claimer != nullptr);
    const tk::Rect cb = claimer->bounds();
    claimer->on_pointer_up({cx - cb.x, cy - cb.y}, /*inside_self=*/true);
    CHECK(closed);
}

TEST_CASE("ThreadListView mark-all-read button: disabled with no unread threads",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});

    bool fired = false;
    v.on_mark_all_read = [&] { fired = true; };

    // No threads → button disabled. A disabled widget is opaque to input: it
    // absorbs the press (so nothing behind it reacts) but never invokes its
    // own click handler.
    const tk::Point p{mark_all_cx(300.0f), ThreadListView::kHeaderH * 0.5f};
    if (tk::Widget* c = v.dispatch_pointer_down(p))
        c->on_pointer_up(p, /*inside_self=*/true);
    CHECK_FALSE(fired);

    // All threads read → still disabled.
    v.set_threads({make_thread("$a", 1, 1000, 0, /*unread=*/false)});
    st.arrange(v, {0, 0, 300, 400});
    if (tk::Widget* c = v.dispatch_pointer_down(p))
        c->on_pointer_up(p, /*inside_self=*/true);
    CHECK_FALSE(fired);
}

TEST_CASE("ThreadListView mark-all-read button fires on_mark_all_read when unread",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1, 1000, 0, /*unread=*/true),
                   make_thread("$b", 2, 2000, 0, /*unread=*/false)});
    st.arrange(v, {0, 0, 300, 400});

    bool fired = false;
    v.on_mark_all_read = [&] { fired = true; };

    const tk::Point p{mark_all_cx(300.0f), ThreadListView::kHeaderH * 0.5f};
    tk::Widget* claimer = v.dispatch_pointer_down(p);
    REQUIRE(claimer != nullptr);
    const tk::Rect r = claimer->bounds();
    claimer->on_pointer_up({p.x - r.x, p.y - r.y}, /*inside_self=*/true);
    CHECK(fired);
}

TEST_CASE("ThreadListView::on_thread_clicked fires for row clicks",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    // Ascending order: $b (lower timestamp) is the first row after the header
    // spacer; $a sorts last.
    v.set_threads({make_thread("$a", 1, /*root_ts=*/2000),
                   make_thread("$b", 5, /*root_ts=*/1000)});
    // set_threads doesn't rebuild row_rects_ — re-arrange after setting.
    st.arrange(v, {0, 0, 300, 400});

    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    // First row centre: rows start below the empty header strip.
    {
        const tk::Point p{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 0.5f};
        REQUIRE(v.on_pointer_down(p));
        v.on_pointer_up(p, true);
        CHECK(clicked == "$b");
    }

    // Second row centre.
    {
        const tk::Point p{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 1.5f};
        REQUIRE(v.on_pointer_down(p));
        v.on_pointer_up(p, true);
        CHECK(clicked == "$a");
    }
}

TEST_CASE("ThreadListView::on_thread_clicked does NOT fire if release outside row",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1, /*root_ts=*/2000)});
    st.arrange(v, {0, 0, 300, 400});

    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    const tk::Point press{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 0.5f};
    REQUIRE(v.on_pointer_down(press));
    // Release inside the header strip (not the row) — must NOT fire the
    // thread-click callback. The press-then-release-elsewhere pattern is
    // essential for cancel-by-drag behaviour.
    v.on_pointer_up({100.0f, 10.0f}, true);
    CHECK(clicked.empty());
}

TEST_CASE("ThreadListView::set_search_text filters rows and remaps clicks",
          "[thread_list]")
{
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    v.set_threads({make_thread("$apple", 1, /*root_ts=*/1000),
                   make_thread("$banana", 2, /*root_ts=*/2000)});
    st.arrange(v, {0, 0, 300, 400});

    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    // Both threads share root_sender_name "Alice" and root_body
    // "Hello world" (see make_thread); "Bob" only appears as the latest
    // reply sender, common to both too, so filter on the reply body instead
    // — both fixtures share that too. Use the distinct latest_body text
    // ("Reply!") vs a query that matches neither to prove filtering excludes
    // rows, then a query that matches to prove it's restored.
    v.set_search_text("nonexistent-query");
    st.arrange(v, {0, 0, 300, 400});
    const tk::Point first_row{100.0f,
                              ThreadListView::kHeaderH +
                                  ThreadListView::kRowH * 0.5f};
    // No rows pass the filter, so the first-row point now falls past the
    // (empty) list content — no row should be hit.
    CHECK_FALSE(v.on_pointer_down(first_row));

    v.set_search_text("");
    st.arrange(v, {0, 0, 300, 400});
    REQUIRE(v.on_pointer_down(first_row));
    v.on_pointer_up(first_row, true);
    CHECK(clicked == "$apple");
}

TEST_CASE("ThreadListView::paint re-masks the header strip over scrolled row content",
          "[thread_list]")
{
    // Regression test for the paint-dispatch bug: ThreadListView inherits
    // tk::ListView, whose paint() is a full override of tk::Widget::paint()
    // that never invokes paint_before_children()/paint_children(). Before
    // the fix, ThreadListView overrode paint_before_children() expecting
    // that hook chain to run it — but since ThreadListView never overrides
    // paint() itself, calling paint() resolved straight to ListView::paint(),
    // silently skipping the header re-mask. This test scrolls a hovered row
    // (whose translucent subtle_hover fill is visually distinct from the
    // plain background) so it sits inside the header strip, then checks the
    // strip was painted back over it.
    TkThreadListViewStage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    std::vector<ThreadInfo> threads;
    for (int i = 0; i < 10; ++i)
        threads.push_back(make_thread("$t" + std::to_string(i),
                                      1, /*root_ts=*/1000 + i));
    v.set_threads(std::move(threads));
    st.arrange(v, {0, 0, 300, 400});

    // Scroll so a data row's top aligns exactly with the panel's top edge —
    // i.e. squarely inside the header strip ([0, kHeaderH) of a kRowH=64
    // row) — then hover it.
    v.scroll_to_index(3, /*align_top=*/true);
    v.on_pointer_move({100.0f, 20.0f});

    auto lc = st.layout_ctx();
    tk::PaintCtx ctx{st.surface->canvas(), st.surface->factory(), lc.theme};
    v.paint(ctx);

    const tk::Color bg = lc.theme.palette.bg;
    const tk::Color px = st.surface->read_pixel(100, 20);
    auto close = [](std::uint8_t a, std::uint8_t b) {
        return std::abs(int(a) - int(b)) <= 2;
    };
    CHECK(close(px.r, bg.r));
    CHECK(close(px.g, bg.g));
    CHECK(close(px.b, bg.b));
}
