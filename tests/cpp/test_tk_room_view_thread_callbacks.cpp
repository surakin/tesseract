#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/RoomView.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::RoomView;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
    }
};

void open_room_and_thread(RoomView& view)
{
    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
}

} // namespace

TEST_CASE("Opening the thread panel wires reply/edit/delete on its message list",
          "[tk][view][room][thread]")
{
    Stage st;
    RoomView view;
    open_room_and_thread(view);
    st.run(view, {0, 0, 800, 600});

    auto* tv = view.thread_view();
    REQUIRE(tv != nullptr);
    auto* ml = tv->message_list();
    REQUIRE(ml != nullptr);

    // The thread panel's MessageListView shares the same hover-action set as
    // the main timeline; without these the reply/edit/redact buttons would
    // render but silently no-op.
    CHECK(static_cast<bool>(ml->on_reply_requested));
    CHECK(static_cast<bool>(ml->on_edit_requested));
    CHECK(static_cast<bool>(ml->on_more_requested));
    CHECK(static_cast<bool>(ml->on_reaction_toggled));
    CHECK(static_cast<bool>(ml->on_add_reaction_requested));
    CHECK(static_cast<bool>(ml->on_image_clicked));
    CHECK(static_cast<bool>(ml->on_link_clicked));
    CHECK(static_cast<bool>(ml->on_sender_clicked));
}

TEST_CASE("Thread message-list more-button fires RoomView::on_delete_requested",
          "[tk][view][room][thread]")
{
    Stage st;
    RoomView view;
    std::string captured;
    view.on_delete_requested = [&](std::string id) { captured = std::move(id); };

    open_room_and_thread(view);

    auto* ml = view.thread_view()->message_list();
    REQUIRE(static_cast<bool>(ml->on_more_requested));

    // Fire the more-button callback; the anchor is in world coords.
    // can_forward=false keeps Delete as the sole item (tests delete path only).
    ml->on_more_requested("$evt:example.org", {10.f, 10.f, 24.f, 24.f},
                          /*can_delete=*/true, /*can_pin=*/false,
                          /*is_pinned=*/false, /*can_forward=*/false);

    auto* pm = view.overflow_menu();
    REQUIRE(pm != nullptr);
    REQUIRE(pm->is_open());

    // Lay out so the popup computes its menu_rect from the anchor.
    st.run(view, {0, 0, 800, 600});

    // Popup bounds equal the full RoomView area; menu card is below the
    // anchor. Click the centre of the first (and only) item row.
    // anchor_local = {10,10,24,24}; menu opens at y = 10+24+2 = 36, x = 0
    // (clamped), so row 0 centre ≈ {90, 53}.
    const tk::Point click{90.f, 53.f};
    pm->on_pointer_down(click);
    pm->on_pointer_up(click, /*inside_self=*/true);
    CHECK(captured == "$evt:example.org");
}

TEST_CASE("Compose-bar reply send routes through on_thread_send_reply while the "
          "thread panel is open",
          "[tk][view][room][thread]")
{
    Stage st;
    RoomView view;

    std::string room_reply_id, room_reply_body;
    view.on_send_reply = [&](std::string id, std::string body) {
        room_reply_id   = std::move(id);
        room_reply_body = std::move(body);
    };

    std::string thread_reply_id, thread_reply_body, thread_reply_formatted;
    view.on_thread_send_reply = [&](const std::string& id,
                                    const std::string& body,
                                    const std::string& formatted) {
        thread_reply_id        = id;
        thread_reply_body      = body;
        thread_reply_formatted = formatted;
    };

    open_room_and_thread(view);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.compose_bar() != nullptr);
    REQUIRE(static_cast<bool>(view.compose_bar()->on_send_reply));
    view.compose_bar()->on_send_reply("$thread_msg:example.org", "in-thread reply");

    CHECK(thread_reply_id == "$thread_msg:example.org");
    CHECK(thread_reply_body == "in-thread reply");
    CHECK(thread_reply_formatted.empty());
    // Must NOT escape the thread to the top-level room reply path.
    CHECK(room_reply_id.empty());
    CHECK(room_reply_body.empty());
}

TEST_CASE("Compose-bar reply send still routes to on_send_reply with the "
          "thread panel closed",
          "[tk][view][room][thread]")
{
    Stage st;
    RoomView view;

    std::string room_reply_id, room_reply_body;
    view.on_send_reply = [&](std::string id, std::string body) {
        room_reply_id   = std::move(id);
        room_reply_body = std::move(body);
    };
    std::string thread_reply_id;
    view.on_thread_send_reply = [&](const std::string& id,
                                    const std::string&,
                                    const std::string&) {
        thread_reply_id = id;
    };

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});

    view.compose_bar()->on_send_reply("$msg:example.org", "regular reply");

    CHECK(room_reply_id == "$msg:example.org");
    CHECK(room_reply_body == "regular reply");
    CHECK(thread_reply_id.empty());
}
