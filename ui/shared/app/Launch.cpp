#include "app/Launch.h"

#include "tk/i18n.h"

#include <tesseract/client.h>
#include <tesseract/crash_handler.h>
#include <tesseract/maintenance.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/version.h>

#include <cstdio>

namespace tesseract
{

namespace
{

std::string g_launch_language;

std::string help_for(std::string_view id)
{
    if (id == "help")
    {
        return tk::tr("Show this help and exit");
    }
    if (id == "version")
    {
        return tk::tr("Show the version and exit");
    }
    if (id == "profile")
    {
        return tk::tr("Use a separate profile with its own accounts and settings");
    }
    if (id == "hidden")
    {
        return tk::tr("Start in the tray without showing the window");
    }
    if (id == "log-level")
    {
        return tk::tr("Set the log level (error, warn, info, debug, trace)");
    }
    if (id == "verbose")
    {
        return tk::tr("Log more detail (same as --log-level=debug)");
    }
    if (id == "open-room")
    {
        return tk::tr("Open the room with this ID");
    }
    if (id == "open-quick-switcher")
    {
        return tk::tr("Open the quick switcher");
    }
    if (id == "open-message-search")
    {
        return tk::tr("Open message search");
    }
    if (id == "open-settings")
    {
        return tk::tr("Open Settings");
    }
    if (id == "logoutall")
    {
        return tk::tr("Sign out of every account and exit");
    }
    return {};
}

void write_or_default(const std::function<void(std::string_view)>& fn,
                      std::FILE* fallback, std::string_view text)
{
    if (fn)
    {
        fn(text);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), fallback);
    std::fflush(fallback);
}

int run_logout_all(const LaunchHooks& hooks)
{
    auto err = [&hooks](const std::string& line)
    { write_or_default(hooks.write_stderr, stderr, line + "\n"); };

    if (hooks.acquire_instance_lock && !hooks.acquire_instance_lock())
    {
        err(tk::tr("Tesseract is running. Quit it first, then try again."));
        return 1;
    }

    const LogoutAllReport report = logout_all_accounts();
    if (report.index_corrupt)
    {
        err(tk::tr("The account list could not be read. Nothing was changed."));
        return 1;
    }
    if (report.accounts.empty())
    {
        err(tk::tr("No accounts to sign out."));
        return 0;
    }

    int status = 0;
    for (const auto& a : report.accounts)
    {
        if (a.server_ok)
        {
            err(tk::trf(tk::tr("Signed out {0}"), {a.user_id}));
        }
        else
        {
            // The local copy is gone either way; the device may still be
            // listed on the homeserver.
            err(tk::trf(tk::tr("Removed {0} from this device, but the server "
                               "sign-out failed: {1}"),
                        {a.user_id, a.error}));
            status = 3;
        }
    }
    return status;
}

} // namespace

std::string launch_help_text(std::string_view program_name)
{
    const auto specs = launch_option_specs(help_for);
    return cli::format_usage(specs, program_name,
                             tk::tr("[OPTIONS] [MATRIX-URI]"),
                             tk::tr("Usage:"), tk::tr("Options:"));
}

LaunchPlan prepare_launch(const std::vector<std::string>& args,
                          const LaunchHooks& hooks)
{
    LaunchPlan plan;
    plan.args = parse_launch_args(args);

    // Must precede every config_dir()/data_dir()/cache_dir() read below.
    if (plan.args.profile)
    {
        set_profile(*plan.args.profile);
    }

    // Load persisted settings before set_locale so the saved language
    // preference is available when choosing the locale.
    Settings::instance().load_from_disk(config_dir());
    install_crash_handler(Settings::instance().crash_reporting_enabled);
    {
        std::string lang = Settings::instance().language;
        g_launch_language = lang;
        if ((lang == "auto" || lang.empty()) && hooks.detect_system_lang)
        {
            lang = hooks.detect_system_lang();
        }
        tk::set_locale(hooks.i18n_dir ? hooks.i18n_dir() : std::string{}, lang);
    }

    for (const auto& d : plan.args.diagnostics)
    {
        write_or_default(hooks.write_stderr, stderr,
                         "tesseract: warning: " + cli::format_diagnostic(d) + "\n");
    }

    if (plan.args.version)
    {
        write_or_default(hooks.write_stdout, stdout,
                         std::string(kAppName) + " " + kVersion + "\n");
        plan.exit_code = 0;
        return plan;
    }
    if (plan.args.help)
    {
        write_or_default(hooks.write_stdout, stdout,
                         launch_help_text(hooks.program_name));
        plan.exit_code = 0;
        return plan;
    }

    // Before --logoutall so --verbose also covers its SDK traffic.
    if (plan.args.log_filter)
    {
        set_log_filter_override(*plan.args.log_filter);
    }

    if (plan.args.logout_all)
    {
        plan.exit_code = run_logout_all(hooks);
        return plan;
    }
    return plan;
}

const std::string& launch_language()
{
    return g_launch_language;
}

std::vector<std::string> relaunch_args()
{
    auto args = profile_relaunch_args(profile());
    args.emplace_back("--relaunch");
    return args;
}

} // namespace tesseract
