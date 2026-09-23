#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace tesseract
{

/// True when `name` is usable as a `--profile` name: 1-32 characters from
/// [A-Za-z0-9_-]. Restrictive on purpose — the name is spliced into
/// directory names, lock/socket paths, Win32 object names, D-Bus app ids and
/// secret-store keys.
bool is_valid_profile_name(std::string_view name);

/// Select a named profile for the rest of the process. Must be called before
/// anything reads config_dir()/data_dir()/cache_dir() (i.e. first thing in
/// main). An empty name selects the default profile; an invalid name is
/// ignored. Not thread-safe — startup only.
void set_profile(std::string_view name);

/// The active profile name, empty for the default profile.
const std::string& profile();

/// "" for the default profile, "-<name>" otherwise. Appended to the app
/// folder names below and to per-instance OS identifiers (single-instance
/// lock, Win32 mutex/window class, D-Bus app id, secret-store service).
std::string profile_suffix();

/// Per-user Tesseract config directory.
/// - Windows: `%APPDATA%/Tesseract`
/// - macOS:   `~/Library/Application Support/Tesseract`
/// - Linux:   `$XDG_CONFIG_HOME/tesseract` (or `~/.config/tesseract`)
///
/// A named profile appends `-<name>` to the final folder
/// (e.g. `~/.config/tesseract-work`).
///
/// The directory is *not* created by this call. Callers that write into it
/// should `std::filesystem::create_directories(config_dir())` first.
std::filesystem::path config_dir();

/// Per-user Tesseract data directory — account/session state and the
/// matrix-sdk store live here, not in `config_dir()`.
/// - Windows: `%APPDATA%/Tesseract` (same as `config_dir()`)
/// - macOS:   `~/Library/Application Support/Tesseract` (same as `config_dir()`)
/// - Linux:   `$XDG_DATA_HOME/tesseract` (or `~/.local/share/tesseract`)
///
/// Only Linux distinguishes data from config; on Windows/macOS this returns
/// `config_dir()`. The directory is *not* created by this call.
std::filesystem::path data_dir();

/// Per-user Tesseract cache directory (expendable, not backed up).
/// - Windows: `%LOCALAPPDATA%/Tesseract`
/// - macOS:   `~/Library/Caches/Tesseract`
/// - Linux:   `$XDG_CACHE_HOME/tesseract` (or `~/.cache/tesseract`)
///
/// The directory is *not* created by this call.
std::filesystem::path cache_dir();

} // namespace tesseract
