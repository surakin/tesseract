#pragma once

#include "tk/host.h" // tk::EncodedImage

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract
{

// Host::encode_for_send, injected so the policy below is testable without a
// platform Host.
using EncodeForSendFn =
    std::function<tk::EncodedImage(const std::uint8_t* data, std::size_t len, bool compress)>;

// image_metadata_strippable(), injected for tests.
using StrippableFn =
    std::function<bool(std::span<const std::uint8_t> bytes, std::string_view mime)>;

struct PreparedImage
{
    std::vector<std::uint8_t> bytes; // empty only when the input was empty
    std::string mime;
    std::string filename;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool is_animated = false; // MSC4230 flag for the send call
    // True when the image must not be sent: its metadata cannot be stripped
    // and it can neither be recompressed (the host cannot decode it) nor, for
    // an animation, flattened.
    bool refused = false;
};

// Decides what actually gets uploaded for an attached image:
//   * animated (the composer's flag OR is_animated_image(): GIF, WebP, APNG,
//     AVIF, JXL) -> the original bytes untouched, is_animated = true, provided
//     the SDK can strip them; an animation it cannot strip is `refused` (it
//     cannot be recompressed without flattening it);
//   * a still whose metadata cannot be stripped (no stripper for the format,
//     or it fails to parse) -> always the host's recompressed re-encode, even
//     with recompression switched off or when it is larger, because the
//     original would leak EXIF/GPS; if the host cannot decode it either the
//     image is `refused`;
//   * otherwise recompress only when `recompress` is on, and keep the original
//     when the result is not smaller (the SDK strips originals on upload).
// When the host cannot decode a strippable image the original is sent with the
// caller-supplied mime/size, so an image the platform has no codec for (e.g.
// JXL on Windows) still goes out intact.
PreparedImage prepare_image_for_send(std::vector<std::uint8_t> bytes, std::string mime,
                                     std::string filename, std::uint32_t width,
                                     std::uint32_t height, bool is_animated_flag, bool recompress,
                                     const EncodeForSendFn& encode, const StrippableFn& strippable);

} // namespace tesseract
