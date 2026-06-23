#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

    virtual ~VideoCapture() = default;

    // Begin delivering frames to the registered callback.
    // No-op if already running or if no camera is available.
    virtual void start() = 0;

    // Stop the pipeline and cease calling the callback.
    // No-op if not running.
    virtual void stop() = 0;

    // Register the per-frame callback. Must be called before start().
    virtual void set_callback(FrameCallback cb) = 0;

    // Platform factory — returns nullptr if no camera device is present.
    static std::unique_ptr<VideoCapture> create();
};

// Per-platform factory function declarations (each defined in
// video_capture_<platform>.cpp; only one is linked per build).
std::unique_ptr<VideoCapture> make_video_capture_gst();
std::unique_ptr<VideoCapture> make_video_capture_macos();
std::unique_ptr<VideoCapture> make_video_capture_win32();

} // namespace tk

#endif // TESSERACT_CALLS_ENABLED
