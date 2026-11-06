#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tk
{

// Cheap header sniff (no decode): true when `bytes` is a GIF (even a
// single-frame one — only the decoder can tell), a WebP whose VP8X header
// has the animation flag set, or an APNG (an `acTL` chunk ahead of the first
// `IDAT`), or a JPEG XL file (animation lives in the codestream header and
// only the decoder can tell, so every JXL is treated as possibly animated).
// False for anything else, including truncated input.
bool bytes_may_be_animated(std::span<const std::uint8_t> bytes);

// True when `bytes` start with a JPEG XL signature: a bare codestream
// (`FF 0A`) or an ISOBMFF container (`JXL ` signature box). Pure header sniff.
bool is_jxl_bytes(std::span<const std::uint8_t> bytes);

// The MIME type of a PNG, JPEG, GIF or WebP file, from its signature. Empty for
// anything else (JPEG XL included), so a caller can refuse the file rather than
// label it with the wrong type.
std::string_view sniff_image_mime(std::span<const std::uint8_t> bytes);

struct ImageDims
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Width/height read from the header of a PNG, GIF, JPEG, WebP or JPEG XL (VP8, VP8L,
// or the VP8X canvas) without decoding. nullopt for any other format or for
// input truncated before the size fields.
std::optional<ImageDims> probe_image_dimensions(std::span<const std::uint8_t> bytes);

// Pixel budget for decoding untrusted bytes. Stills: 100 MP and 32768 px per
// side (~400 MB RGBA). Animations keep every frame plus compositing buffers,
// so they get 16 MP (4096x4096).
inline constexpr std::uint64_t kMaxStillDecodePixels = 100'000'000;
inline constexpr std::uint32_t kMaxDecodeSide = 32768;
// JPEG stills may be larger only where the decoder scales in the DCT domain,
// so memory is bounded by the output size: the Qt and GTK shells on Linux.
// Windows (WIC) and macOS (ImageIO) decode a JPEG at full size, so there a
// 268 MP file would cost 1-3 GB per decode and gets the still budget instead.
#if defined(_WIN32) || defined(__APPLE__)
inline constexpr std::uint64_t kMaxJpegStillDecodePixels = kMaxStillDecodePixels;
inline constexpr std::uint32_t kMaxJpegDecodeSide = kMaxDecodeSide;
#else
inline constexpr std::uint64_t kMaxJpegStillDecodePixels =
    1ull << 28; // 268 MP: scaled in the DCT domain
inline constexpr std::uint32_t kMaxJpegDecodeSide = 65535;
#endif
inline constexpr std::uint64_t kMaxAnimatedDecodePixels = 4096ull * 4096ull;

// False only when the header is recognised and over budget. Unrecognised
// formats pass: the WIC and ImageIO backends then check the decoder-reported
// size with decode_dims_allowed, GTK caps it in size-prepared, and Qt relies on
// QImageReader's allocation limit.
bool decode_size_allowed(std::span<const std::uint8_t> bytes, bool animated);

// Budget check for a size a platform decoder (WIC, ImageIO) reported for a
// format probe_image_dimensions cannot read. Applies kMaxDecodeSide and
// kMaxStillDecodePixels (kMaxAnimatedDecodePixels when animated); on Windows
// and macOS that equals the JPEG budget. A zero width or height is refused.
bool decode_dims_allowed(std::uint64_t width, std::uint64_t height, bool animated);

} // namespace tk
