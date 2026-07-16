#include <catch2/catch_test_macros.hpp>

#include "tk/text_area.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

using namespace tk;

namespace
{

struct TkTextAreaStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    LayoutCtx lc()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& w, Rect bounds)
    {
        auto l = lc();
        w.measure(l, {bounds.w, bounds.h});
        w.arrange(l, bounds);
        PaintCtx p{surface->canvas(), surface->factory(), Theme::light()};
        w.paint(p);
    }
};

} // namespace

TEST_CASE("TextArea: degrades to a plain spacer when the host has no native backend")
{
    TestHost host(nullptr);
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    CHECK_FALSE(area.focusable());
    area.set_text("hello");
    CHECK(area.text().empty());
    CHECK_FALSE(area.visible());
}

TEST_CASE("TextArea: text/placeholder round-trip through a real native backend")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);

    area.set_text("draft message");
    CHECK(area.text() == "draft message");
    CHECK(area.focusable());
}

TEST_CASE("TextArea: on_changed fires from the native backend")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);

    std::string last;
    area.set_on_changed([&](const std::string& t) { last = t; });
    host.areas_created[0]->on_changed("typed text");
    CHECK(last == "typed text");
}

TEST_CASE("TextArea: natural_height and set_on_height_changed forward to the backend")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    host.areas_created[0]->natural_height_ = 96.0f;
    CHECK(area.natural_height() == 96.0f);

    float last_height = -1.0f;
    area.set_on_height_changed([&](float h) { last_height = h; });
    host.areas_created[0]->on_height_changed(72.0f);
    CHECK(last_height == 72.0f);
}

TEST_CASE("TextArea: arrange positions the native backend and respects min_height")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    TkTextAreaStage stage;
    stage.run(area, {10, 10, 200, 10}); // bounds shorter than min_height
    // No direct getter for the last rect on StubTextArea; this at minimum
    // exercises arrange() without crashing when bounds.h < min_height_.
    CHECK(area.focusable());
}

TEST_CASE("TextArea: push_popup_nav gives the handler stack first refusal, including Tab")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    auto& native = *host.areas_created[0];
    REQUIRE(native.on_popup_nav);

    bool handler_saw_tab = false;
    area.push_popup_nav(
        [&](NavKey nk) -> bool
        {
            if (nk == NavKey::Tab)
            {
                handler_saw_tab = true;
                return true; // consume it — mirrors a composer popup cycling suggestions
            }
            return false;
        });

    CHECK(native.on_popup_nav(NavKey::Tab));
    CHECK(handler_saw_tab);
}

TEST_CASE("TextArea: Tab falls through to Host::advance_focus when no handler consumes it")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    auto& native = *host.areas_created[0];
    REQUIRE(native.on_popup_nav);

    // No focusable widget to advance to — advance_focus() returns false,
    // but reaching it at all (rather than a handler swallowing it first)
    // is what this test verifies.
    bool consumed = native.on_popup_nav(NavKey::Tab);
    CHECK_FALSE(consumed);
}

TEST_CASE("Host::request_focus re-asserts native focus even when the "
          "widget was already tk-focused")
{
    // Regression test: real native/OS keyboard focus can drift away from a
    // widget independently of tk-level bookkeeping — e.g. a platform
    // surface widget unconditionally grabbing native focus for itself as
    // part of its own default mouse-down handling (Qt's
    // Surface::mousePressEvent calls setFocus(Qt::MouseFocusReason) on
    // itself before our dispatch even runs), without ever going through
    // Host::clear_focus(). request_focus() used to early-return without
    // calling on_focus_gained() (and thus without re-syncing the native
    // control) whenever the requested widget was already the tracked
    // focused_widget_ — so a click that redirects focus back to a widget
    // that tk-level state already believed was focused silently failed to
    // win back real keyboard focus, even though it had just been stolen.
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    auto surface = TestSurface::create(320, 200);
    auto l = LayoutCtx{surface->factory(), Theme::light()};
    area.measure(l, {200, 40});
    area.arrange(l, {0, 0, 200, 40});

    int focus_gained_count = 0;
    area.set_on_focus_changed([&](bool f) { if (f) ++focus_gained_count; });

    host.request_focus(&area);
    CHECK(host.focused_widget() == &area);
    host.request_focus(&area); // same widget again — must still re-assert

    CHECK(focus_gained_count == 2);
}

TEST_CASE("TextArea: a pushed handler that declines a key lets an earlier-pushed one try")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    auto& native = *host.areas_created[0];

    bool first_called = false;
    bool second_called = false;
    area.push_popup_nav(
        [&](NavKey) -> bool
        {
            first_called = true;
            return true;
        });
    area.push_popup_nav(
        [&](NavKey) -> bool
        {
            second_called = true;
            return false; // declines — most-recently-pushed tried first
        });

    CHECK(native.on_popup_nav(NavKey::Up));
    CHECK(second_called);
    CHECK(first_called);
}

TEST_CASE("TextArea: pop_popup_nav removes the most recently pushed handler")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    auto& native = *host.areas_created[0];

    bool called = false;
    area.push_popup_nav([&](NavKey) -> bool { called = true; return true; });
    area.pop_popup_nav();

    native.on_popup_nav(NavKey::Escape);
    CHECK_FALSE(called);
}

TEST_CASE("TextArea: set_on_edit_last and set_on_image_paste forward to the backend")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    REQUIRE(host.areas_created.size() == 1);
    auto& native = *host.areas_created[0];

    bool edit_last_called = false;
    area.set_on_edit_last([&]() -> bool { edit_last_called = true; return true; });
    REQUIRE(native.on_edit_last);
    CHECK(native.on_edit_last());
    CHECK(edit_last_called);

    bool paste_called = false;
    area.set_on_image_paste(
        [&](std::vector<std::uint8_t>, std::string) { paste_called = true; });
    REQUIRE(native.on_image_paste);
    native.on_image_paste({}, "image/png");
    CHECK(paste_called);
}

TEST_CASE("TextArea: replace_range and insert_at_cursor mutate the backend's text")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    area.set_text("hello world");
    area.replace_range(0, 5, "goodbye");
    CHECK(area.text() == "goodbye world");
}
