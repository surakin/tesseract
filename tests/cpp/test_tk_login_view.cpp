#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/LoginView.h"
#include "tk_test_surface.h"

#include <memory>

using namespace tk;
using tesseract::views::LoginView;

namespace
{

struct StubTextField : public tk::NativeTextField
{
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_focused(bool) override {}
    void set_visible(bool) override {}
    void set_enabled(bool) override {}
    void set_password(bool) override {}
    void set_on_changed(std::function<void(const std::string&)> f) override
    {
        on_changed = std::move(f);
    }
    void set_on_submit(std::function<void()>) override {}

    std::string text_;
    std::function<void(const std::string&)> on_changed;
};

struct TkLoginViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
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

} // namespace

TEST_CASE("LoginView starts discovery for default homeserver on init",
          "[tk][view][login][discovery]")
{
    LoginView lv;
    lv.set_relayout([] {});  // required: hs_changed_() calls relayout_()

    auto stub = std::make_unique<StubTextField>();
    lv.init_with_field(std::move(stub));

    // Without the fix, discovery_state() is Idle after init — the default
    // text is set but on_changed is never called for a pre-populated field.
    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
}

TEST_CASE("LoginView re-triggers discovery after reset",
          "[tk][view][login][discovery]")
{
    LoginView lv;
    lv.set_relayout([] {});

    auto stub = std::make_unique<StubTextField>();
    lv.init_with_field(std::move(stub));

    // Simulate what happens when the user clicks Add Account: reset() is called.
    // Without the fix, discovery_state() goes back to Idle and stays there.
    lv.reset();

    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
}

TEST_CASE("LoginView Mode::Initial hides Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    LoginView lv; // default: Mode::Initial, State::Form
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::Initial);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::Initial keeps Cancel hidden in Waiting state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    LoginView lv;
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount shows Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    LoginView lv;
    lv.set_mode(LoginView::Mode::AddAccount);
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::AddAccount);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount keeps Cancel visible in Waiting state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    LoginView lv;
    lv.set_mode(LoginView::Mode::AddAccount);
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView toggling Mode flips Cancel visibility on its own",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    LoginView lv;
    st.run(lv, {0, 0, 640, 480});
    REQUIRE_FALSE(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::AddAccount);
    CHECK(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::Initial);
    CHECK_FALSE(lv.cancel_visible());
}
