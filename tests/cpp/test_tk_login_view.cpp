#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/i18n.h"
#include "views/LoginView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <cstdlib>
#include <memory>
#include <vector>

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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED

namespace
{

using Idp = tesseract::Client::IdentityProvider;

// Settles discovery on an OAuth-less server that offers SSO.
void settle_sso(LoginView& lv, std::vector<Idp> idps, bool supports_password = false)
{
    lv.simulate_discovery_result(true, "https://example.org", supports_password, true,
                                 std::move(idps));
    lv.simulate_oauth_probe_result(false);
}

std::unique_ptr<LoginView> make_login_view(StubHost& host)
{
    auto lv = tk::create_root_widget<LoginView>(&host);
    lv->set_relayout([] {});
    lv->finish_init();
    return lv;
}

// Not a decodable image: the staleness checks only need non-empty bytes.
const std::vector<std::uint8_t> kFakeIcon{0x89, 'P', 'N', 'G'};

// A real 2x2 RGBA PNG.
const std::vector<std::uint8_t> kPng2x2{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
    0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8,
    0xCF, 0xC0, 0xF0, 0x1F, 0x84, 0x19, 0x60, 0x0C, 0x00, 0x47, 0xCA, 0x07, 0xF9, 0x67, 0x59,
    0x6E, 0xB7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

} // namespace

TEST_CASE("LoginView shows one SSO button per provider when OAuth is unavailable",
          "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}, {"corp", "Corp", "", ""}});

    CHECK_FALSE(lv.sign_in_visible());
    CHECK(lv.sso_visible());
    CHECK(lv.sso_button_labels() ==
          std::vector<std::string>{tk::trf(tk::tr("Continue with {0}"), {"GitHub"}),
                                   tk::trf(tk::tr("Continue with {0}"), {"Corp"})});
}

TEST_CASE("LoginView shows a single SSO button when no providers are listed",
          "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {});

    CHECK(lv.sso_button_labels() == std::vector<std::string>{tk::tr("Sign in with SSO")});
}

TEST_CASE("LoginView hides SSO when OAuth is available", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    lv.simulate_discovery_result(true, "https://example.org", false, true,
                                 {{"github", "GitHub", "", ""}});
    lv.simulate_oauth_probe_result(true);

    CHECK_FALSE(lv.sso_visible());
    CHECK(lv.sign_in_visible());
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::OAuth);
}

TEST_CASE("LoginView SSO and password: Enter opens the password form", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}}, /*supports_password=*/true);

    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Password);
    CHECK(lv.sso_visible());
}

TEST_CASE("LoginView single SSO button: Enter starts SSO", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}});

    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Sso);
}

TEST_CASE("LoginView several SSO buttons: Enter does nothing", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}, {"corp", "Corp", "", ""}});

    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Nothing);
}

TEST_CASE("LoginView SSO buttons begin the flow for their own provider", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    std::vector<std::string> begun;
    lv.set_on_begin_sso_for_test(
        [&begun](const std::string& id)
        {
            begun.push_back(id);
        });

    settle_sso(lv, {{"oidc-github", "GitHub", "", ""}, {"saml", "Corp", "", ""}});
    lv.simulate_sso_click(1);
    REQUIRE(begun.size() == 1);
    CHECK(begun[0] == "saml");

    settle_sso(lv, {});
    lv.simulate_sso_click(0);
    REQUIRE(begun.size() == 2);
    CHECK(begun[1].empty());
}

TEST_CASE("LoginView fetches SSO icons only once the SSO box is shown", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;

    lv.simulate_discovery_result(true, "https://example.org", false, true,
                                 {{"github", "GitHub", "mxc://x/y", ""}});
    CHECK_FALSE(lv.sso_icons_requested()); // OAuth probe still pending

    lv.simulate_oauth_probe_result(false);
    CHECK(lv.sso_icons_requested());

    // OAuth available: the SSO box stays hidden and nothing is fetched.
    auto lv2_owner = make_login_view(host);
    LoginView& lv2 = *lv2_owner;
    lv2.simulate_discovery_result(true, "https://example.org", false, true,
                                  {{"github", "GitHub", "mxc://x/y", ""}});
    lv2.simulate_oauth_probe_result(true);
    CHECK_FALSE(lv2.sso_icons_requested());
}

TEST_CASE("LoginView drops a stale SSO icon after rediscovery", "[tk][view][login][sso]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "mxc://x/y", ""}});
    const uint32_t old_gen = lv.sso_icon_gen();

    settle_sso(lv, {{"github", "GitHub", "mxc://x/y", ""}}); // rediscovery
    CHECK(lv.sso_icon_gen() != old_gen);

    lv.simulate_login_icon_result(old_gen, 0, kFakeIcon);
    st.run(lv, {0, 0, 640, 480});
    CHECK_FALSE(lv.sso_button_has_image(0));
}

TEST_CASE("LoginView SSO button list is cleared by a rediscovery without providers",
          "[tk][view][login][sso]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}, {"corp", "Corp", "", ""}});
    st.run(lv, {0, 0, 640, 480});
    REQUIRE(lv.sso_button_labels().size() == 2);

    lv.simulate_discovery_result(true, "https://example.org", false, false, {});
    lv.simulate_oauth_probe_result(false);
    st.run(lv, {0, 0, 640, 480});
    CHECK_FALSE(lv.sso_visible());
    CHECK(lv.sso_button_labels().empty());
    CHECK(lv.homeserver_submit_action() == LoginView::SubmitAction::Nothing);
}

TEST_CASE("LoginView SSO click enters waiting state with Cancel visible", "[tk][view][login][sso]")
{
    using tesseract::views::WebAuthDone;

    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", ""}});

    // No client: the click is a no-op and the form stays as it was.
    lv.simulate_sso_click(0);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK_FALSE(lv.cancel_visible());

    // The browser step the SSO flow shares with OAuth (a stub web-auth sheet
    // keeps the real browser closed).
    lv.set_web_auth(
        []
        {
            return true;
        },
        [](const std::string&, const std::string&, WebAuthDone)
        {
            return std::function<void()>([] {});
        });
    lv.set_state(LoginView::State::Waiting);
    lv.begin_completed_for_test_(true, "https://auth/x", "io.github.surakin:/callback");
    st.run(lv, {0, 0, 640, 480});
    CHECK(lv.state() == LoginView::State::Waiting);
    CHECK(lv.cancel_visible());
    CHECK_FALSE(lv.sso_visible());
}

TEST_CASE("LoginView brand SSO button paints its icon", "[tk][view][login][sso]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "", "github"}});

    auto lc = st.layout_ctx();
    lv.measure(lc, {640, 480});
    lv.arrange(lc, {0, 0, 640, 480});
    PaintCtx pc{st.surface->canvas(), st.surface->factory(), Theme::light()};
    lv.paint(pc);

    // The leading icon sits in a 16px slot at the start of the (centred)
    // content; a drawn glyph differs from the button's plain fill there.
    const tk::Button* btn = lv.sso_button(0);
    REQUIRE(btn != nullptr);
    const tk::Rect b = btn->bounds();
    const float content_w = const_cast<tk::Button*>(btn)->measure(lc, {-1, -1}).w - 2 * 16.0f;
    const int cx = static_cast<int>(b.x + (b.w - content_w) * 0.5f + 8.0f);
    const int cy = static_cast<int>(b.y + b.h * 0.5f);
    const tk::Color bg = st.surface->read_pixel(static_cast<int>(b.x) + 4, cy);
    bool painted = false;
    for (int dy = -5; dy <= 5 && !painted; ++dy)
    {
        for (int dx = -5; dx <= 5 && !painted; ++dx)
        {
            const tk::Color px = st.surface->read_pixel(cx + dx, cy + dy);
            painted = std::abs(px.r - bg.r) + std::abs(px.g - bg.g) + std::abs(px.b - bg.b) > 40;
        }
    }
    CHECK(painted);
}

TEST_CASE("LoginView SSO monogram is one uppercase letter", "[tk][view][login][sso]")
{
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"acme", "acme sso", "", ""}, {"corp", "Corp Login", "", ""}});

    CHECK(lv.sso_button(0)->leading_monogram() == "A");
    CHECK(lv.sso_button(1)->leading_monogram() == "C");
}

TEST_CASE("LoginView shows a downloaded SSO icon after layout", "[tk][view][login][sso]")
{
    TkLoginViewStage st;
    StubHost host;
    auto lv_owner = make_login_view(host);
    LoginView& lv = *lv_owner;
    settle_sso(lv, {{"github", "GitHub", "mxc://x/y", ""}});

    lv.simulate_login_icon_result(lv.sso_icon_gen(), 0, kPng2x2);
    st.run(lv, {0, 0, 640, 480});
    CHECK(lv.sso_button_has_image(0));
}

#endif
