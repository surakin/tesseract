#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/RoomView.h"
#include "views/ThreadView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;

namespace
{

struct TkRoomViewCopyShortcutStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    void run(Widget& root)
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {1000, 700});
        root.arrange(lc, {0, 0, 1000, 700});
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

MessageRowData make_text(const std::string& id, const std::string& body)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender = "@alice:example.org";
    r.sender_name = "Alice";
    r.body = body;
    return r;
}

void open_room(RoomView& view)
{
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
}

// Double-click across row 0 until a word gets selected — the exact glyph
// positions depend on the backend's fonts, so probe rather than hardcode.
bool select_word_in_first_row(MessageListView& ml)
{
    const Rect b = ml.bounds();
    const Rect r0 = ml.row_world_rect(0);
    for (float dy = 6.0f; dy < r0.h; dy += 3.0f)
    {
        for (float dx = 40.0f; dx < r0.w; dx += 20.0f)
        {
            const Point local{r0.x + dx - b.x, r0.y + dy - b.y};
            ml.on_pointer_down(local);
            ml.on_pointer_down(local); // 2nd press = word select
            ml.on_pointer_up(local, true);
            if (ml.has_selection())
            {
                return true;
            }
        }
    }
    return false;
}

KeyEvent ctrl_c()
{
    KeyEvent e;
    e.key = Key::Character;
    e.text = "c";
    e.ctrl = true;
    return e;
}

} // namespace

TEST_CASE("Ctrl+C copies the main timeline selection", "[tk][view][room][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "hello world")});
    st.run(view);

    CHECK_FALSE(view.has_active_selection());
    CHECK_FALSE(view.dispatch_key_down(ctrl_c())); // nothing to copy: falls through
    CHECK(clip.empty());

    REQUIRE(select_word_in_first_row(*view.message_list()));
    CHECK(view.has_active_selection());

    SECTION("Ctrl+C")
    {
        CHECK(view.dispatch_key_down(ctrl_c()));
    }
    SECTION("Cmd+C (meta)")
    {
        KeyEvent e = ctrl_c();
        e.ctrl = false;
        e.meta = true;
        CHECK(view.dispatch_key_down(e));
    }
    SECTION("Caps Lock reports an uppercase C")
    {
        KeyEvent e = ctrl_c();
        e.text = "C";
        CHECK(view.dispatch_key_down(e));
    }
    CHECK_FALSE(clip.empty());
    CHECK(std::string("hello world").find(clip) != std::string::npos);
}

TEST_CASE("Ctrl+Shift+C and Ctrl+Alt+C are not treated as copy", "[tk][view][room][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "hello world")});
    st.run(view);
    REQUIRE(select_word_in_first_row(*view.message_list()));

    KeyEvent shifted = ctrl_c();
    shifted.shift = true;
    CHECK_FALSE(view.dispatch_key_down(shifted));
    KeyEvent alted = ctrl_c();
    alted.alt = true;
    CHECK_FALSE(view.dispatch_key_down(alted));
    CHECK(clip.empty());
}

TEST_CASE("Ctrl+C copies a selection made in the thread panel",
          "[tk][view][room][thread][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "main timeline text")});
    view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
    auto* tv = view.thread_view();
    REQUIRE(tv != nullptr);
    tv->set_messages({make_text("$t:example.org", "threaded reply")}, false);
    st.run(view);

    REQUIRE(select_word_in_first_row(*tv->message_list()));
    CHECK(view.has_active_selection());
    CHECK(view.dispatch_key_down(ctrl_c()));
    CHECK_FALSE(clip.empty());
    CHECK(std::string("threaded reply").find(clip) != std::string::npos);
}

TEST_CASE("Starting a selection in one timeline clears the other's",
          "[tk][view][room][thread][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "main timeline text")});
    view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
    auto* tv = view.thread_view();
    REQUIRE(tv != nullptr);
    tv->set_messages({make_text("$t:example.org", "threaded reply")}, false);
    st.run(view);

    REQUIRE(select_word_in_first_row(*tv->message_list()));
    REQUIRE(select_word_in_first_row(*view.message_list()));
    CHECK_FALSE(tv->message_list()->has_selection());

    REQUIRE(view.copy_active_selection());
    CHECK(std::string("main timeline text").find(clip) != std::string::npos);

    REQUIRE(select_word_in_first_row(*tv->message_list()));
    CHECK_FALSE(view.message_list()->has_selection());
    REQUIRE(view.copy_active_selection());
    CHECK(std::string("threaded reply").find(clip) != std::string::npos);
}

TEST_CASE("Right-click on a selection uses the shell's copy menu if set",
          "[tk][view][room][thread][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "hello world")});
    st.run(view);
    REQUIRE(select_word_in_first_row(*view.message_list()));

    // Right-click inside the row opens the message menu with Copy first.
    const Rect r0 = view.message_list()->row_world_rect(0);
    const Rect mb = view.message_list()->bounds();
    const Point in_row{r0.x + 10.0f - mb.x, r0.y + 10.0f - mb.y};
    REQUIRE(view.message_list()->on_right_click(in_row));
    REQUIRE(view.overflow_menu()->is_open());
    const auto& items = view.overflow_menu()->items_for_test();
    REQUIRE_FALSE(items.empty());
    CHECK(items[0].label == tk::tr("Copy"));
    CHECK(clip.empty()); // copying is the menu item's job
    items[0].on_selected();
    CHECK_FALSE(clip.empty());
}

TEST_CASE("Right-click with no selection and no link under the pointer does nothing",
          "[tk][view][room][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "hello world")});
    st.run(view);

    // Outside every row (the pointer is above the list).
    CHECK_FALSE(view.message_list()->on_right_click({-1000.0f, -1000.0f}));
    CHECK_FALSE(view.overflow_menu()->is_open());
    CHECK(clip.empty());
    CHECK_FALSE(view.has_context_link());
    CHECK(view.copy_menu_label() == tk::tr("Copy"));
    CHECK_FALSE(view.copy_context_menu_target());
}

TEST_CASE("A selection covered by a modal overlay is not copied", "[tk][view][room][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    TestHost host(nullptr);
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "hello world")});
    st.run(view);
    REQUIRE(select_word_in_first_row(*view.message_list()));
    REQUIRE(view.has_active_selection());

    // A focus scope that doesn't contain the timeline = an overlay over it
    // (RoomView's own panels and MainAppWidget's transients scope this way).
    host.set_focus_scope(view.compose_bar());
    CHECK_FALSE(view.has_active_selection());
    CHECK_FALSE(view.dispatch_key_down(ctrl_c()));
    CHECK(clip.empty());

    // A scope that contains the timeline doesn't block it.
    host.set_focus_scope(&view);
    CHECK(view.dispatch_key_down(ctrl_c()));
    CHECK_FALSE(clip.empty());
}

TEST_CASE("A selection left in a closed thread panel is not copied",
          "[tk][view][room][thread][clipboard]")
{
    TkRoomViewCopyShortcutStage st;
    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    std::string clip;
    view.on_set_clipboard = [&](std::string_view s)
    {
        clip = std::string(s);
    };
    open_room(view);
    view.set_messages({make_text("$a:example.org", "main timeline text")});
    view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
    auto* tv = view.thread_view();
    REQUIRE(tv != nullptr);
    tv->set_messages({make_text("$t:example.org", "threaded reply")}, false);
    st.run(view);
    REQUIRE(select_word_in_first_row(*tv->message_list()));

    view.set_thread_panel(RoomView::ThreadPanelState::Closed, "");
    st.run(view);

    CHECK_FALSE(view.has_active_selection());
    CHECK_FALSE(view.dispatch_key_down(ctrl_c()));
    CHECK(clip.empty());
}
