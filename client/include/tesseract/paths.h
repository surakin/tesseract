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

} // namespace tesseract
