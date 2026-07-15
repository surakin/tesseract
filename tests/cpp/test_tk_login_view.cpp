#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/LoginView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

using namespace tk;
using tesseract::views::LoginView;

namespace
{

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
    StubHost host;
    LoginView lv(host);
    lv.set_relayout([] {});  // required: hs_changed_() calls relayout_()
    lv.finish_init();

    // Without the fix, discovery_state() is Idle after init — the default
    // text is set but on_changed is never called for a pre-populated field.
    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
    // Fail-open: the OAuth button stays visible while discovery is pending.
    CHECK(lv.sign_in_visible());
}

TEST_CASE("LoginView re-triggers discovery after reset",
          "[tk][view][login][discovery]")
{
    StubHost host;
    LoginView lv(host);
    lv.set_relayout([] {});
    lv.finish_init();

    // Simulate what happens when the user clicks Add Account: reset() is called.
    // Without the fix, discovery_state() goes back to Idle and stays there.
    lv.reset();

    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
    CHECK(lv.sign_in_visible());
}

TEST_CASE("LoginView shows Sign in button by default before any discovery",
          "[tk][view][login][discovery]")
{
    TkLoginViewStage st;
    StubHost host;
    LoginView lv(host); // finish_init() not called — no discovery has run
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.sign_in_visible());
}

TEST_CASE("LoginView Mode::Initial hides Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    LoginView lv(host); // default: Mode::Initial, State::Form
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::Initial);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::Initial keeps Cancel hidden in Waiting state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    LoginView lv(host);
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount shows Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    LoginView lv(host);
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
    StubHost host;
    LoginView lv(host);
    lv.set_mode(LoginView::Mode::AddAccount);
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView toggling Mode flips Cancel visibility on its own",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    LoginView lv(host);
    st.run(lv, {0, 0, 640, 480});
    REQUIRE_FALSE(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::AddAccount);
    CHECK(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::Initial);
    CHECK_FALSE(lv.cancel_visible());
}
