#pragma once

// Shared JPEG XL decode glue for the four shells. Every shell's decode
// virtuals (decode_image_, decode_image_streamed_, decode_image_streamed_windowed_)
// and CanvasFactory::decode_image / decode_animated_image start with
// `if (tesseract::is_jxl_data(bytes)) return tesseract::jxl::...(...)`; the
// decoding itself (Rust jxl-oxide, tesseract/jxl.h) and the animation session
// live here once instead of per platform.
//
// All functions are worker-thread safe provided the supplied MakeImage is.

#include "MediaTypes.h"

#include <tesseract/jxl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace tesseract::jxl
{

// Builds a tk::Image from straight RGBA8 (stride = w * 4). The pixels are only
// valid for the duration of the call, so the callee must copy them. Shells bind
// this to their own worker-thread-safe image construction (CanvasFactory's
// create_image_rgba(), QImage(...).copy(), a CGImage over a copied buffer, ...).
using MakeImage = std::function<std::unique_ptr<tk::Image>(const std::uint8_t* rgba, int w, int h)>;

// Whole-image decode: a still fills `still`; an animation (>= 2 frames) fills
// `frames` / `delays_ms` (capped at tk::kAnimDecodeMaxFrames, delays
// normalised). Empty for non-JXL, invalid, or over-budget input.
DecodedImage decode_image(std::span<const std::uint8_t> bytes, int max_w, int max_h,
                          const MakeImage& make_image);

// Streaming decode matching ShellBase::decode_image_streamed_: false for
// non-JXL / invalid / single-frame input (the caller then takes its still
// path); otherwise frame 0 goes to on_first_frame and the rest to on_frame.
bool decode_streamed(std::span<const std::uint8_t> bytes, int max_w, int max_h,
                     const MakeImage& make_image,
                     const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
                     const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame);

// Windowed decode matching ShellBase::decode_image_streamed_windowed_: same
// contract as decode_streamed, except an animation longer than
// tk::kAnimDecodeWindowThresholdFrames hands on_first_frame a live
// tk::AnimDecodeSession plus the total frame count and delivers only the first
// tk::kAnimDecodeInitialBatchFrames frames; shorter ones decode whole with
// (nullptr, 0).
bool decode_streamed_windowed(
    std::span<const std::uint8_t> bytes, int max_w, int max_h, const MakeImage& make_image,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>, std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame);

// CanvasFactory::decode_image for JXL bytes: the still, or frame 0 of an
// animation, at native size (bounded by tk::kMaxDecodeSide). nullptr when
// invalid / over budget / the factory has no create_image_rgba.
std::unique_ptr<tk::Image> decode_still(tk::CanvasFactory& factory,
                                        std::span<const std::uint8_t> bytes);

// CanvasFactory::decode_animated_image for JXL bytes: every frame (capped)
// pre-scaled to fit max_px x max_px; nullptr for a still image or on failure.
std::unique_ptr<tk::AnimatedImage> decode_animated(tk::CanvasFactory& factory,
                                                   std::span<const std::uint8_t> bytes, int max_px);

} // namespace tesseract::jxl
