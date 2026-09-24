#pragma once

#include <tesseract/launch_args.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shared process-startup pipeline for every native shell. Each shell's
// main()/delegate collects its native argv as UTF-8 and calls
// prepare_launch(); everything that is not genuinely platform-specific
// (profile selection, settings/crash-handler/locale bootstrap, --help,
// --version, --logoutall, diagnostics, the SDK log override) happens here,
// in the same order on every platform.
namespace tesseract
{

struct LaunchHooks
{
    /// Locale name to use when Settings::language is "auto" (e.g. "es_MX").
    std::function<std::string()> detect_system_lang;
    /// Directory holding tesseract.<lang>.mo catalogs.
    std::function<std::string()> i18n_dir;
    /// Console output. Default: stdout / stderr. Win32 overrides these to
    /// attach to the parent console first (it is a GUI-subsystem binary).
    std::function<void(std::string_view)> write_stdout;
    std::function<void(std::string_view)> write_stderr;
    /// Take the active profile's single-instance lock (and keep holding it
    /// for the rest of the process). Returns false when another instance of
    /// the same profile is running. Used by --logoutall, which must not
    /// wipe stores a live instance has open.
    std::function<bool()> acquire_instance_lock;
    /// Program name shown in --help. Default "tesseract".
    std::string program_name = "tesseract";
};

struct LaunchPlan
{
    LaunchArgs args;
    /// Set when the process should exit right away with this status
    /// (--help, --version, --logoutall).
    std::optional<int> exit_code;

    /// Whether the main window should start hidden in the tray.
    bool start_hidden() const
    {
        return args.autostart || args.hidden;
    }

    /// Whether this launch carries something for an already-running
    /// instance to act on. A duplicate launch that has nothing to forward
    /// and asked to stay hidden exits quietly without raising the other
    /// instance.
    bool has_forwardable_intent() const
    {
        return args.matrix_uri.has_value() ||
               args.action != LaunchAction::None;
    }
    bool should_raise_existing_instance() const
    {
        return has_forwardable_intent() || !start_hidden();
    }
};

/// Run the shared startup steps, in order:
///   1. parse `args` (argv[1..], UTF-8);
///   2. select the --profile (before anything reads config_dir());
///   3. load Settings, install the crash handler, set the UI locale;
///   4. print parser warnings to stderr;
///   5. --version / --help: print and return exit_code = 0;
///   6. --logoutall: sign every account out and return its exit code;
///   7. apply --log-level / --verbose to the SDK logger.
/// Otherwise returns with exit_code unset and the shell continues its
/// normal startup.
LaunchPlan prepare_launch(const std::vector<std::string>& args,
                          const LaunchHooks& hooks);

/// The translated --help text.
std::string launch_help_text(std::string_view program_name);

} // namespace tesseract
