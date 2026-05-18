#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tk
{

/// Decode a BlurHash string (MSC2448 / xyz.amorgan.blurhash) to a flat
/// RGBA8888 pixel buffer (stride = w * 4). Returns false when `hash` is
/// empty or malformed.
bool decode_blurhash(const std::string& hash, int w, int h,
                     std::vector<uint8_t>& out_rgba);

} // namespace tk
