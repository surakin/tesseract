#include <catch2/catch_test_macros.hpp>

#include "tk/text_field.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

// Exercises tk::TextField::set_visible()'s same-value native-forward guard.
// See test_encryption_setup_overlay.cpp for the actual regression coverage
// of the recovery-key focus bug this guard is one half of fixing — the
// other half is EncryptionSetupOverlay::paint() no longer calling
// set_visible(false) on a field that's about to stay the active one.

using namespace tk;

namespace
{

// Tracks native set_visible() calls (StubTextField in tk_test_host.h is
// inert) so tests can assert the guard actually elides redundant ones.
struct TrackingNativeTextField : public tk::NativeTextField
{
    void set_rect(tk::Rect r) override { last_rect = r; }
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_enabled(bool) override {}
    void set_password(bool) override {}
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()>) override {}
    void set_focused(bool f) override { focused_ = f; }
    void set_visible(bool v) override
    {
        ++set_visible_calls;
        visible_ = v;
    }
    void set_on_focus_changed(std::function<void(bool)> f) override
    {
        on_focus_changed = std::move(f);
    }

    std::string text_;
    tk::Rect last_rect{};
    bool visible_ = true;
    bool focused_ = false;
    int set_visible_calls = 0;
    std::function<void(bool)> on_focus_changed;
    std::function<void(const std::string&)> on_changed_;
};

struct TrackingTextFieldHost : public TestHost
{
    TrackingTextFieldHost() : TestHost(nullptr) {}

    std::unique_ptr<tk::NativeTextField> make_text_field() override
    {
        auto f = std::make_unique<TrackingNativeTextField>();
        field = f.get(); // borrowed, owned by the TextField
        return f;
    }

    TrackingNativeTextField* field = nullptr;
};

} // namespace

TEST_CASE("TextField::set_visible() is a no-op when the value doesn't change",
          "[tk][widget][text_field][focus]")
{
    TrackingTextFieldHost host;
    auto field_owner = tk::create_root_widget<TextField>(&host, 40.0f);
    TextField& field = *field_owner;
    host.set_root(&field);
    // The native control is created lazily (see TextField::ensure_native_)
    // — arrange() is the reliable trigger for a field that's visible by
    // Widget's own default and never receives an explicit set_visible(true).
    auto surface = TestSurface::create(200, 40);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    field.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.field != nullptr);
    // arrange() creating the native control also syncs its visibility
    // explicitly (see TextField::arrange()'s comment — real backends default
    // a freshly created control to visible regardless of Widget::visible(),
    // e.g. Win32's BetterTextField hardcodes WS_VISIBLE) — that's the one
    // real call this baseline already reflects.
    REQUIRE(host.field->set_visible_calls == 1);

    REQUIRE(host.field->visible_); // starts visible
    field.set_visible(true); // already visible — must not reach the native control
    CHECK(host.field->visible_);
    CHECK(host.field->set_visible_calls == 1); // unchanged from the baseline above
}

TEST_CASE("TextField::set_visible(false) forwards a genuine transition to "
          "the native control",
          "[tk][widget][text_field][focus]")
{
    TrackingTextFieldHost host;
    auto field_owner = tk::create_root_widget<TextField>(&host, 40.0f);
    TextField& field = *field_owner;
    host.set_root(&field);
    auto surface = TestSurface::create(200, 40);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    field.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.field != nullptr);
    REQUIRE(host.field->set_visible_calls == 1); // creation-time sync — see the test above

    field.set_visible(false);
    CHECK_FALSE(host.field->visible_);
    CHECK(host.field->set_visible_calls == 2); // +1 for the genuine transition
}

TEST_CASE("TextField defers native creation until arrange() or "
          "set_visible(true), and replays state set before then",
          "[tk][widget][text_field][lazy_native]")
{
    TrackingTextFieldHost host;
    auto field_owner = tk::create_root_widget<TextField>(&host, 40.0f);
    TextField& field = *field_owner;
    host.set_root(&field);

    // Nothing native yet — construction alone must not create it (that's
    // the whole point of the deferral).
    REQUIRE(host.field == nullptr);

    // State set before the native control exists must be buffered, not
    // silently dropped.
    field.set_text("hello");
    field.set_placeholder("type here");
    std::string changed_to;
    field.set_on_changed([&](const std::string& t) { changed_to = t; });
    REQUIRE(host.field == nullptr); // still not created — setters alone don't trigger it
    CHECK(field.text() == "hello"); // read back from the buffer

    // First set_visible(true) — starting from an explicit false, so it's a
    // genuine transition — creates the native control and replays the
    // buffered state into it.
    field.set_visible(false);
    REQUIRE(host.field == nullptr);
    field.set_visible(true);
    REQUIRE(host.field != nullptr);
    CHECK(host.field->text_ == "hello");
    REQUIRE(field.text() == "hello"); // now reads through to the native control

    host.field->on_changed_("typed"); // simulate a native-side edit
    CHECK(changed_to == "typed"); // buffered on_changed was replayed, not dropped
}

TEST_CASE("TextField insets the native control inside its own rect so an "
          "opaque capture can't overpaint a view's border box",
          "[tk][widget][text_field][overlay_inset]")
{
    TrackingTextFieldHost host;
    auto field_owner = tk::create_root_widget<TextField>(&host, 40.0f);
    TextField& field = *field_owner;
    host.set_root(&field);
    auto surface = TestSurface::create(200, 40);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};

    // Default: 2px inset on every side.
    field.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    REQUIRE(host.field != nullptr);
    CHECK(host.field->last_rect.x == 2.0f);
    CHECK(host.field->last_rect.y == 2.0f);
    CHECK(host.field->last_rect.w == 196.0f);
    CHECK(host.field->last_rect.h == 36.0f);

    // Opt-out: full-bleed for views that draw no box / clip themselves.
    field.set_overlay_inset(0.0f);
    field.arrange(lc, {0.0f, 0.0f, 200.0f, 40.0f});
    CHECK(host.field->last_rect.x == 0.0f);
    CHECK(host.field->last_rect.y == 0.0f);
    CHECK(host.field->last_rect.w == 200.0f);
    CHECK(host.field->last_rect.h == 40.0f);

    // A rect narrower than twice the inset must clamp to zero, never go
    // negative (e.g. an offscreen 1x1 paste sink). Height has a min_height
    // floor; width does not, so it's the one that can underflow.
    TrackingTextFieldHost tiny_host;
    auto tiny_owner = tk::create_root_widget<TextField>(&tiny_host, 0.0f);
    TextField& tiny = *tiny_owner;
    tiny_host.set_root(&tiny);
    tiny.arrange(lc, {0.0f, 0.0f, 1.0f, 1.0f});
    REQUIRE(tiny_host.field != nullptr);
    CHECK(tiny_host.field->last_rect.w == 0.0f);
    CHECK(tiny_host.field->last_rect.h == 0.0f);
}
