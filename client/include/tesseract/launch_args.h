#pragma once

#include "tesseract/cli.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract
{

enum class LaunchAction
{
    None,
    QuickSwitcher,
    MessageSearch,
    Settings,
    Room,
};

/// Result of parsing a shell's argv into the flags Tesseract understands.
/// Shared, platform-agnostic parsing logic built on tesseract::cli — see
/// parse_launch_args() and launch_option_specs() below.
struct LaunchArgs
{
    /// True when `--autostart` is present, i.e. the OS launched the app via
    /// its login-item mechanism rather than the user opening it directly.
    bool autostart = false;

    /// `--hidden` / `--minimized`: start in the tray without a window. Unlike
    /// autostart this is a user request, so a duplicate launch still forwards
    /// any URI/action it carries.
    bool hidden = false;

    /// `-h` / `--help` and `-V` / `--version`: print and exit.
    bool help = false;
    bool version = false;

    /// `--logoutall`: sign out every stored account and exit without UI.
    bool logout_all = false;

    /// `--profile=NAME`: run against an isolated set of config/data/cache
    /// directories. Validated as [A-Za-z0-9_-]{1,32}; invalid names are
    /// dropped with an InvalidValue diagnostic.
    std::optional<std::string> profile;

    /// `--log-level=LEVEL|FILTER` or `-v` / `--verbose` (= "debug"). Either a
    /// bare level (error|warn|info|debug|trace) or a full tracing EnvFilter
    /// directive string (anything containing '=' or ',').
    std::optional<std::string> log_filter;

    /// Set when one of the arguments is a recognised matrix.to URL or
    /// `matrix:` URI (per Client::parse_matrix_link).
    std::optional<std::string> matrix_uri;

    /// Fixed app-wide action requested on the command line (Windows Jump
    /// List shortcuts use these). The first recognised action wins when
    /// conflicting switches are passed.
    LaunchAction action = LaunchAction::None;
    std::optional<std::string> room_id;

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    /// Destination directory for deterministic CI screenshots. This field and
    /// its parser branch do not exist in normal production builds.
    std::optional<std::string> screenshot_dir;
#endif

    /// Parser warnings (unknown options, bad values, ...). Never fatal —
    /// callers print them to stderr and carry on.
    std::vector<cli::Diagnostic> diagnostics;
};

/// Supplies the translated help line for an option id ("help", "profile",
/// ...). Kept as a callback so the translatable strings live in ui/shared
/// (where the i18n extractor looks) rather than in this library.
using LaunchHelpFn = std::function<std::string(std::string_view id)>;

/// The option table Tesseract accepts. With no `help` callback the help
/// strings are left empty (enough for parsing).
std::vector<cli::OptionSpec> launch_option_specs(const LaunchHelpFn& help = {});

/// Toolkit/OS arguments every shell may legitimately receive and should skip
/// silently: Qt's own switches (-platform, -style, ...), AppKit user-default
/// overrides (-NS..., -Apple...), and the legacy Carbon -psn_ serial.
std::span<const cli::ForeignOption> launch_foreign_options();

/// Parse command-line arguments (argv[1..], i.e. excluding argv[0]) into a
/// LaunchArgs. Order-independent: `--autostart` and a matrix URI may appear
/// in either order or alone. Unrecognised arguments are reported in
/// `diagnostics` and otherwise ignored. Pure function — no OS calls, safe to
/// unit test directly.
LaunchArgs parse_launch_args(const std::vector<std::string>& args);

/// The option id ("open-settings", "open-room", ...) for `action`, and back.
/// Used to carry an action across the Linux activation socket. Empty /
/// LaunchAction::None for no or unknown action.
std::string_view launch_action_option_id(LaunchAction action);
LaunchAction launch_action_from_option_id(std::string_view id);

/// Build the argument list that relaunches into the same profile, for OS
/// integration points that store a command line (Jump List tasks,
/// autostart entries). Empty when `profile` is empty.
std::vector<std::string> profile_relaunch_args(std::string_view profile);

} // namespace tesseract
