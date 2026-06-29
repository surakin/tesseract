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
    return make_screen_capture_gst();
#endif
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
