#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ThreadListView.h"
#include "tk_test_surface.h"

#include <cstdint>
#include <memory>
#include <string>

using tesseract::ThreadInfo;
using tesseract::views::ThreadListView;

namespace
{

ThreadInfo make_thread(const std::string& root, std::uint64_t replies,
                       std::uint64_t root_ts = 2000,
                       std::uint64_t latest_ts = 0)
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
    return t;
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
