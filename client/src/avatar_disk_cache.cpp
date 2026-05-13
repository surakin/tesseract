#include "tesseract/avatar_disk_cache.h"

#include <array>
#include <fstream>
#include <system_error>

namespace tesseract {

namespace {

// base64url alphabet (RFC 4648 §5): no padding, uses - and _ instead of + and /
constexpr char kAlpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

} // namespace

// static
std::string AvatarDiskCache::encode(const std::string& s) {
    std::string out;
    out.reserve((s.size() * 4 + 2) / 3);
    for (std::size_t i = 0; i < s.size(); i += 3) {
        uint32_t b = static_cast<uint8_t>(s[i]) << 16;
        if (i + 1 < s.size()) b |= static_cast<uint8_t>(s[i + 1]) << 8;
        if (i + 2 < s.size()) b |= static_cast<uint8_t>(s[i + 2]);
        out += kAlpha[(b >> 18) & 63];
        out += kAlpha[(b >> 12) & 63];
        if (i + 1 < s.size()) out += kAlpha[(b >>  6) & 63];
        if (i + 2 < s.size()) out += kAlpha[b & 63];
    }
    return out;
}

// static
std::string AvatarDiskCache::decode(const std::string& stem) {
    // Build reverse lookup: ASCII value → 6-bit index, 0xFF for invalid.
    static constexpr auto make_table = []() {
        std::array<uint8_t, 128> t{};
        t.fill(0xFF);
        for (int i = 0; i < 64; ++i)
            t[static_cast<uint8_t>(kAlpha[i])] = static_cast<uint8_t>(i);
        return t;
    };
    static constexpr auto kTable = make_table();

    std::string out;
    out.reserve(stem.size() * 3 / 4);
    uint32_t buf  = 0;
    int      bits = 0;
    for (unsigned char c : stem) {
        if (c >= 128) return {};
        uint8_t v = kTable[c];
        if (v == 0xFF) return {};
        buf  = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

AvatarDiskCache::AvatarDiskCache(std::filesystem::path dir)
    : dir_(std::move(dir))
{
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
}

std::filesystem::path AvatarDiskCache::path_for(
        const std::string& mxc_url) const
{
    return dir_ / (encode(mxc_url) + ".bin");
}

std::vector<uint8_t> AvatarDiskCache::get(const std::string& mxc_url) const {
    if (mxc_url.empty()) return {};
    std::ifstream f(path_for(mxc_url), std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

void AvatarDiskCache::put(const std::string& mxc_url,
                          std::span<const uint8_t> bytes) const
{
    if (mxc_url.empty() || bytes.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return;

    auto final_path = path_for(mxc_url);
    auto tmp_path   = std::filesystem::path(final_path).replace_extension(".tmp");

    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            std::filesystem::remove(tmp_path, ec);
            return;
        }
    }
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) std::filesystem::remove(tmp_path, ec);
}

std::vector<std::string> AvatarDiskCache::all_keys() const {
    std::vector<std::string> keys;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir_, ec))
    {
        if (ec) break;
        const auto& p = entry.path();
        if (p.extension() != ".bin") continue;
        auto url = decode(p.stem().string());
        if (!url.empty())
            keys.push_back(std::move(url));
    }
    return keys;
}

void AvatarDiskCache::remove(const std::string& mxc_url) const {
    if (mxc_url.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path_for(mxc_url), ec);
}

void AvatarDiskCache::clear() const {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir_, ec))
    {
        if (ec) break;
        if (entry.path().extension() == ".bin" ||
            entry.path().extension() == ".tmp")
        {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

} // namespace tesseract
