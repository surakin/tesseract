#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/key_chord.h"
#include "tk/scroll_view.h"
#include "views/ConfirmDialog.h"
#include "views/RoomView.h"
#include "tk/theme.h"
#include "views/KeyboardShortcutsOverlay.h"
#include "views/MainAppWidget.h"
#include "views/shortcut_registry.h"
#include "access_test_util.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <algorithm>
#include <memory>

// The read-only Keyboard Shortcuts overlay: opened by Ctrl+/ / F1 through
// MainAppWidget, listing every registry entry.

using namespace tk;
using tesseract::views::KeyboardShortcutsOverlay;
using tesseract::views::MainAppWidget;

namespace
{

// `surface` must be destroyed after the widget tree (see the note in
// test_tk_widgets.cpp's MainAppWidget focus tests), so it's declared first.
struct ShortcutsStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1100, 700);
    StubHost host;
    std::unique_ptr<MainAppWidget> app = tk::create_root_widget<MainAppWidget>(&host);

    ShortcutsStage()
    {
        host.set_root(app.get());
    }

    void run()
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        app->measure(lc, {1100.0f, 700.0f});
        app->arrange(lc, {0, 0, 1100, 700});
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        pc.host = &host;
        app->paint(pc);
    }

    KeyboardShortcutsOverlay& overlay() const
    {
        return *app->keyboard_shortcuts_overlay();
    }
};

KeyEvent ctrl_slash()
{
    KeyEvent e{};
    e.key = Key::Character;
    e.text = "/";
    e.ctrl = true;
    return e;
}

void collect_static_text(const AccessNode& n, std::vector<std::string>& out)
{
    if (n.role == Role::StaticText)
    {
        out.push_back(n.name);
    }
    for (const auto& ch : n.children)
    {
        collect_static_text(ch, out);
    }
}

} // namespace

TEST_CASE("Ctrl+/ toggles the keyboard shortcuts overlay", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.run();
    REQUIRE_FALSE(st.overlay().is_open());

    CHECK(st.host.dispatch_key_down(ctrl_slash()));
    CHECK(st.overlay().is_open());
    CHECK(st.overlay().visible());
    st.run();

    // The same key closes it again.
    CHECK(st.host.dispatch_key_down(ctrl_slash()));
    CHECK_FALSE(st.overlay().is_open());
    CHECK_FALSE(st.overlay().visible());
}

TEST_CASE("F1 opens the shortcuts overlay and Escape closes it", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.run();
    if (tk::current_platform() == tk::Platform::MacOS)
    {
        // F1 is a media key on macOS, so the registry deliberately leaves it unbound.
        CHECK_FALSE(st.host.dispatch_key_down(KeyEvent{Key::F1}));
        CHECK_FALSE(st.overlay().is_open());
        return;
    }
    CHECK(st.host.dispatch_key_down(KeyEvent{Key::F1}));
    CHECK(st.overlay().is_open());
    st.run();
    CHECK(st.host.dispatch_key_down(KeyEvent{Key::Escape}));
    CHECK_FALSE(st.overlay().is_open());
}

TEST_CASE("shortcuts overlay keeps Tab on its Close button", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.run();
    st.app->show_keyboard_shortcuts();
    st.run(); // opening moves focus to Close and scopes Tab to the overlay
    CHECK(st.host.focused_widget() == st.overlay().close_button());

    st.host.dispatch_key_down(KeyEvent{Key::Tab});
    CHECK(st.host.focused_widget() == st.overlay().close_button());
    st.host.dispatch_key_down(KeyEvent{Key::Tab});
    CHECK(st.host.focused_widget() == st.overlay().close_button());

    // Enter on Close closes it.
    st.host.dispatch_key_down(KeyEvent{Key::Enter});
    CHECK_FALSE(st.overlay().is_open());
}

TEST_CASE("shortcuts overlay lists every registry entry", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.app->show_keyboard_shortcuts();
    st.run();

    AccessNode tree = build_access_tree(&st.overlay());
    CHECK(tree.role == Role::Dialog);
    CHECK(tree.modal);
    CHECK(tree.name == "Keyboard Shortcuts");

    std::vector<std::string> texts;
    collect_static_text(tree, texts);
    const auto& all = tesseract::views::shortcuts();
    for (const auto& def : all)
    {
        const std::string want =
            std::string(def.description) + ", " + tesseract::views::shortcut_label(def.id);
        CHECK(std::find(texts.begin(), texts.end(), want) != texts.end());
    }
    REQUIRE(access_test::find_named(tree, "Close") != nullptr);
}

TEST_CASE("Page Down scrolls the shortcuts list", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.app->show_keyboard_shortcuts();
    st.run();
    auto* scroll = st.overlay().scroll_view();
    REQUIRE(scroll->scroll_y() == 0.0f);

    CHECK(st.host.dispatch_key_down(KeyEvent{Key::PageDown}));
    CHECK(scroll->scroll_y() > 0.0f);
    CHECK(st.host.dispatch_key_down(KeyEvent{Key::Home}));
    CHECK(scroll->scroll_y() == 0.0f);
}

TEST_CASE("shortcuts overlay takes focus from the composer behind it", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.run();
    // Something behind the overlay has focus when it opens.
    st.app->room_view()->set_room({.id = "!room:example.org", .name = "Room"});
    st.app->show_room();
    st.run();
    auto* composer = st.app->room_view()->compose_bar()->text_area();
    st.host.request_focus(composer);
    REQUIRE(st.host.focused_widget() == composer);
    CHECK(st.host.dispatch_key_down(ctrl_slash()));
    st.run();
    CHECK(st.host.focused_widget() == st.overlay().close_button());
    // Escape now closes the overlay rather than reaching the composer.
    CHECK(st.host.dispatch_key_down(KeyEvent{Key::Escape}));
    CHECK_FALSE(st.overlay().is_open());
}

TEST_CASE("shortcuts overlay swallows the wheel at the list's end and over the backdrop",
          "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.app->show_keyboard_shortcuts();
    st.run();
    auto* scroll = st.overlay().scroll_view();
    st.host.dispatch_key_down(KeyEvent{Key::End});
    st.run();
    const float bottom = scroll->scroll_y();
    REQUIRE(bottom > 0.0f);

    const Rect b = scroll->bounds();
    // Past the end of the list: consumed, nothing moves.
    CHECK(st.app->dispatch_wheel({b.x + b.w * 0.5f, b.y + b.h * 0.5f}, 0.0f, 120.0f));
    CHECK(scroll->scroll_y() == bottom);
    // Over the dimmed backdrop (top-left corner, outside the card).
    CHECK(st.app->dispatch_wheel({5.0f, 5.0f}, 0.0f, 120.0f));

    // Closed again, the wheel goes back to the app.
    st.overlay().close();
    CHECK(st.app->dispatch_wheel({5.0f, 5.0f}, 0.0f, 120.0f) == false);
}

TEST_CASE("shortcut rows keep clear of the scrollbar", "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.app->show_keyboard_shortcuts();
    st.run();
    auto* scroll = st.overlay().scroll_view();
    REQUIRE(scroll->child() != nullptr);
    const Rect sb = scroll->bounds();
    const float gutter_left = sb.x + sb.w - ScrollableBase::kScrollbarGutter;
    for (const auto& row : scroll->child()->children())
    {
        CHECK(row->bounds().x + row->bounds().w <= gutter_left + 0.01f);
    }
    // ...and the rows still line up with the Close button's right edge.
    const Rect close = st.overlay().close_button()->bounds();
    CHECK(gutter_left == Catch::Approx(close.x + close.w).margin(0.5));
}

TEST_CASE("Escape closes the shortcuts overlay before a confirm dialog under it",
          "[shortcuts][overlay]")
{
    ShortcutsStage st;
    st.app->confirm_dialog()->open({.title = "Leave?"}, [] {});
    st.app->show_keyboard_shortcuts();
    st.run();
    CHECK(st.host.dispatch_key_down(KeyEvent{Key::Escape}));
    CHECK_FALSE(st.overlay().is_open());
    CHECK(st.app->confirm_dialog()->is_open());
}
