#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace tk
{

// Per-platform camera capture abstraction. Mirrors tk::AudioCapture in structure.
// Delivers raw I420 frames at ~30fps from the default camera device.
// start() is a no-op when no camera is available; the RTC session continues
// audio-only in that case.
class VideoCapture
{
public:
    struct Frame
    {
        const std::uint8_t* y;
        const std::uint8_t* u;
        const std::uint8_t* v;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride_y;
        std::uint32_t stride_u;
        std::uint32_t stride_v;
    };

    using FrameCallback = std::function<void(const Frame&)>;

    // Callback for premultiplied BGRA frames (B,G,R,A byte order, stride=w*4).
    // Delivered using the platform's native colour converter — no software
    // BT.601 loop. Camera pixels are opaque (A=255) so premult = straight.
    using BgraCallback =
        std::function<void(const std::uint8_t* bgra, std::uint32_t w,
                           std::uint32_t h)>;

    virtual ~VideoCapture() = default;

    // Begin delivering frames to the registered callback(s).
    // No-op if already running or if no camera is available.
    virtual void start() = 0;

    // Stop the pipeline and cease calling the callbacks.
    // No-op if not running.
    virtual void stop() = 0;

    // Register the per-frame I420 callback. Must be called before start().
    // Used by the RTC path (rtc_push_video_frame_i420).
    virtual void set_callback(FrameCallback cb) = 0;

    // Register the per-frame BGRA callback. Must be called before start().
    // Used by CameraWidget for the /selfie overlay. Default no-op — platforms
    // override this to deliver BGRA using native conversion.
    virtual void set_bgra_callback(BgraCallback /*cb*/) {}

    // Platform factory — returns nullptr if no camera device is present.
    static std::unique_ptr<VideoCapture> create();
};

// Per-platform factory function declarations (each defined in
// video_capture_<platform>.cpp; only one is linked per build).
std::unique_ptr<VideoCapture> make_video_capture_gst();
std::unique_ptr<VideoCapture> make_video_capture_macos();
std::unique_ptr<VideoCapture> make_video_capture_win32();

} // namespace tk
