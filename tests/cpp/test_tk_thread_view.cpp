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

MessageRowData make_preview()
{
    MessageRowData r;
    r.kind               = MessageRowData::Kind::Text;
    r.event_id           = "$root";
    r.sender_name        = "Alice";
    r.body               = "Hello";
    r.is_thread_root     = true;
    r.thread_reply_count = 3;
    return r;
}

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

TEST_CASE("ThreadView::set_thread stores the root id and preview",
          "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
    CHECK(v.thread_root() == "$root");
    CHECK(v.root_preview().sender_name == "Alice");
    CHECK(v.root_preview().body == "Hello");
}

TEST_CASE("ThreadView::set_messages forwards an empty list", "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
    v.set_messages({}, true);
    REQUIRE(v.message_list() != nullptr);
    CHECK(v.message_list()->messages().empty());
}

TEST_CASE("ThreadView keeps reply rows even though they have thread_root_id",
          "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
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

TEST_CASE("ThreadView::on_close fires when header close button clicked",
          "[thread_view]")
{
    Stage st;
    ThreadView v;
    st.arrange(v, {0, 0, 400, 600});

    bool closed = false;
    v.on_close = [&] { closed = true; };

    // Close button anchored to the right edge of the header. With
    // bounds {0,0,400,600} the button sits at:
    //   x = 400 - kPadX - kCloseSz, w = kCloseSz, h = kCloseSz
    //   y centered in kHeaderH.
    // Widget bounds origin is (0,0) so widget-local == world coords.
    const float cx = 400.0f - ThreadView::kPadX
                     - ThreadView::kCloseSz * 0.5f;
    const float cy = ThreadView::kHeaderH * 0.5f;
    REQUIRE(v.on_pointer_down({cx, cy}));
    v.on_pointer_up({cx, cy}, true);
    CHECK(closed);
}

TEST_CASE("ThreadView::on_send fires when ComposeBar reports a send",
          "[thread_view]")
{
    ThreadView v;
    std::string got_body;
    std::string got_formatted;
    v.on_send = [&](const std::string& b, const std::string& f)
    {
        got_body      = b;
        got_formatted = f;
    };

    // The ThreadView constructor installs a forwarding lambda onto
    // compose_bar_->on_send. Triggering trigger_send() on the bare
    // ComposeBar (no pending attachment, no reply, no edit) routes
    // straight through to that lambda with the current_text_.
    REQUIRE(v.compose_bar() != nullptr);
    v.compose_bar()->set_current_text("hello");
    v.compose_bar()->trigger_send();
    CHECK(got_body == "hello");
    CHECK(got_formatted.empty());
}
