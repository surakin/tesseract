#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

/// One capturable screen or application window.
struct ScreenSource
{
    std::string id;           // opaque platform handle (stringified)
    std::string display_name; // user-visible name, e.g. "Display 1", "My App"
    bool        is_window;    // false = full monitor, true = application window
};

/// Cross-platform screen / window capture abstraction.
/// Delivers raw I420 frames on a background thread at ~15 fps.
/// Mirror of tk::VideoCapture but with source enumeration and no BGRA path.
class ScreenCapture
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

    virtual ~ScreenCapture() = default;

    /// Enumerate available capture sources (monitors + application windows).
    /// Synchronous; safe to call before start(). Returns at least one entry
    /// (the primary monitor) on any supported platform.
    virtual std::vector<ScreenSource> enumerate_sources() = 0;

    /// Select which source to capture. Must be called before start().
    /// Passing an empty string selects the primary monitor.
    virtual void set_source(const std::string& source_id) = 0;

    /// Register the per-frame I420 callback. Must be called before start().
    virtual void set_callback(FrameCallback cb) = 0;

    /// Begin capturing and delivering frames. No-op if already running.
    virtual void start() = 0;

    /// Stop capturing. No-op if not running.
    /// After stop() returns, the callback will no longer be invoked.
    virtual void stop() = 0;

    /// One-shot RGBA8888 thumbnail for a single source, independent of the
    /// continuous set_source/set_callback/start streaming path. Safe to call
    /// from a background thread — must not touch the streaming loop's state.
    /// Returns false if unsupported by this backend or capture failed; the
    /// caller should leave the tile on its placeholder in that case.
    virtual bool capture_thumbnail(const std::string& /*source_id*/,
                                   std::vector<std::uint8_t>& /*out_rgba*/,
                                   std::uint32_t& /*out_w*/, std::uint32_t& /*out_h*/)
    {
        return false;
    }

    /// Platform factory. Returns nullptr when screen capture is unavailable
    /// (e.g. missing entitlements or unsupported OS version).
    static std::unique_ptr<ScreenCapture> create();
};

std::unique_ptr<ScreenCapture> make_screen_capture_win32();
std::unique_ptr<ScreenCapture> make_screen_capture_macos();
std::unique_ptr<ScreenCapture> make_screen_capture_gst();
std::unique_ptr<ScreenCapture> make_screen_capture_portal(); // Linux/Wayland via xdg-desktop-portal

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
