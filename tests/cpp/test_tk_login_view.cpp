#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/i18n.h"
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
    LayoutCtx layout_ctx() const
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds) const
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
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {}); // required: hs_changed_() calls relayout_()
    lv.finish_init();

    // The homeserver field shows "matrix.org" as a placeholder only — it has
    // no real text after finish_init(). Regression coverage for treating an
    // empty field as matrix.org by default: discovery must still run (and
    // reach Discovering here, since no client_ is wired up to complete it)
    // rather than sitting Idle because nothing was typed.
    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
    // Fail-open: the OAuth button stays visible while discovery is pending.
    CHECK(lv.sign_in_visible());
}

TEST_CASE("LoginView re-triggers discovery after reset", "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
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
    auto lv_owner =
        tk::create_root_widget<LoginView>(&host); // finish_init() not called — no discovery has run
    LoginView& lv = *lv_owner;
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.sign_in_visible());
}

TEST_CASE("LoginView Mode::Initial hides Cancel in Form state", "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host); // default: Mode::Initial, State::Form
    LoginView& lv = *lv_owner;
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::Initial);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::Initial keeps Cancel hidden on the form but shows it "
          "while Waiting",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    st.run(lv, {0, 0, 640, 480});
    CHECK_FALSE(lv.cancel_visible());

    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});
    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount shows Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
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
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
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
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    st.run(lv, {0, 0, 640, 480});
    REQUIRE_FALSE(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::AddAccount);
    CHECK(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::Initial);
    CHECK_FALSE(lv.cancel_visible());
}

// ── Enter in the homeserver field ───────────────────────────────────────────

TEST_CASE("LoginView remembers Enter pressed while the server is being checked",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init(); // discovery starts (Discovering)

    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Wait);
    lv.simulate_homeserver_enter();
    CHECK(lv.homeserver_submit_pending());

    // The lookup answers (no client here, so there's no OAuth probe to wait
    // for): the remembered Enter is acted on.
    lv.simulate_discovery_result(true, "https://example.org", false);
    CHECK_FALSE(lv.homeserver_submit_pending());
}

TEST_CASE("LoginView sign-in controls are inert until the server's methods are known",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init(); // discovery starts (Discovering)

    CHECK_FALSE(lv.sign_in_enabled());
    CHECK_FALSE(lv.register_enabled());

    // The lookup answers (no client here, so no OAuth probe is left to wait for).
    lv.simulate_discovery_result(true, "https://example.org", false);
    CHECK(lv.sign_in_enabled());
    CHECK(lv.register_enabled());
}

TEST_CASE("LoginView keeps waiting until the server's sign-in methods are known",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    lv.simulate_homeserver_enter();
    // Resolved alone (before the lookup's password answer) isn't enough.
    lv.set_discovery_state(LoginView::DiscoveryState::Resolved, "https://example.org");
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Wait);
    CHECK(lv.homeserver_submit_pending());
}

TEST_CASE("LoginView typing a different server drops a remembered Enter",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    lv.simulate_homeserver_enter();
    REQUIRE(lv.homeserver_submit_pending());
    lv.reset(); // re-runs discovery for a fresh server, like typing
    CHECK_FALSE(lv.homeserver_submit_pending());
}

TEST_CASE("LoginView Enter opens password login on a password-only server",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    lv.simulate_discovery_result(true, "https://example.org", /*supports_password=*/true);
    lv.simulate_oauth_probe_result(false);
    CHECK_FALSE(lv.sign_in_visible());
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Password);
#else
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Nothing);
#endif
}

TEST_CASE("LoginView Enter does nothing when the server offers neither",
          "[tk][view][login][discovery]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    lv.simulate_discovery_result(true, "https://example.org", false);
    lv.simulate_oauth_probe_result(false);
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Nothing);
}

TEST_CASE("LoginView settings button shows only on the first-login form",
          "[tk][view][login][settings]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();

    int opened = 0;
    lv.set_on_open_settings(
        [&]
        {
            ++opened;
        });

    CHECK(lv.settings_visible());
    lv.simulate_settings_click();
    CHECK(opened == 1);

    lv.set_state(LoginView::State::Waiting);
    CHECK_FALSE(lv.settings_visible());
    lv.set_state(LoginView::State::Form);
    CHECK(lv.settings_visible());

    lv.set_mode(LoginView::Mode::AddAccount);
    CHECK_FALSE(lv.settings_visible());
}

TEST_CASE("LoginView show_missing_session_notice is safe to call repeatedly",
          "[tk][view][login][alert]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();

    lv.show_missing_session_notice({});
    lv.show_missing_session_notice({"@ghost:example.org"});
    lv.show_missing_session_notice({"@a:x", "@b:x"});
}

TEST_CASE("LoginView web auth: done is not called after cancel", "[tk][view][login][oauth]")
{
    using tesseract::views::WebAuthDone;
    using tesseract::views::WebAuthResult;

    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_mode(LoginView::Mode::AddAccount);
    lv.set_relayout([] {});

    bool started = false;
    bool cancelled = false;
    int cancel_calls = 0;
    WebAuthDone recorded_done;
    lv.set_web_auth(
        []
        {
            return true;
        },
        [&](const std::string&, const std::string& scheme, WebAuthDone done)
        {
            started = true;
            CHECK(scheme == "io.github.surakin");
            recorded_done = std::move(done);
            return std::function<void()>(
                [&]
                {
                    cancelled = true;
                    ++cancel_calls;
                });
        });

    lv.begin_completed_for_test_(true, "https://auth/x", "io.github.surakin:/callback");
    CHECK(started);

    lv.simulate_cancel_click();
    CHECK(cancelled);
    CHECK(lv.state() == LoginView::State::Form);

    REQUIRE(recorded_done);
    recorded_done({WebAuthResult::Outcome::Callback, "io.github.surakin:/callback?code=a"});
    CHECK(lv.state() == LoginView::State::Form);
    CHECK(lv.status_text().empty());
    CHECK(cancel_calls == 1);
}

TEST_CASE("LoginView web auth: a failed sheet keeps the error on the form",
          "[tk][view][login][oauth]")
{
    using tesseract::views::WebAuthDone;
    using tesseract::views::WebAuthResult;

    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_mode(LoginView::Mode::AddAccount);
    lv.set_relayout([] {});

    bool cancel_done = false;
    lv.set_on_cancel_done(
        [&]
        {
            cancel_done = true;
        });
    WebAuthDone recorded_done;
    lv.set_web_auth(
        []
        {
            return true;
        },
        [&](const std::string&, const std::string&, WebAuthDone done)
        {
            recorded_done = std::move(done);
            return std::function<void()>([] {});
        });

    lv.begin_completed_for_test_(true, "https://auth/x", "io.github.surakin:/callback");
    REQUIRE(recorded_done);
    recorded_done({WebAuthResult::Outcome::Failed, ""});

    CHECK_FALSE(cancel_done);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK(lv.status_text() == tk::tr("The sign-in window could not be opened."));
}
