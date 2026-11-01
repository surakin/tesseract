#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tesseract
{

/// Private scratch directory for attachments opened in an external app:
/// `<cache_dir()>/open-media`. Not created by this call.
std::filesystem::path open_media_dir();

/// Writes `bytes` to `<open_media_dir()>/<random>/<filename>` (a fresh
/// subdirectory per call so the sender's exact file name survives without
/// collisions) and returns the path. The directories are created 0700 and the
/// file 0600 (POSIX). `filename` must already be sanitized — a name containing
/// a path separator is refused. Returns nullopt on any I/O error.
std::optional<std::filesystem::path> write_open_media_file(const std::string& filename,
                                                           const std::vector<std::uint8_t>& bytes);

/// Deletes everything under `open_media_dir()`. Best-effort: files an external
/// app still holds open (Windows) are skipped and swept on the next call.
void purge_open_media_dir();

/// Hands `path` to the OS default application (ShellExecute / `open` /
/// `xdg-open`). Refuses anything that is not a regular, non-symlink file
/// inside `open_media_dir()`. True only when the launch was dispatched.
bool open_local_file(const std::filesystem::path& path);

} // namespace tesseract
