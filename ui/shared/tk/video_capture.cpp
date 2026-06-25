#include "video_capture.h"

namespace tk
{

std::unique_ptr<VideoCapture> VideoCapture::create()
{
#if defined(__linux__)
    return make_video_capture_gst();
#elif defined(__APPLE__)
    return make_video_capture_macos();
#elif defined(_WIN32)
    return make_video_capture_win32();
#else
    return nullptr;
#endif
}

} // namespace tk
