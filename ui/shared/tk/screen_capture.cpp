#ifdef TESSERACT_CALLS_ENABLED
#include "screen_capture.h"

namespace tk
{

std::unique_ptr<ScreenCapture> ScreenCapture::create()
{
#if defined(_WIN32)
    return make_screen_capture_win32();
#elif defined(__APPLE__)
    return make_screen_capture_macos();
#else
    {
        const bool wayland = (std::getenv("WAYLAND_DISPLAY") != nullptr ||
                              std::getenv("FLATPAK_ID") != nullptr);
        if (wayland)
            if (auto p = make_screen_capture_portal())
                return p;
    }
    return make_screen_capture_gst();
#endif
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
