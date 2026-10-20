#include <catch2/catch_test_macros.hpp>
#include <tesseract/launch_args.h>
#include <tesseract/settings.h>

#include "app/Launch.h"

#include <string>
#include <vector>

using tesseract::cli::Diagnostic;

TEST_CASE("parse_launch_args: no args returns defaults")
{
    auto args = tesseract::parse_launch_args({});
    CHECK(args.autostart == false);
    CHECK(!args.matrix_uri.has_value());
    CHECK(args.action == tesseract::LaunchAction::None);
    CHECK_FALSE(args.room_id);
}

TEST_CASE("parse_launch_args: --autostart alone")
{
    auto args = tesseract::parse_launch_args({"--autostart"});
    CHECK(args.autostart == true);
    CHECK(!args.matrix_uri.has_value());
}

TEST_CASE("parse_launch_args: matrix URI alone")
{
    auto args = tesseract::parse_launch_args({"https://matrix.to/#/@alice:example.org"});
    CHECK(args.autostart == false);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: --autostart then matrix URI")
{
    auto args =
        tesseract::parse_launch_args({"--autostart", "https://matrix.to/#/@alice:example.org"});
    CHECK(args.autostart == true);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: matrix URI then --autostart")
{
    auto args =
        tesseract::parse_launch_args({"https://matrix.to/#/@alice:example.org", "--autostart"});
    CHECK(args.autostart == true);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: unrecognised args are ignored")
{
    auto args = tesseract::parse_launch_args({"--bogus-flag", "not-a-uri"});
    CHECK(args.autostart == false);
    CHECK(!args.matrix_uri.has_value());
}

TEST_CASE("parse_launch_args: taskbar actions are parsed")
{
    CHECK(tesseract::parse_launch_args({"--open-quick-switcher"}).action ==
          tesseract::LaunchAction::QuickSwitcher);
    CHECK(tesseract::parse_launch_args({"--open-message-search"}).action ==
          tesseract::LaunchAction::MessageSearch);
    CHECK(tesseract::parse_launch_args({"--open-settings"}).action ==
          tesseract::LaunchAction::Settings);
}

TEST_CASE("parse_launch_args: first taskbar action wins")
{
    auto args =
        tesseract::parse_launch_args({"--open-settings", "--open-message-search", "--autostart"});
    CHECK(args.action == tesseract::LaunchAction::Settings);
    CHECK(args.autostart);
}

TEST_CASE("parse_launch_args: recent room action retains the room ID")
{
    auto args =
        tesseract::parse_launch_args({"--open-room=!abcdef:example.org", "--open-settings"});
    CHECK(args.action == tesseract::LaunchAction::Room);
    REQUIRE(args.room_id);
    CHECK(*args.room_id == "!abcdef:example.org");
}

TEST_CASE("parse_launch_args: empty recent room action is ignored")
{
    auto args = tesseract::parse_launch_args({"--open-room="});
    CHECK(args.action == tesseract::LaunchAction::None);
    CHECK_FALSE(args.room_id);
    REQUIRE(args.diagnostics.size() == 1);
    CHECK(args.diagnostics[0].kind == Diagnostic::Kind::InvalidValue);
}

TEST_CASE("parse_launch_args: open-room accepts a separate value")
{
    auto args = tesseract::parse_launch_args({"--open-room", "!abc:example.org"});
    CHECK(args.action == tesseract::LaunchAction::Room);
    REQUIRE(args.room_id);
    CHECK(*args.room_id == "!abc:example.org");
    CHECK_FALSE(args.matrix_uri);
}

TEST_CASE("parse_launch_args: unknown options produce diagnostics")
{
    auto args = tesseract::parse_launch_args({"--bogus-flag", "--autostart"});
    CHECK(args.autostart);
    REQUIRE(args.diagnostics.size() == 1);
    CHECK(args.diagnostics[0].kind == Diagnostic::Kind::Unknown);
    CHECK(args.diagnostics[0].arg == "--bogus-flag");
}

TEST_CASE("parse_launch_args: help, version, hidden, logoutall")
{
    auto a = tesseract::parse_launch_args({"-h"});
    CHECK(a.help);
    CHECK(tesseract::parse_launch_args({"--help"}).help);
    CHECK(tesseract::parse_launch_args({"-V"}).version);
    CHECK(tesseract::parse_launch_args({"--version"}).version);
    CHECK(tesseract::parse_launch_args({"--hidden"}).hidden);
    CHECK(tesseract::parse_launch_args({"--minimized"}).hidden);
    CHECK_FALSE(tesseract::parse_launch_args({"--hidden"}).autostart);
    CHECK(tesseract::parse_launch_args({"--logoutall"}).logout_all);
}

TEST_CASE("parse_launch_args: profile names are validated")
{
    auto ok = tesseract::parse_launch_args({"--profile=work"});
    REQUIRE(ok.profile);
    CHECK(*ok.profile == "work");
    CHECK(ok.diagnostics.empty());

    auto sep = tesseract::parse_launch_args({"-p", "home"});
    REQUIRE(sep.profile);
    CHECK(*sep.profile == "home");

    auto bad = tesseract::parse_launch_args({"--profile=../etc"});
    CHECK_FALSE(bad.profile);
    REQUIRE(bad.diagnostics.size() == 1);
    CHECK(bad.diagnostics[0].kind == Diagnostic::Kind::InvalidValue);
}

TEST_CASE("parse_launch_args: log level and verbose")
{
    CHECK(tesseract::parse_launch_args({"-v"}).log_filter == "debug");
    CHECK(tesseract::parse_launch_args({"--log-level=trace"}).log_filter == "trace");
    CHECK(tesseract::parse_launch_args({"--log-level", "matrix_sdk=debug,tesseract=trace"})
              .log_filter == "matrix_sdk=debug,tesseract=trace");
    // --log-level is more specific than --verbose regardless of order.
    CHECK(tesseract::parse_launch_args({"--log-level=info", "-v"}).log_filter == "info");

    auto bad = tesseract::parse_launch_args({"--log-level=loud"});
    CHECK_FALSE(bad.log_filter);
    REQUIRE(bad.diagnostics.size() == 1);
    CHECK(bad.diagnostics[0].kind == Diagnostic::Kind::InvalidValue);
}

TEST_CASE("parse_launch_args: toolkit arguments are skipped silently")
{
    auto args = tesseract::parse_launch_args({"-platform", "wayland", "-psn_0_12345",
                                              "-NSDocumentRevisionsDebugMode", "YES",
                                              "matrix:r/room:example.org"});
    CHECK(args.diagnostics.empty());
    REQUIRE(args.matrix_uri);
    CHECK(*args.matrix_uri == "matrix:r/room:example.org");
}

TEST_CASE("parse_launch_args: a URI after -- is still honoured")
{
    auto args = tesseract::parse_launch_args({"--", "matrix:r/room:example.org"});
    REQUIRE(args.matrix_uri);
}

TEST_CASE("launch_option_specs: help callback fills every visible option")
{
    auto specs = tesseract::launch_option_specs(
        [](std::string_view id)
        {
            return "help for " + std::string(id);
        });
    for (const auto& s : specs)
    {
        CHECK(s.help == "help for " + std::string(s.id));
    }
}

TEST_CASE("profile_relaunch_args")
{
    CHECK(tesseract::profile_relaunch_args("").empty());
    CHECK(tesseract::profile_relaunch_args("work") == std::vector<std::string>{"--profile=work"});
}

TEST_CASE("parse_launch_args: --relaunch waits for the single-instance lock")
{
    auto args = tesseract::parse_launch_args({"--relaunch", "--profile=work"});
    CHECK(args.relaunch);
    CHECK(args.diagnostics.empty());
    CHECK_FALSE(tesseract::parse_launch_args({}).relaunch);

    tesseract::LaunchPlan plan;
    CHECK(plan.instance_lock_wait().count() == 0);
    plan.args = args;
    CHECK(plan.instance_lock_wait().count() > 0);
}

TEST_CASE("relaunch_args ends with --relaunch")
{
    const auto args = tesseract::relaunch_args();
    REQUIRE_FALSE(args.empty());
    CHECK(args.back() == "--relaunch");
}

TEST_CASE("launch_help_text: --relaunch is internal and not listed")
{
    const std::string help = tesseract::launch_help_text("tesseract");
    CHECK(help.find("--relaunch") == std::string::npos);
    CHECK(help.find("--profile") != std::string::npos);
}

// ── LaunchPlan::start_hidden / should_raise_existing_instance ──────────────
// Both read the persisted "Start minimized to tray" preference, so each case
// saves and restores it to keep the suite order-independent.

namespace
{

class ScopedStartMinimized
{
public:
    explicit ScopedStartMinimized(bool value)
        : saved_(tesseract::Settings::instance().start_minimized)
    {
        tesseract::Settings::instance().start_minimized = value;
    }
    ~ScopedStartMinimized()
    {
        tesseract::Settings::instance().start_minimized = saved_;
    }
    ScopedStartMinimized(const ScopedStartMinimized&) = delete;
    ScopedStartMinimized& operator=(const ScopedStartMinimized&) = delete;

private:
    bool saved_;
};

tesseract::LaunchPlan plan_with(const std::vector<std::string>& argv)
{
    tesseract::LaunchPlan plan;
    plan.args = tesseract::parse_launch_args(argv);
    return plan;
}

} // namespace

TEST_CASE("LaunchPlan: default preference shows the window")
{
    ScopedStartMinimized pref(false);
    CHECK_FALSE(plan_with({}).start_hidden());
    // A login-item launch must not silently hide the app when the user never
    // asked for it — this is the behaviour change from autostart-implied-hide.
    CHECK_FALSE(plan_with({"--autostart"}).start_hidden());
}

TEST_CASE("LaunchPlan: start_minimized hides every kind of launch")
{
    ScopedStartMinimized pref(true);
    CHECK(plan_with({}).start_hidden());
    CHECK(plan_with({"--autostart"}).start_hidden());
    CHECK(plan_with({"--open-room=!room:example.org"}).start_hidden());
}

TEST_CASE("LaunchPlan: --hidden always starts hidden, pref or not")
{
    ScopedStartMinimized off(false);
    CHECK(plan_with({"--hidden"}).start_hidden());
    CHECK(plan_with({"--minimized"}).start_hidden());
    CHECK(plan_with({"--hidden", "--autostart"}).start_hidden());

    ScopedStartMinimized on(true);
    CHECK(plan_with({"--hidden"}).start_hidden());
}

TEST_CASE("LaunchPlan: a duplicate launch always raises, whatever the pref")
{
    // Otherwise an app living in the tray would be reachable only by clicking
    // the tray icon: launching it again would quietly exit and show nothing.
    ScopedStartMinimized on(true);
    CHECK(plan_with({}).should_raise_existing_instance());

    ScopedStartMinimized off(false);
    CHECK(plan_with({}).should_raise_existing_instance());
}

TEST_CASE("LaunchPlan: an OS-started duplicate stays out of the way")
{
    ScopedStartMinimized on(true);
    CHECK_FALSE(plan_with({"--autostart"}).should_raise_existing_instance());
}

TEST_CASE("LaunchPlan: --hidden with nothing to forward stays quiet")
{
    ScopedStartMinimized off(false);
    CHECK_FALSE(plan_with({"--hidden"}).should_raise_existing_instance());
}

TEST_CASE("LaunchPlan: anything to forward reaches the running instance")
{
    ScopedStartMinimized off(false);
    CHECK(plan_with({"--open-room=!room:example.org"}).should_raise_existing_instance());
    CHECK(plan_with({"--open-settings"}).should_raise_existing_instance());
    CHECK(plan_with({"--open-settings", "--autostart"}).should_raise_existing_instance());
    CHECK(plan_with({"matrix:u/user:example.org"}).should_raise_existing_instance());
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
TEST_CASE("parse_launch_args: screenshot output is available in CI builds")
{
    auto args = tesseract::parse_launch_args({"--screenshot-dir=/tmp/tesseract-shots"});
    REQUIRE(args.screenshot_dir);
    CHECK(*args.screenshot_dir == "/tmp/tesseract-shots");
    CHECK_FALSE(args.autostart);
    CHECK_FALSE(args.matrix_uri);
}
#endif

#ifdef TESSERACT_SMOKE_MODE_ENABLED
TEST_CASE("parse_launch_args: smoke result path is available in CI builds")
{
    auto args = tesseract::parse_launch_args({"--smoke-test=/tmp/tesseract-smoke.json"});
    REQUIRE(args.smoke_result);
    CHECK(*args.smoke_result == "/tmp/tesseract-smoke.json");
    CHECK_FALSE(args.autostart);
    CHECK_FALSE(args.matrix_uri);
}
#endif

TEST_CASE("parse_launch_args: options after -- are not honoured")
{
    auto args = tesseract::parse_launch_args({"--", "--logoutall"});
    CHECK_FALSE(args.logout_all);
}

TEST_CASE("parse_launch_args: matrix URI after -- is still recognised")
{
    auto args = tesseract::parse_launch_args({"--", "matrix:r/room:example.org"});
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "matrix:r/room:example.org");
}

TEST_CASE("parse_launch_args: quote break-out after -- stays positional")
{
    // What CommandLineToArgvW yields for  "exe" -- "matrix:x" --logoutall ""
    // i.e. a URI containing a raw quote launched through the registered handler.
    auto args = tesseract::parse_launch_args({"--", "matrix:r/x:example.org", "--logoutall", ""});
    CHECK_FALSE(args.logout_all);
}
