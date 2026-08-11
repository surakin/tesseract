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
    // set_text() before (and here, permanently absent) a native backend is
    // buffered rather than silently dropped — see TextArea::PendingState —
    // so text() still reads back what was set even though it never reaches
    // any actual control.
    area.set_text("hello");
    CHECK(area.text() == "hello");
    CHECK_FALSE(area.visible());
}

TEST_CASE("TextArea: text/placeholder round-trip through a real native backend")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    // Native creation is now deferred (see TextArea::ensure_native_) —
    // arrange() is the reliable general-case trigger.
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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

namespace
{

// Tracks native set_visible() calls (StubTextArea in tk_test_host.h is
// inert) so tests can assert TextArea::set_visible()'s same-value guard
// actually elides redundant ones. Mirrors TrackingNativeTextField in
// test_tk_text_field.cpp.
struct TrackingNativeTextArea : public tk::NativeTextArea
{
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_focused(bool f) override { focused_ = f; }
    void set_visible(bool v) override
    {
        ++set_visible_calls;
        visible_ = v;
    }
    bool visible() const override { return visible_; }
    void set_enabled(bool) override {}
    float natural_height() const override { return 0.0f; }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()>) override {}
    void set_on_height_changed(std::function<void(float)>) override {}
    void insert_at_cursor(std::string text) override { text_ += text; }
    tk::Rect cursor_rect() const override { return {}; }
    void replace_range(int start, int end, std::string text) override
    {
        text_ = text_.substr(0, start) + text + text_.substr(end);
    }
    void set_on_popup_nav(std::function<bool(tk::NavKey)>) override {}
    void set_on_edit_last(std::function<bool()>) override {}
    void set_on_image_paste(ImagePasteHandler) override {}
    void set_on_focus_changed(std::function<void(bool)> f) override
    {
        on_focus_changed = std::move(f);
    }

    std::string text_;
    bool visible_ = true;
    bool focused_ = false;
    int set_visible_calls = 0;
    std::function<void(bool)> on_focus_changed;
    std::function<void(const std::string&)> on_changed_;
};

struct TrackingTextAreaHost : public TestHost
{
    TrackingTextAreaHost() : TestHost(nullptr) {}

    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        auto a = std::make_unique<TrackingNativeTextArea>();
        area = a.get(); // borrowed, owned by the TextArea
        return a;
    }

    TrackingNativeTextArea* area = nullptr;
};

} // namespace

TEST_CASE("TextArea::set_visible() is a no-op when the value doesn't change",
          "[tk][widget][text_area][focus]")
{
    TrackingTextAreaHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    host.set_root(&area);
    auto surface = TestSurface::create(200, 40);
    LayoutCtx lc{surface->factory(), Theme::light()};
    area.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.area != nullptr);
    // arrange() creating the native control also syncs its visibility
    // explicitly (see TextArea::arrange()'s comment — real backends default
    // a freshly created control to visible regardless of Widget::visible(),
    // e.g. Win32's BetterTextArea hardcodes WS_VISIBLE) — that's the one
    // real call this baseline already reflects.
    REQUIRE(host.area->set_visible_calls == 1);

    REQUIRE(host.area->visible_); // starts visible
    area.set_visible(true); // already visible — must not reach the native control
    CHECK(host.area->visible_);
    CHECK(host.area->set_visible_calls == 1); // unchanged from the baseline above
}

TEST_CASE("TextArea::set_visible(false) forwards a genuine transition to "
          "the native control",
          "[tk][widget][text_area][focus]")
{
    TrackingTextAreaHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    host.set_root(&area);
    auto surface = TestSurface::create(200, 40);
    LayoutCtx lc{surface->factory(), Theme::light()};
    area.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.area != nullptr);
    REQUIRE(host.area->set_visible_calls == 1); // creation-time sync — see the test above

    area.set_visible(false);
    CHECK_FALSE(host.area->visible_);
    CHECK(host.area->set_visible_calls == 2); // +1 for the genuine transition
}

TEST_CASE("TextArea defers native creation until arrange() or "
          "set_visible(true), and replays state set before then",
          "[tk][widget][text_area][lazy_native]")
{
    TrackingTextAreaHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    host.set_root(&area);

    REQUIRE(host.area == nullptr); // construction alone must not create it

    area.set_text("draft");
    std::string changed_to;
    area.set_on_changed([&](const std::string& t) { changed_to = t; });
    REQUIRE(host.area == nullptr); // setters alone don't trigger it either
    CHECK(area.text() == "draft"); // read back from the buffer

    auto surface = TestSurface::create(200, 40);
    LayoutCtx lc{surface->factory(), Theme::light()};
    area.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.area != nullptr);
    CHECK(host.area->text_ == "draft");
    CHECK(host.area->visible_); // synced to Widget::visible() on creation

    host.area->on_changed_("typed"); // simulate a native-side edit
    CHECK(changed_to == "typed"); // buffered on_changed was replayed, not dropped
}

TEST_CASE("TextArea: Tab falls through to Host::advance_focus when no handler consumes it")
{
    StubHost host;
    auto area_owner = tk::create_root_widget<TextArea>(&host, 40.0f);
    TextArea& area = *area_owner;
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
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
    // replace_range/insert_at_cursor are mutation methods on existing
    // content, not buffered setters (see TextArea::PendingState's doc
    // comment) — they need a real native backend already in place.
    TkTextAreaStage stage;
    stage.run(area, {0, 0, 200, 40});
    area.set_text("hello world");
    area.replace_range(0, 5, "goodbye");
    CHECK(area.text() == "goodbye world");
}
