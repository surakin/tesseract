#pragma once
#include <filesystem>

namespace tesseract
{

/// Per-user Tesseract config directory.
/// - Windows: `%APPDATA%/Tesseract`
/// - macOS:   `~/Library/Application Support/Tesseract`
/// - Linux:   `$XDG_CONFIG_HOME/tesseract` (or `~/.config/tesseract`)
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
