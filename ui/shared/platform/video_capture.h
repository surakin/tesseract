#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tk
{

// Per-platform camera capture abstraction. Mirrors tk::AudioCapture in structure.
// Delivers raw I420 frames at ~30fps from the default camera device.
//
// Failures (no device, device held by another app, permission denied, the
// stream dying mid-capture) are reported through the error callback and
// error(), from whichever thread detected them — start() itself, or a
// backend streaming/bus thread later. Only the first error after start() is
// reported; the capture delivers no further frames after one and should be
// discarded (stop() + recreate to retry once the device may be free again).
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

    enum class Error
    {
        None,
        NoDevice,          // no camera, or the selected one is gone
        Busy,              // held exclusively by another app
        PermissionDenied,  // OS privacy setting / device node permissions
        Failed,            // anything else (negotiation, driver error, ...)
    };

    // Fired at most once per start(), on any thread. Must be thread-safe.
    using ErrorCallback = std::function<void(Error)>;

    // Callback for premultiplied BGRA frames (B,G,R,A byte order, stride=w*4).
    // Delivered using the platform's native colour converter — no software
    // BT.601 loop. Camera pixels are opaque (A=255) so premult = straight.
    using BgraCallback =
        std::function<void(const std::uint8_t* bgra, std::uint32_t w,
                           std::uint32_t h)>;

    virtual ~VideoCapture() = default;

    // Begin delivering frames to the registered callback(s).
    // No-op if already running. Failures go to the error callback.
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

    // Register the failure callback. Call before start().
    void set_error_callback(ErrorCallback cb);

    // The error reported since the last start(), or Error::None. Thread-safe;
    // lets paint-driven widgets poll instead of marshalling the callback.
    Error error() const { return error_.load(std::memory_order_acquire); }

    // Platform factory — returns nullptr if no camera device is present.
    static std::unique_ptr<VideoCapture> create();

    // Tests only: replace the platform factory used by create(). Pass an
    // empty function to restore the real one.
    static void set_factory_for_testing(
        std::function<std::unique_ptr<VideoCapture>()> factory);

    // Localised user-facing sentence for an error (empty for None).
    static std::string describe(Error e);

protected:
    // Backends call this on failure. The first error after start() wins;
    // later ones (e.g. the state-change failure that follows a bus error)
    // are dropped so consumers see exactly one notification.
    void report_error_(Error e);
    // Backends call this at the top of start().
    void clear_error_() { error_.store(Error::None, std::memory_order_release); }

private:
    std::atomic<Error> error_{Error::None};
    std::mutex         error_cb_mu_;
    ErrorCallback      error_cb_;
};

// Per-platform factory function declarations (each defined in
// video_capture_<platform>.cpp; only one is linked per build).
std::unique_ptr<VideoCapture> make_video_capture_gst();
std::unique_ptr<VideoCapture> make_video_capture_macos();
std::unique_ptr<VideoCapture> make_video_capture_win32();

} // namespace tk
