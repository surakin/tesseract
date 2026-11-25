#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tesseract
{

using Sha256Digest = std::array<std::uint8_t, 32>;

/// SHA-256 of `size` bytes at `data` (computed by the Rust SDK). `data` may be
/// null when `size` is 0.
Sha256Digest sha256(const void* data, std::size_t size);

/// Lower-case hex of a digest (64 characters).
std::string to_hex(const Sha256Digest& d);

} // namespace tesseract
