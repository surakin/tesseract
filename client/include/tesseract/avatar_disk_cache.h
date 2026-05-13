#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tesseract {

/// Filesystem L1 cache for avatar images (room + user).
///
/// Files live at `<dir>/<base64url(mxc_url)>.bin`. Each put is atomic
/// (write tmp, then rename) so a crash mid-write never leaves a partial file
/// visible to get. All methods silently ignore I/O errors — the L1 cache is
/// a best-effort layer; all callers fall back to the Matrix SDK L2 cache.
class AvatarDiskCache {
public:
    explicit AvatarDiskCache(std::filesystem::path dir);

    /// Returns cached bytes for `mxc_url`, or empty on miss / I/O error.
    std::vector<uint8_t> get(const std::string& mxc_url) const;

    /// Atomically stores `bytes` for `mxc_url`. Silently ignores errors.
    void put(const std::string& mxc_url,
             std::span<const uint8_t> bytes) const;

    /// Returns all mxc URLs present in the cache directory.
    std::vector<std::string> all_keys() const;

    /// Removes the entry for `mxc_url`. No-op on miss. Silently ignores errors.
    void remove(const std::string& mxc_url) const;

    /// Removes all cached entries. Silently ignores errors.
    void clear() const;

private:
    std::filesystem::path dir_;

    static std::string encode(const std::string& s);
    static std::string decode(const std::string& stem);

    std::filesystem::path path_for(const std::string& mxc_url) const;
};

} // namespace tesseract
