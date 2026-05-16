#pragma once
#include <filesystem>

namespace tesseract {

/// Per-user Tesseract config directory.
/// - Windows: `%APPDATA%/Tesseract`
/// - macOS:   `~/Library/Application Support/Tesseract`
/// - Linux:   `$XDG_CONFIG_HOME/tesseract` (or `~/.config/tesseract`)
///
/// The directory is *not* created by this call. Callers that write into it
/// should `std::filesystem::create_directories(config_dir())` first.
std::filesystem::path config_dir();

/// Per-user Tesseract cache directory (expendable, not backed up).
/// - Windows: `%LOCALAPPDATA%/Tesseract`
/// - macOS:   `~/Library/Caches/Tesseract`
/// - Linux:   `$XDG_CACHE_HOME/tesseract` (or `~/.cache/tesseract`)
///
/// The directory is *not* created by this call.
std::filesystem::path cache_dir();

} // namespace tesseract
