#pragma once

#include <optional>
#include <string>
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
/// Shared, platform-agnostic parsing logic — see parse_launch_args() below.
/// macOS normally receives launch events through AppKit, but screenshot-only
/// builds also use this parser for the deterministic capture argument.
struct LaunchArgs
{
    /// True when `--autostart` is present, i.e. the OS launched the app via
    /// its login-item mechanism rather than the user opening it directly.
    bool autostart = false;

    /// Set when one of the arguments is a recognised matrix.to URL or
    /// `matrix:` URI (per Client::parse_matrix_link).
    std::optional<std::string> matrix_uri;

    /// Fixed app-wide action requested by a Windows Jump List shortcut.
    /// The first recognised action wins when conflicting switches are passed.
    LaunchAction action = LaunchAction::None;
    std::optional<std::string> room_id;

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    /// Destination directory for deterministic CI screenshots. This field and
    /// its parser branch do not exist in normal production builds.
    std::optional<std::string> screenshot_dir;
#endif
};

/// Parse command-line arguments (argv[1..], i.e. excluding argv[0]) into a
/// LaunchArgs. Order-independent: `--autostart` and a matrix URI may appear
/// in either order or alone. Unrecognised arguments are ignored. Pure
/// function — no OS calls, safe to unit test directly.
LaunchArgs parse_launch_args(const std::vector<std::string>& args);

} // namespace tesseract
