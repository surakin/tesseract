#pragma once
// Off-thread video frame extraction for the GIF strip.
//
// decode_video_frames() is safe to call from any thread. It returns raw
// BGRA pixel data; the caller converts to platform Image objects on
// whichever thread is appropriate (worker thread for Qt6/GTK4/macOS,
// UI thread for Win32 where the D2D backend must be present).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tk
{

struct VideoFrame
{
    std::vector<std::uint8_t> bgra; // BGRA, row-major, stride = w * 4
    int w = 0;
    int h = 0;
    int delay_ms = 33; // per-frame display time; ≥1
};

// Box-average downscale of a BGRA frame to fit within (max_w, max_h) while
// preserving aspect ratio. Returns `src` unchanged when it already fits or the
// caps are non-positive. Used by the decoders to keep GIF-strip frames at the
// preview cell size so they stay within the animation cache's byte budget
// instead of holding full-resolution video frames.
inline VideoFrame downscale_bgra(VideoFrame src, int max_w, int max_h)
{
    if (max_w <= 0 || max_h <= 0 || src.w <= 0 || src.h <= 0)
        return src;
    if (src.w <= max_w && src.h <= max_h)
        return src;
    const double s =
        std::min(double(max_w) / src.w, double(max_h) / src.h);
    const int tw = std::max(1, static_cast<int>(src.w * s));
    const int th = std::max(1, static_cast<int>(src.h * s));
    VideoFrame out;
    out.w = tw;
    out.h = th;
    out.delay_ms = src.delay_ms;
    out.bgra.resize(static_cast<std::size_t>(tw) * th * 4);
    for (int ty = 0; ty < th; ++ty)
    {
        const int sy0 = static_cast<int>(std::int64_t(ty) * src.h / th);
        const int sy1 =
            std::max(sy0 + 1, static_cast<int>(std::int64_t(ty + 1) * src.h / th));
        for (int tx = 0; tx < tw; ++tx)
        {
            const int sx0 = static_cast<int>(std::int64_t(tx) * src.w / tw);
            const int sx1 = std::max(
                sx0 + 1, static_cast<int>(std::int64_t(tx + 1) * src.w / tw));
            std::uint32_t b = 0, g = 0, r = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
            {
                const std::uint8_t* p =
                    src.bgra.data() +
                    (static_cast<std::size_t>(sy) * src.w + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx)
                {
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    a += p[3];
                    p += 4;
                    ++n;
                }
            }
            std::uint8_t* d =
                out.bgra.data() + (static_cast<std::size_t>(ty) * tw + tx) * 4;
            d[0] = static_cast<std::uint8_t>(b / n);
            d[1] = static_cast<std::uint8_t>(g / n);
            d[2] = static_cast<std::uint8_t>(r / n);
            d[3] = static_cast<std::uint8_t>(a / n);
        }
    }
    return out;
}

struct DecodedVideoFrames
{
    std::vector<VideoFrame> frames; // empty on failure or zero-frame file
};

// Decode all video frames from the raw bytes of an MP4 (or other container).
// Returns empty frames on format error or decode failure.
// max_w / max_h: soft dimension cap — the pipeline scales down to fit while
// preserving aspect ratio where the backend supports it. A value of 0 means
// no limit.
DecodedVideoFrames decode_video_frames(const std::uint8_t* data,
                                       std::size_t size,
                                       int max_w, int max_h);

} // namespace tk
