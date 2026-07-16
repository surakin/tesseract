#include <catch2/catch_test_macros.hpp>

#include "tk/text_field.h"
#include "tk/widget.h"
#include "tk_test_host.h"

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
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_enabled(bool) override {}
    void set_password(bool) override {}
    void set_on_changed(std::function<void(const std::string&)>) override {}
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
    bool visible_ = true;
    bool focused_ = false;
    int set_visible_calls = 0;
    std::function<void(bool)> on_focus_changed;
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
    REQUIRE(host.field != nullptr);

    REQUIRE(host.field->visible_); // starts visible
    field.set_visible(true); // already visible — must not reach the native control
    CHECK(host.field->visible_);
    CHECK(host.field->set_visible_calls == 0);
}

TEST_CASE("TextField::set_visible(false) forwards a genuine transition to "
          "the native control",
          "[tk][widget][text_field][focus]")
{
    TrackingTextFieldHost host;
    auto field_owner = tk::create_root_widget<TextField>(&host, 40.0f);
    TextField& field = *field_owner;
    host.set_root(&field);
    REQUIRE(host.field != nullptr);

    field.set_visible(false);
    CHECK_FALSE(host.field->visible_);
    CHECK(host.field->set_visible_calls == 1);
}
