#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace tesseract
{

// True when `bytes` are an image in a format that can animate AND actually
// contain more than one frame: GIF (counted image blocks), WebP (VP8X
// animation flag), APNG (`acTL` with num_frames > 1), AVIF (`avis` brand) and
// JPEG XL (header animation field, via the Rust decoder). A single-frame GIF,
// a still WebP/PNG/AVIF/JXL, JPEG and unrecognised data are all false.
//
// Magic bytes decide the format; `mime` (may be empty) is only a pre-filter so
// a clearly non-image mime (video/, audio/, text/, ...) short-circuits to false.
// Header scans only: no frame is decoded. Safe on worker threads.
bool is_animated_image(std::span<const std::uint8_t> bytes, std::string_view mime = {});

} // namespace tesseract
