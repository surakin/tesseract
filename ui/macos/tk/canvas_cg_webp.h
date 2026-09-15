#pragma once

// libwebp-backed animated WebP decode for macOS — see canvas_cg_webp.cpp's
// file-level doc comment for the full story. In short: ImageIO's public
// frame-extraction APIs (CGImageSourceCreateImageAtIndex,
// CreateThumbnailAtIndex, CGAnimateImageDataWithBlock) were all measured
// this session to realize a given frame's pixels at a cost that scales with
// that frame's true position in the animation, regardless of decode order
// or which API triggers the realization — confirmed via a reverse-decode-
// order test and independently documented by Discord's own engineering
// writeup on the identical bug. libwebp's WebPAnimDecoder keeps real
// persistent compositing state across sequential frame requests, the one
// capability ImageIO never exposes publicly, matching how GTK4/Qt6/Windows'
// own native decoders already avoid this. GIF/APNG/still images are
// untouched by this file — canvas_cg.cpp only ever calls into here once
// is_webp_data() has confirmed the content is WebP.

#include "canvas_cg.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace tk::cg
{

// Signature check only (WebPGetInfo) — doesn't touch ImageIO or require a
// CGImageSourceRef to have parsed anything. True iff `bytes` is a valid
// WebP (RIFF/WEBP) file.
bool is_webp_data(std::span<const std::uint8_t> bytes);

// libwebp-backed equivalent of decode_image_bytes (canvas_cg.h) — identical
// contract, including the streaming on_first_frame/on_extra_frame mode. Only
// called once the caller has confirmed `bytes` is WebP via is_webp_data().
DecodedFrames decode_webp_bytes_libwebp(
    std::span<const std::uint8_t> bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<Image>, int)>* on_first_frame =
        nullptr,
    const std::function<void(int, std::unique_ptr<Image>, int)>*
        on_extra_frame = nullptr);

// libwebp-backed equivalent of decode_image_bytes_windowed (canvas_cg.h) —
// identical contract.
DecodedFrames decode_webp_bytes_windowed_libwebp(
    std::span<const std::uint8_t> bytes, int max_w, int max_h,
    int window_threshold_frames,
    const std::function<void(std::unique_ptr<Image>, int delay_ms,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t total_frames)>& on_first_frame,
    const std::function<void(int frame_index, std::unique_ptr<Image>,
                             int delay_ms)>& on_extra_frame);

} // namespace tk::cg
