#include "canvas_cg_webp.h"

// libwebp-backed animated WebP decode.
//
// Why this exists: ImageIO's public frame-extraction entry points all hand
// back frame N's pixels at a cost that scales with N, regardless of which
// call triggers the realization or what order frames are requested in —
// measured directly this session across three different APIs
// (CGImageSourceCreateThumbnailAtIndex, CreateImageAtIndex + a forced
// offscreen redraw, CGAnimateImageDataWithBlock), and confirmed not to be an
// artifact of decode order via a reverse-decode-order test: frame 79 of an
// 80-frame animation was equally expensive whether decoded first or last.
// Discord's own engineering writeup documents the identical symptom
// ("animated WebP files would play progressively slower over time") for the
// identical reason and reached the same conclusion after their own
// profiling: bypass ImageIO for WebP, use libwebp's WebPAnimDecoder, whose
// internal canvas persists across sequential GetNext() calls — the one
// capability ImageIO's CGImageSourceRef never exposes publicly, and exactly
// what GTK4 (gdk-pixbuf), Qt6 (QImageReader), and Windows (WIC) already have
// for their own native decoders, which is why none of them have this bug.
//
// Scope: WebP only. GIF/APNG are not confirmed to have this problem (no
// report found either way) and keep using the existing, working ImageIO
// path in canvas_cg.cpp unchanged — only content is_webp_data() identifies
// as WebP ever reaches this file.

#include <tesseract/webp_anim.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <vector>

namespace tk::cg
{

namespace
{

// Minimal local RAII for the one CF type this file touches. Deliberately not
// named the same as canvas_cg.cpp's own (larger, more general) CFRetained<T>
// — under a unity build the two files land in the same translation unit, and
// C++ anonymous namespaces are per-TU, not per-`namespace {}` block, so an
// identically-named local helper in each file collides at link... actually
// compile time (confirmed: this exact collision was hit and is why this one
// is named differently). This is generic boilerplate, not decode logic, so
// unlike scale_cgimage/make_image there's no "one implementation" concern in
// duplicating it — just a naming one.
template <class T>
struct WebPCFRetained
{
    T ref = nullptr;
    WebPCFRetained() = default;
    explicit WebPCFRetained(T r) : ref(r)
    {
    }
    ~WebPCFRetained()
    {
        if (ref)
        {
            CFRelease(ref);
        }
    }
    WebPCFRetained(const WebPCFRetained&) = delete;
    WebPCFRetained& operator=(const WebPCFRetained&) = delete;
    T get() const
    {
        return ref;
    }
};

// Wrap one decoded frame buffer (premultiplied BGRA, native byte order — the
// decoder is configured for MODE_bgrA in sdk/src/webp_anim.rs) into an owned (+1
// retained, Create-rule) CGImageRef. `pixels` is copied into a fresh
// CFDataRef — WebPAnimDecoderGetNext's buffer is only valid until the next
// GetNext call or decoder teardown, so it cannot be wrapped directly the way
// CGImageCreate's other callers in this codebase wrap already-persistent
// buffers.
//
// kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little is the same
// premultiplied-BGRA pairing already used (and working) elsewhere in this
// codebase for raw camera-capture BGRA buffers — see
// MainWindow::encode_selfie_jpeg_. Getting this pairing wrong
// would reproduce an R/B swap via a new mechanism, not one of ImageIO's
// historical ones — this is the first thing to check visually once built.
CGImageRef make_cgimage_from_bgra(const std::uint8_t* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0)
    {
        return nullptr;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    WebPCFRetained<CFDataRef> data{
        CFDataCreate(kCFAllocatorDefault, pixels,
                     static_cast<CFIndex>(stride * static_cast<std::size_t>(height)))};
    if (!data.get())
    {
        return nullptr;
    }
    WebPCFRetained<CGDataProviderRef> provider{CGDataProviderCreateWithCFData(data.get())};
    if (!provider.get())
    {
        return nullptr;
    }
    WebPCFRetained<CGColorSpaceRef> cs{CGColorSpaceCreateDeviceRGB()};
    return CGImageCreate(
        static_cast<std::size_t>(width), static_cast<std::size_t>(height), 8, 32, stride, cs.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little,
        provider.get(), nullptr, false, kCGRenderingIntentDefault);
}

// Wraps tesseract::WebpAnimDecoder (libwebp's WebPAnimDecoder, owned by the
// Rust SDK; it keeps its own copy of the compressed bytes, which a windowed
// session's decoder needs long after the caller's own buffer goes away —
// same reasoning as CGAnimSession owning its retained CGImageSourceRef in
// canvas_cg.cpp). Shared by both the whole-batch decode functions below and
// WebPAnimSession so there is exactly one place that knows how to pull a
// frame + delay out of the decoder and turn it into a CGImageRef.
class WebPDecodeHandle
{
public:
    explicit WebPDecodeHandle(std::span<const std::uint8_t> bytes)
        : decoder_(bytes), source_bytes_(bytes.size())
    {
    }

    bool valid() const
    {
        return decoder_.valid();
    }
    std::size_t total_frames() const
    {
        return decoder_.frame_count();
    }
    bool has_more_frames() const
    {
        return decoder_.has_more_frames();
    }

    // Decodes exactly one more frame from the decoder's own internal
    // sequential cursor. Returns nullptr (out_delay_ms left unchanged) once
    // exhausted or on a decode failure — the same "skip this frame" contract
    // canvas_cg.cpp's ImageIO path uses. Caller owns the returned CGImageRef.
    CGImageRef decode_next(int max_w, int max_h, int& out_delay_ms)
    {
        if (!has_more_frames())
        {
            return nullptr;
        }
        int timestamp_ms = 0;
        const std::uint8_t* buf = decoder_.next_frame(timestamp_ms);
        if (!buf)
        {
            return nullptr;
        }
        CGImageRef cg = make_cgimage_from_bgra(buf, static_cast<int>(decoder_.canvas_width()),
                                               static_cast<int>(decoder_.canvas_height()));
        if (!cg)
        {
            return nullptr;
        }
        // The timestamp is cumulative ms since the animation started, not
        // this frame's own duration.
        const int delay_ms = timestamp_ms - last_timestamp_ms_;
        last_timestamp_ms_ = timestamp_ms;
        out_delay_ms = tk::normalize_frame_delay_ms(delay_ms);
        if (CGImageRef scaled = scale_cgimage(cg, max_w, max_h))
        {
            CGImageRelease(cg);
            cg = scaled;
        }
        return cg;
    }

    void restart()
    {
        decoder_.reset();
        last_timestamp_ms_ = 0;
    }

    // Source copy plus libwebp's two internal full-canvas BGRA buffers
    // (current frame + previous-disposed).
    std::size_t memory_bytes() const
    {
        return source_bytes_ + 2u * static_cast<std::size_t>(decoder_.canvas_width()) *
                                   decoder_.canvas_height() * 4u;
    }

private:
    tesseract::WebpAnimDecoder decoder_;
    std::size_t source_bytes_;
    int last_timestamp_ms_ = 0;
};

// tk::AnimDecodeSession backed by one persistent WebPAnimDecoder — the WebP
// analogue of CGAnimSession in canvas_cg.cpp. Unlike CGAnimSession (whose
// underlying ImageIO source is genuinely random-access, just not fast),
// WebPAnimDecoder is a real forward-only sequential decoder, matching the
// GTK4/Qt6/Windows sessions' shape more closely than ImageIO's own.
class WebPAnimSession final : public tk::AnimDecodeSession
{
public:
    WebPAnimSession(std::span<const std::uint8_t> bytes, int max_w, int max_h)
        : handle_(bytes), max_w_(max_w), max_h_(max_h)
    {
    }

    std::size_t memory_bytes() const override
    {
        return handle_.memory_bytes();
    }

    int decode_next_batch(
        int n, const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame) override
    {
        int produced = 0;
        for (int attempted = 0; attempted < n && cursor_ < static_cast<int>(handle_.total_frames());
             ++attempted)
        {
            const int idx = cursor_++;
            int delay_ms = 100;
            CGImageRef cg = handle_.decode_next(max_w_, max_h_, delay_ms);
            if (!cg)
            {
                continue;
            }
            on_frame(idx, make_image(cg), delay_ms);
            CGImageRelease(cg); // make_image() took its own extra retain
            ++produced;
        }
        return produced;
    }

    bool exhausted() const override
    {
        return cursor_ >= static_cast<int>(handle_.total_frames());
    }

    void restart() override
    {
        handle_.restart();
        cursor_ = 0;
    }

private:
    WebPDecodeHandle handle_;
    int max_w_, max_h_;
    int cursor_ = 0;
};

} // namespace

bool is_webp_data(std::span<const std::uint8_t> bytes)
{
    return tesseract::is_webp_data(bytes);
}

DecodedFrames decode_webp_bytes_libwebp(
    std::span<const std::uint8_t> bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<Image>, int)>* on_first_frame,
    const std::function<void(int, std::unique_ptr<Image>, int)>* on_extra_frame)
{
    DecodedFrames d;
    WebPDecodeHandle handle(bytes);
    if (!handle.valid())
    {
        return d;
    }
    const std::size_t count =
        std::min<std::size_t>(handle.total_frames(), tk::kAnimDecodeMaxFrames);
    if (count == 0)
    {
        return d;
    }

    const bool streaming = on_first_frame && on_extra_frame;
    d.frames.reserve(count);
    d.delays_ms.reserve(count);
    bool first_emitted = false;
    int extra_index = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        int delay = 100;
        CGImageRef cg = handle.decode_next(max_w, max_h, delay);
        if (!cg)
        {
            continue;
        }
        if (streaming)
        {
            if (!first_emitted)
            {
                first_emitted = true;
                (*on_first_frame)(make_image(cg), delay);
            }
            else
            {
                (*on_extra_frame)(extra_index++, make_image(cg), delay);
            }
        }
        else
        {
            d.frames.push_back(make_image(cg));
            d.delays_ms.push_back(delay);
        }
        CGImageRelease(cg); // make_image() took its own extra retain
    }
    return d;
}

DecodedFrames decode_webp_bytes_windowed_libwebp(
    std::span<const std::uint8_t> bytes, int max_w, int max_h, int window_threshold_frames,
    const std::function<void(std::unique_ptr<Image>, int, std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<Image>, int)>& on_extra_frame)
{
    DecodedFrames d;
    // Peek the frame count without committing to the batch/windowed choice
    // below — cheap: WebPAnimDecoderNew parses the container but decodes no
    // pixels up front, mirroring decode_image_bytes_windowed's own
    // count-only CGImageSourceRef parse.
    WebPDecodeHandle probe(bytes);
    if (!probe.valid())
    {
        return d;
    }
    const std::size_t count = std::min<std::size_t>(probe.total_frames(), tk::kAnimDecodeMaxFrames);
    if (count <= 1)
    {
        return d; // not animated — caller falls back to the still decode
    }

    if (count <= static_cast<std::size_t>(window_threshold_frames))
    {
        std::function<void(std::unique_ptr<Image>, int)> first_cb =
            [&](std::unique_ptr<Image> img, int delay_ms)
        {
            on_first_frame(std::move(img), delay_ms, nullptr, 0);
        };
        decode_webp_bytes_libwebp(bytes, max_w, max_h, &first_cb, &on_extra_frame);
        return d;
    }

    auto session = std::make_shared<WebPAnimSession>(bytes, max_w, max_h);
    bool got_first = false;
    session->decode_next_batch(tk::kAnimDecodeInitialBatchFrames,
                               [&](int idx, std::unique_ptr<Image> img, int delay_ms)
                               {
                                   if (!got_first)
                                   {
                                       got_first = true;
                                       on_first_frame(std::move(img), delay_ms, session, count);
                                   }
                                   else
                                   {
                                       on_extra_frame(idx, std::move(img), delay_ms);
                                   }
                               });
    return d;
}

} // namespace tk::cg
