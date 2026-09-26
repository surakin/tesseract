#include "tesseract/launch_args.h"

#include "tesseract/client.h"
#include "tesseract/paths.h"

#include <array>
#include <string_view>
#include <utility>

namespace tesseract
{

namespace
{

using cli::Arity;
using cli::Diagnostic;
using cli::OptionSpec;

bool is_valid_log_filter(std::string_view v)
{
    static constexpr std::array<std::string_view, 5> kLevels = {
        "error", "warn", "info", "debug", "trace"};
    for (auto level : kLevels)
    {
        if (v == level)
        {
            return true;
        }
    }
    // A full tracing EnvFilter directive list ("matrix_sdk=debug,tesseract=trace").
    // The Rust side validates it properly; here just reject obvious junk.
    return !v.empty() && (v.find('=') != std::string_view::npos ||
                          v.find(',') != std::string_view::npos);
}

} // namespace

LaunchAction launch_action_from_option_id(std::string_view id)
{
    if (id == "open-quick-switcher")
    {
        return LaunchAction::QuickSwitcher;
    }
    if (id == "open-message-search")
    {
        return LaunchAction::MessageSearch;
    }
    if (id == "open-settings")
    {
        return LaunchAction::Settings;
    }
    if (id == "open-room")
    {
        return LaunchAction::Room;
    }
    return LaunchAction::None;
}

std::string_view launch_action_option_id(LaunchAction action)
{
    switch (action)
    {
    case LaunchAction::QuickSwitcher:
        return "open-quick-switcher";
    case LaunchAction::MessageSearch:
        return "open-message-search";
    case LaunchAction::Settings:
        return "open-settings";
    case LaunchAction::Room:
        return "open-room";
    case LaunchAction::None:
        break;
    }
    return {};
}

std::vector<OptionSpec> launch_option_specs(const LaunchHelpFn& help)
{
    auto h = [&help](std::string_view id)
    { return help ? help(id) : std::string{}; };

    std::vector<OptionSpec> specs = {
        {"help", "help", 'h', Arity::Flag, "", h("help"), false, false, {}},
        {"version", "version", 'V', Arity::Flag, "", h("version"), false, false, {}},
        {"profile", "profile", 'p', Arity::Required, "NAME", h("profile"), false,
         false, {}},
        {"hidden", "hidden", '\0', Arity::Flag, "", h("hidden"), false, false,
         {"minimized"}},
        {"log-level", "log-level", '\0', Arity::Required, "LEVEL",
         h("log-level"), false, false, {}},
        {"verbose", "verbose", 'v', Arity::Flag, "", h("verbose"), false, false,
         {}},
        {"open-room", "open-room", '\0', Arity::Required, "ROOM_ID",
         h("open-room"), false, false, {}},
        {"open-quick-switcher", "open-quick-switcher", '\0', Arity::Flag, "",
         h("open-quick-switcher"), false, false, {}},
        {"open-message-search", "open-message-search", '\0', Arity::Flag, "",
         h("open-message-search"), false, false, {}},
        {"open-settings", "open-settings", '\0', Arity::Flag, "",
         h("open-settings"), false, false, {}},
        {"logoutall", "logoutall", '\0', Arity::Flag, "", h("logoutall"), false,
         false, {}},
        // Written by the OS login-item registration, not typed by users.
        {"autostart", "autostart", '\0', Arity::Flag, "", h("autostart"), false,
         true, {}},
        // Passed by the app to itself when it restarts; not typed by users.
        {"relaunch", "relaunch", '\0', Arity::Flag, "", h("relaunch"), false,
         true, {}},
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        {"screenshot-dir", "screenshot-dir", '\0', Arity::Required, "DIR",
         h("screenshot-dir"), false, true, {}},
#endif
    };
    return specs;
}

std::span<const cli::ForeignOption> launch_foreign_options()
{
    // Qt's QGuiApplication/QApplication command-line switches (see the
    // QGuiApplication and QApplication class docs), AppKit's NSUserDefaults
    // argument domain (-NSFoo value, -AppleLanguages (...)), and the Carbon
    // process serial number Finder used to append (-psn_0_12345).
    static constexpr std::array<cli::ForeignOption, 16> kForeign = {{
        {"-psn_", false},
        {"-NS", true},
        {"-Apple", true},
        {"-platformpluginpath", true},
        {"-platformtheme", true},
        {"-platform", true},
        {"-plugin", true},
        {"-qmljsdebugger", false},
        {"-qwindowgeometry", true},
        {"-qwindowicon", true},
        {"-qwindowtitle", true},
        {"-reverse", false},
        {"-session", true},
        {"-display", true},
        {"-stylesheet", true},
        {"-style", true},
    }};
    return kForeign;
}

LaunchArgs parse_launch_args(const std::vector<std::string>& args)
{
    const auto specs = launch_option_specs();
    const auto parsed = cli::parse(specs, args, launch_foreign_options());

    LaunchArgs result;
    result.diagnostics = parsed.diagnostics();

    result.autostart = parsed.has("autostart");
    result.hidden = parsed.has("hidden");
    result.help = parsed.has("help");
    result.version = parsed.has("version");
    result.logout_all = parsed.has("logoutall");
    result.relaunch = parsed.has("relaunch");

    if (auto p = parsed.value("profile"))
    {
        if (is_valid_profile_name(*p))
        {
            result.profile = std::move(*p);
        }
        else
        {
            result.diagnostics.push_back(
                {Diagnostic::Kind::InvalidValue, "--profile",
                 "expected 1-32 characters from [A-Za-z0-9_-]"});
        }
    }

    // --log-level beats --verbose regardless of order: it is the more
    // specific request.
    if (parsed.has("verbose"))
    {
        result.log_filter = "debug";
    }
    if (auto lvl = parsed.value("log-level"))
    {
        if (is_valid_log_filter(*lvl))
        {
            result.log_filter = std::move(*lvl);
        }
        else
        {
            result.diagnostics.push_back(
                {Diagnostic::Kind::InvalidValue, "--log-level",
                 "expected error|warn|info|debug|trace or a filter directive"});
        }
    }

    // First recognised action wins; later ones are ignored.
    for (const auto& [id, value] : parsed.occurrences())
    {
        const LaunchAction action = launch_action_from_option_id(id);
        if (action == LaunchAction::None)
        {
            continue;
        }
        if (action == LaunchAction::Room && value.empty())
        {
            result.diagnostics.push_back(
                {Diagnostic::Kind::InvalidValue, "--open-room", "empty room ID"});
            continue;
        }
        if (result.action != LaunchAction::None)
        {
            continue;
        }
        result.action = action;
        if (action == LaunchAction::Room)
        {
            result.room_id = value;
        }
    }

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (auto dir = parsed.value("screenshot-dir"); dir && !dir->empty())
    {
        result.screenshot_dir = std::move(*dir);
    }
#endif

    // The first positional that is a recognised matrix link. Anything else
    // (e.g. a stray file argument from a launcher) is ignored silently, as
    // before.
    for (const auto& arg : parsed.positionals())
    {
        if (Client::parse_matrix_link(arg).kind !=
            Client::MatrixLink::Kind::Unknown)
        {
            result.matrix_uri = arg;
            break;
        }
    }

    return result;
}

std::vector<std::string> profile_relaunch_args(std::string_view profile)
{
    if (profile.empty())
    {
        return {};
    }
    return {"--profile=" + std::string(profile)};
}

} // namespace tesseract
