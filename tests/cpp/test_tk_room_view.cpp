#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/RoomView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>

using namespace tk;
using tesseract::views::RoomView;

namespace
{

struct TkRoomViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

// Tight tolerance — the light theme's near-white grays (bg/chrome_bg/
// compose_card_bg) are only ~10-15 units apart, too close for a coarse
// anti-aliasing-tolerant compare to discriminate. Fine here since every
// sample point below is deep inside a flat fill_rect, not near an edge.
bool exact_ish(Color a, Color b, int tol = 3)
{
    auto delta = [](int x, int y) { return x > y ? x - y : y - x; };
    return delta(a.r, b.r) <= tol && delta(a.g, b.g) <= tol &&
           delta(a.b, b.b) <= tol;
}

} // namespace

TEST_CASE("RoomView exposes a compose text-area rect while a room is active",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    st.run(view, {0, 0, 800, 600});
    CHECK_FALSE(view.compose_text_area_rect().empty());
}

TEST_CASE("RoomView clears the compose text-area rect after the room closes",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});
    REQUIRE_FALSE(view.compose_text_area_rect().empty());

    // Closing the room shows the brand view again; the host overlays the
    // native text area at compose_text_area_rect(), so an empty rect is what
    // tells the shell to hide it.
    view.clear_room();
    st.run(view, {0, 0, 800, 600});
    CHECK(view.compose_text_area_rect().empty());
}

TEST_CASE("RoomView claims drag-hover onto its compose bar and releases it "
          "on leave",
          "[tk][view][room][drag_hover]")
{
    TkRoomViewStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.compose_bar() != nullptr);
    const Rect cb = view.compose_bar()->bounds();
    const Point inside{cb.x + cb.w * 0.5f, cb.y + cb.h * 0.5f};

    Widget* target = view.dispatch_drag_hover(inside);
    CHECK(target == &view);

    view.on_drag_leave();
    // on_drag_leave clears the highlight flag; re-claiming should still work
    // (not left in some latched state).
    CHECK(view.dispatch_drag_hover(inside) == &view);
}

TEST_CASE("RoomView does not claim drag-hover while its compose bar is "
          "disabled (no active room)",
          "[tk][view][room][drag_hover]")
{
    TkRoomViewStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    st.run(view, {0, 0, 800, 600});

    // Mirrors on_file_drop's gate: compose_bar_->enabled() is false until
    // set_room() runs (and again after clear_room()), regardless of where in
    // RoomView's bounds the point falls — RoomView is the position-agnostic
    // catch-all, so it always claims-or-rejects as a whole, never by point.
    Widget* target = view.dispatch_drag_hover({400.0f, 300.0f});
    CHECK(target == nullptr);
}

TEST_CASE("RoomView closes the action-pill overflow menu on room switch",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;

    tesseract::RoomInfo room_a;
    room_a.id   = "!a:example.org";
    room_a.name = "Room A";
    view.set_room(room_a);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.overflow_menu() != nullptr);
    view.overflow_menu()->open({{"", {}, "Pin message", false, [] {}}},
                               {10, 10, 20, 20});
    REQUIRE(view.overflow_menu()->is_open());

    // Switching to a different room must not leave the previous room's
    // action-pill submenu open — it used to stay open until the user
    // happened to click in the timeline, which triggers the popup's own
    // backdrop-dismiss handler.
    tesseract::RoomInfo room_b;
    room_b.id   = "!b:example.org";
    room_b.name = "Room B";
    view.set_room(room_b);
    st.run(view, {0, 0, 800, 600});

    CHECK_FALSE(view.overflow_menu()->is_open());
}

TEST_CASE("RoomView scopes Tab traversal to the open Room Settings modal, "
          "excluding the header",
          "[tk][view][room][focus]")
{
    // Regression test: RoomSettingsView is an ordinary sibling child of
    // RoomHeader in RoomView's tree (not a separate surface), and Tab/
    // Shift-Tab used to be resolved via a flat, whole-window walk with no
    // concept of a modal scope — so pressing Tab while the settings dialog
    // was open still cycled through the header's buttons underneath it.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    REQUIRE(view.header() != nullptr);
    view.header()->set_show_search_btn(true);

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    pc.host = &host;

    // Arrange/paint once with the room open but Settings still closed —
    // matching real usage, where the header is shown (and its search button
    // laid out to visible()==true by its own arrange()) before the user ever
    // opens Settings. RoomView::arrange()/paint() both skip re-arranging/
    // repainting the header once Settings is open (it's fully replaced), so
    // this first pass is what the header's stale-but-still-visible() state
    // comes from in the real bug.
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    view.paint(pc);
    REQUIRE(view.header()->search_btn_rect_for_test().empty() == false);

    REQUIRE(view.room_settings_view() != nullptr);
    view.room_settings_view()->open(info);
    view.room_settings_view()->set_field_permissions(/*can_name=*/true,
                                                      /*can_topic=*/true,
                                                      /*can_avatar=*/true);

    // Second pass with Settings now open — this is what must sync the focus
    // scope (mirrors the real per-frame repaint that follows opening it).
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    view.paint(pc);

    REQUIRE(view.room_settings_view()->is_open());
    REQUIRE(view.room_settings_view()->name_field() != nullptr);
    CHECK(view.room_settings_view()->name_field()->visible());

    Widget* header_search = view.header()->search_btn_for_test();
    REQUIRE(header_search != nullptr);
    bool advanced_at_least_once = false;
    for (int i = 0; i < 30; ++i)
    {
        if (host.advance_focus(/*forward=*/true))
            advanced_at_least_once = true;
        CHECK(host.focused_widget() != header_search);
    }
    CHECK(advanced_at_least_once);
}

TEST_CASE("RoomView scopes Tab traversal to the open room media gallery, "
          "excluding the compose bar behind it",
          "[tk][view][room][focus]")
{
    // Regression test: RoomMediaView used to be a MainAppWidget-level
    // overlay (a sibling of RoomView, not a descendant), so it never
    // participated in RoomView::active_overlay_panel_()'s Tab-scoping —
    // pressing Tab while the gallery was open still cycled through the
    // compose bar / buttons in the room underneath it. Fixed by making it
    // a RoomView-owned child, like room_info_panel_/room_settings_view_.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    pc.host = &host;

    // First pass with the room open but the gallery still closed — matches
    // real usage (composer gets default focus here).
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    view.paint(pc);
    REQUIRE(view.compose_bar() != nullptr);
    REQUIRE(view.compose_bar()->text_area() != nullptr);
    CHECK(host.focused_widget() == view.compose_bar()->text_area());

    REQUIRE(view.room_media_view() != nullptr);
    view.room_media_view()->open(info.id, info.name);
    REQUIRE(view.room_media_view()->is_open());

    // Second pass with the gallery now open — syncs the focus scope,
    // mirroring the real per-frame repaint that follows opening it.
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    view.paint(pc);

    Widget* compose_text_area = view.compose_bar()->text_area();
    bool advanced_at_least_once = false;
    for (int i = 0; i < 30; ++i)
    {
        if (host.advance_focus(/*forward=*/true))
            advanced_at_least_once = true;
        CHECK(host.focused_widget() != compose_text_area);
    }
    CHECK(advanced_at_least_once);
}

TEST_CASE("RoomView actually paints the room media gallery when open, not "
          "just arranges it",
          "[tk][view][room]")
{
    // Regression test: RoomView::paint() is a fully manual list of
    // per-child paint() calls, disjoint from arrange()'s equivalent list —
    // room_media_view_ was added to arrange() (so it correctly occupied
    // space and participated in focus-scope/pointer routing) but the
    // matching paint() call was forgotten, so the gallery never actually
    // rendered anything, leaving the room content underneath fully
    // visible. Both loops now walk the same overlay_panels_() list so this
    // can't drift apart again.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    REQUIRE(view.room_media_view() != nullptr);
    view.room_media_view()->open(info.id, info.name);
    REQUIRE(view.room_media_view()->is_open());

    // Single measure/arrange/paint pass with the gallery already open —
    // deliberately not doing an earlier "before open" pass first: TestSurface
    // (Qt backend)'s read_pixel() ends its underlying QPainter the first
    // time it's called, silently no-opping any further paint() calls
    // against the same PaintCtx/canvas.
    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    pc.host = &host;
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    view.paint(pc);

    // Deep inside where the compose bar's card would otherwise be (if the
    // gallery's paint() had been skipped, as it was before this fix, this
    // point would show the card's own background instead) — the gallery's
    // full-bleed background proves RoomMediaView::paint() actually ran and
    // drew over the room content underneath, not just that arrange() gave
    // it the space.
    const int x = 400;
    const int y = 580;
    CHECK(exact_ish(st.surface->read_pixel(x, y), Theme::light().palette.bg));
    CHECK_FALSE(exact_ish(st.surface->read_pixel(x, y),
                          Theme::light().palette.compose_card_bg));
}

TEST_CASE("RoomView::is_overlay_open() is true while the room media "
          "gallery is open",
          "[tk][view][room][focus]")
{
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    REQUIRE(view.room_media_view() != nullptr);
    CHECK_FALSE(view.is_overlay_open());

    view.room_media_view()->open(info.id, info.name);
    CHECK(view.is_overlay_open());

    view.room_media_view()->close();
    CHECK_FALSE(view.is_overlay_open());
}

TEST_CASE("RoomView::set_room() closes the room media gallery on a genuine "
          "room switch",
          "[tk][view][room][focus]")
{
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo room_a;
    room_a.id   = "!a:example.org";
    room_a.name = "Room A";
    view.set_room(room_a);

    REQUIRE(view.room_media_view() != nullptr);
    view.room_media_view()->open(room_a.id, room_a.name);
    REQUIRE(view.room_media_view()->is_open());

    tesseract::RoomInfo room_b;
    room_b.id   = "!b:example.org";
    room_b.name = "Room B";
    view.set_room(room_b); // genuine switch — different id

    CHECK_FALSE(view.room_media_view()->is_open());
}

TEST_CASE("RoomView::set_room() focuses the composer on a genuine room "
          "switch, once the next paint settles visibility",
          "[tk][view][room][focus]")
{
    // Default-focus policy: composing is the primary activity in a chat
    // client, so switching to a room (which also covers app startup, since
    // every shell's tab-restore reuses the same set_room() call) should
    // land the user in the compose box ready to type.
    //
    // Deliberately deferred to the next paint() rather than fired
    // synchronously inside set_room(): at that point text_area()'s own
    // visible_ flag may still reflect an earlier "no room active" layout
    // pass (MainAppWidget::arrange() force-hides it whenever
    // compose_text_area_rect() is empty) — the *next* relayout is what
    // actually reveals it, asynchronously, after set_room() has already
    // returned. Calling focus() synchronously inside set_room() raced that
    // and silently no-op'd via Host::request_focus's visible_in_tree()
    // guard, with nothing ever retrying once the widget became visible —
    // a real bug reproduced against the live app, not a hypothetical.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    REQUIRE(view.compose_bar() != nullptr);
    REQUIRE(view.compose_bar()->text_area() != nullptr);
    CHECK(host.focused_widget() == nullptr); // not yet — deferred to paint()

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    pc.host = &host;
    view.paint(pc);

    CHECK(host.focused_widget() == view.compose_bar()->text_area());
}

TEST_CASE("RoomView::set_room() does not steal focus when re-selecting the "
          "same room",
          "[tk][view][room][focus]")
{
    // A same-room set_room() call (e.g. a metadata refresh push) is not a
    // genuine switch — it must not yank focus away from whatever the user
    // is currently doing.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    REQUIRE(view.header() != nullptr);
    view.header()->set_show_search_btn(true);
    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});

    // Move focus somewhere else on purpose, then re-select the same room.
    host.request_focus(view.header()->search_btn_for_test());
    REQUIRE(host.focused_widget() == view.header()->search_btn_for_test());

    view.set_room(info); // same id — room_changed == false
    CHECK(host.focused_widget() == view.header()->search_btn_for_test());
}

TEST_CASE("RoomView::dispatch_pointer_down redirects an unclaimed click "
          "to the composer",
          "[tk][view][room][focus]")
{
    // Regression coverage for the empty-canvas-click fallback: a click that
    // lands within the room view but that nothing (header, message list,
    // compose bar) claims — e.g. blank timeline space right after a room
    // switch, before the list has settled — should redirect to the compose
    // box rather than clearing focus to nothing, matching the "click
    // anywhere, just start typing" chat-app convention.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});

    // Deliberately move focus elsewhere first, so a pass just from
    // set_room()'s own default-focus wouldn't make this assertion vacuous.
    host.clear_focus();
    REQUIRE(host.focused_widget() == nullptr);

    // Well clear of the 60px header and the compose bar's ~56px min
    // height — squarely inside the message-list area, which declines
    // pointer-downs while its room-switch gate hasn't settled yet.
    Widget* claimed = view.dispatch_pointer_down({400.0f, 300.0f});
    REQUIRE(claimed == view.compose_bar()->text_area());
}

TEST_CASE("RoomView::dispatch_pointer_down does not redirect to the "
          "composer while no room is active",
          "[tk][view][room][focus]")
{
    // Before any room is shown (brand view state), an unclaimed click must
    // not conjure up focus on a composer that isn't actually in use.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});

    Widget* claimed = view.dispatch_pointer_down({400.0f, 300.0f});
    CHECK(claimed == nullptr);
}

TEST_CASE("RoomView::dispatch_pointer_down redirects a click on a real "
          "populated message row's plain body text to the composer",
          "[tk][view][room][focus]")
{
    // Broader coverage than the empty-list case above: with a real message
    // row present (room-switch gate cleared), an ordinary click on plain
    // body text — not a link, not the sender name/avatar, not a button —
    // still falls through MessageListView's on_pointer_down (declines via
    // ListView::on_pointer_down since the row isn't "selectable") and
    // reaches the same empty-canvas fallback.
    StubHost host;
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);

    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    REQUIRE(view.message_list() != nullptr);
    tesseract::views::MessageRowData m;
    m.kind = tesseract::views::MessageRowData::Kind::Text;
    m.event_id = "$a";
    m.sender = "@alice:example.org";
    m.sender_name = "Alice";
    m.body = "hello world, this is an ordinary plain-text message body";
    view.message_list()->set_messages({m}, false);

    TkRoomViewStage st;
    auto lc = st.layout_ctx();
    view.measure(lc, {800.0f, 600.0f});
    view.arrange(lc, {0.0f, 0.0f, 800.0f, 600.0f});
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    pc.host = &host;
    view.paint(pc); // populate hovered_row_geom_/link_cache_ etc.

    host.clear_focus();
    REQUIRE(host.focused_widget() == nullptr);

    // Body text sits below the 60px header + avatar/sender row, well clear
    // of the compose bar at the bottom.
    Widget* claimed = view.dispatch_pointer_down({150.0f, 130.0f});
    CHECK(claimed == view.compose_bar()->text_area());
}
