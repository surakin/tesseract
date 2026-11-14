#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/ThreadView.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::views::MessageRowData;
using tesseract::views::ThreadView;

namespace
{

MessageRowData make_reply(const std::string& id)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.thread_root_id = "$root";
    r.body = "reply";
    return r;
}

struct TkThreadViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);
    tk::LayoutCtx layout_ctx() const
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    void arrange(tk::Widget& w, tk::Rect bounds) const
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }
};

} // namespace

TEST_CASE("ThreadView::set_messages forwards an empty list", "[thread_view]")
{
    ThreadView v;
    v.set_messages({}, true);
    REQUIRE(v.message_list() != nullptr);
    CHECK(v.message_list()->messages().empty());
}

TEST_CASE("ThreadView keeps reply rows even though they have thread_root_id", "[thread_view]")
{
    ThreadView v;
    std::vector<MessageRowData> rows;
    rows.push_back(make_reply("$r1"));
    rows.push_back(make_reply("$r2"));
    v.set_messages(std::move(rows), false);
    REQUIRE(v.message_list() != nullptr);
    REQUIRE(v.message_list()->messages().size() == 2);
    // The strip happened: the embedded list sees no thread_root_id.
    CHECK(v.message_list()->messages()[0].thread_root_id.empty());
    CHECK(v.message_list()->messages()[1].thread_root_id.empty());
}

TEST_CASE("ThreadView::on_close fires when floating close button clicked", "[thread_view]")
{
    TkThreadViewStage st;
    ThreadView v;
    st.arrange(v, {0, 0, 400, 600});

    bool closed = false;
    v.on_close = [&]
    {
        closed = true;
    };

    // Close button sits in the right side of the empty header strip.
    const float cx = 400.0f - ThreadView::kCloseInset - ThreadView::kCloseSz * 0.5f;
    const float cy = ThreadView::kHeaderH * 0.5f;
    tk::Widget* claimer = v.dispatch_pointer_down({cx, cy});
    REQUIRE(claimer != nullptr);
    const tk::Rect cb = claimer->bounds();
    claimer->on_pointer_up({cx - cb.x, cy - cb.y}, /*inside_self=*/true);
    CHECK(closed);
}

TEST_CASE("ThreadView follow toggle is hidden until supported and reports the wanted state",
          "[thread_view]")
{
    TkThreadViewStage st;
    ThreadView v;
    st.arrange(v, {0, 0, 400, 600});

    std::vector<bool> toggles;
    v.on_follow_toggled = [&](bool follow)
    {
        toggles.push_back(follow);
    };

    // Left of the close button, same vertical centre.
    const float fx = 400.0f - 2.0f * ThreadView::kCloseSz - ThreadView::kCloseInset -
                     ThreadView::kHeaderGap + ThreadView::kCloseSz * 0.5f;
    const tk::Point p{fx, ThreadView::kHeaderH * 0.5f};

    auto click = [&]
    {
        tk::Widget* claimer = v.dispatch_pointer_down(p);
        if (!claimer)
        {
            return;
        }
        const tk::Rect cb = claimer->bounds();
        claimer->on_pointer_up({p.x - cb.x, p.y - cb.y}, /*inside_self=*/true);
    };

    // Unsupported: the button is hidden, so a click reaches nothing that toggles.
    click();
    CHECK(toggles.empty());

    v.set_follow_state(/*supported=*/true, /*following=*/false);
    st.arrange(v, {0, 0, 400, 600});
    click();
    REQUIRE(toggles.size() == 1);
    CHECK(toggles[0] == true);

    v.set_follow_state(true, /*following=*/true);
    click();
    REQUIRE(toggles.size() == 2);
    CHECK(toggles[1] == false);
}

TEST_CASE("ThreadView's find-in-thread bar is open from construction, no toggle needed",
          "[thread_view]")
{
    ThreadView v;
    REQUIRE(v.search_bar() != nullptr);
    CHECK(v.search_bar()->is_open());
}

TEST_CASE("ThreadView's header row is the embedded search bar, exactly kHeaderH tall",
          "[thread_view]")
{
    TkThreadViewStage st;
    ThreadView v;
    st.arrange(v, {0, 0, 400, 600});
    REQUIRE(v.message_list() != nullptr);

    // The message list starts right below the single merged header row —
    // there is no separate strip for the search bar anymore.
    CHECK(v.message_list()->bounds().y == ThreadView::kHeaderH);

    // The search bar fills that same row, stopping short of the panel's own
    // close button rather than overlapping it.
    const tk::Rect bar_bounds = v.search_bar()->bounds();
    CHECK(bar_bounds.y == 0.0f);
    CHECK(bar_bounds.h == ThreadView::kHeaderH);
    CHECK(bar_bounds.w ==
          400.0f - ThreadView::kCloseSz - ThreadView::kCloseInset - ThreadView::kHeaderGap);
}

TEST_CASE("ThreadView's message list fires on_near_top on open when content "
          "doesn't fill the viewport",
          "[thread_view]")
{
    // Regression coverage: subscribe_thread's initial fetch is a fixed-size
    // batch with no viewport awareness. If it doesn't happen to fill a tall
    // panel, on_near_top must fire on its own — without any real user
    // scroll — so RoomPane::paginate_thread_back_ can pull the next batch.
    TkThreadViewStage st;
    ThreadView v;

    std::vector<MessageRowData> rows;
    rows.push_back(make_reply("$r1"));
    rows.push_back(make_reply("$r2"));
    v.set_messages(std::move(rows), /*room_switch=*/true);

    int fires = 0;
    REQUIRE(v.message_list() != nullptr);
    v.message_list()->on_near_top = [&]
    {
        ++fires;
    };

    // A 600px-tall panel with only 2 short rows is far from full — the very
    // first arrange() pass (no scroll gesture at all) must ask for more.
    st.arrange(v, {0, 0, 400, 600});
    CHECK(fires >= 1);
}

TEST_CASE("ThreadView::reset_search clears the query without closing the bar", "[thread_view]")
{
    ThreadView v;
    REQUIRE(v.search_bar() != nullptr);
    v.search_bar()->set_query("hello");
    REQUIRE(v.search_bar()->query() == "hello");

    // Firing on_query_changed("") is what lets the shell clear its own
    // matches/highlights (see ShellBase::handle_thread_search_query_) —
    // reset_search() itself only owns the bar's own query/text state.
    std::string last_query = "unset";
    v.search_bar()->on_query_changed = [&](const std::string& q)
    {
        last_query = q;
    };

    v.reset_search();

    CHECK(v.search_bar()->query().empty());
    CHECK(last_query.empty());
    CHECK(v.search_bar()->is_open());
}
