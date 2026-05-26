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
    r.kind           = MessageRowData::Kind::Text;
    r.event_id       = id;
    r.thread_root_id = "$root";
    r.body           = "reply";
    return r;
}

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);
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

TEST_CASE("ThreadView::set_messages forwards an empty list", "[thread_view]")
{
    ThreadView v;
    v.set_messages({}, true);
    REQUIRE(v.message_list() != nullptr);
    CHECK(v.message_list()->messages().empty());
}

TEST_CASE("ThreadView keeps reply rows even though they have thread_root_id",
          "[thread_view]")
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

TEST_CASE("ThreadView::on_close fires when floating close button clicked",
          "[thread_view]")
{
    Stage st;
    ThreadView v;
    st.arrange(v, {0, 0, 400, 600});

    bool closed = false;
    v.on_close = [&] { closed = true; };

    // Close button sits in the right side of the empty header strip.
    const float cx = 400.0f - ThreadView::kCloseInset
                     - ThreadView::kCloseSz * 0.5f;
    const float cy = ThreadView::kHeaderH * 0.5f;
    tk::Widget* claimer = v.dispatch_pointer_down({cx, cy});
    REQUIRE(claimer != nullptr);
    const tk::Rect cb = claimer->bounds();
    claimer->on_pointer_up({cx - cb.x, cy - cb.y}, /*inside_self=*/true);
    CHECK(closed);
}
