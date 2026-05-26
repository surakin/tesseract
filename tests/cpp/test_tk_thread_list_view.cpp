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

ThreadInfo make_thread(const std::string& root, std::uint64_t replies)
{
    ThreadInfo t;
    t.root_event_id     = root;
    t.root_sender_name  = "Alice";
    t.root_body         = "Hello world";
    t.root_timestamp    = 1000;
    t.latest_sender_name = "Bob";
    t.latest_body        = "Reply!";
    t.num_replies        = replies;
    return t;
}

struct Stage
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
    v.set_threads({make_thread("$a", 1), make_thread("$b", 5)});
    REQUIRE(v.threads().size() == 2);
    CHECK(v.threads()[0].root_event_id == "$a");
    CHECK(v.threads()[1].root_event_id == "$b");
}

TEST_CASE("ThreadListView::on_close fires when close button clicked",
          "[thread_list]")
{
    Stage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    bool closed = false;
    v.on_close = [&] { closed = true; };

    // Close button anchored to the right edge of the header. With
    // bounds {0,0,300,400} the button sits at x ≈ 300 - kPadX - kCloseSz/2,
    // y ≈ kHeaderH/2. View bounds origin is (0,0) so widget-local ==
    // world coords.
    const float cx = 300.0f - ThreadListView::kPadX
                     - ThreadListView::kCloseSz * 0.5f;
    const float cy = ThreadListView::kHeaderH * 0.5f;
    REQUIRE(v.on_pointer_down({cx, cy}));
    v.on_pointer_up({cx, cy}, true);
    CHECK(closed);
}

TEST_CASE("ThreadListView::on_thread_clicked fires for row clicks",
          "[thread_list]")
{
    Stage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1), make_thread("$b", 5)});
    // set_threads doesn't rebuild row_rects_ — re-arrange after setting.
    st.arrange(v, {0, 0, 300, 400});

    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    // First row centre: x ≈ 100, y ≈ kHeaderH + kRowH/2.
    {
        const tk::Point p{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 0.5f};
        REQUIRE(v.on_pointer_down(p));
        v.on_pointer_up(p, true);
        CHECK(clicked == "$a");
    }

    // Second row centre.
    {
        const tk::Point p{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 1.5f};
        REQUIRE(v.on_pointer_down(p));
        v.on_pointer_up(p, true);
        CHECK(clicked == "$b");
    }
}

TEST_CASE("ThreadListView::on_thread_clicked does NOT fire if release outside row",
          "[thread_list]")
{
    Stage st;
    ThreadListView v;
    st.arrange(v, {0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1)});
    st.arrange(v, {0, 0, 300, 400});

    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    const tk::Point press{100.0f,
                          ThreadListView::kHeaderH +
                              ThreadListView::kRowH * 0.5f};
    REQUIRE(v.on_pointer_down(press));
    // Release inside the header strip (not the row) — must NOT fire the
    // thread-click callback. The press-then-release-elsewhere pattern
    // is essential for cancel-by-drag behaviour.
    v.on_pointer_up({100.0f, 10.0f}, true);
    CHECK(clicked.empty());
}
