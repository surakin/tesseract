#include "video_capture.h"

#include "i18n.h"

namespace tk
{

namespace
{
std::function<std::unique_ptr<VideoCapture>()>& test_factory()
{
    static std::function<std::unique_ptr<VideoCapture>()> f;
    return f;
}
} // namespace

std::unique_ptr<VideoCapture> VideoCapture::create()
{
    if (test_factory())
        return test_factory()();
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

void VideoCapture::set_factory_for_testing(
    std::function<std::unique_ptr<VideoCapture>()> factory)
{
    test_factory() = std::move(factory);
}

void VideoCapture::set_error_callback(ErrorCallback cb)
{
    std::lock_guard<std::mutex> lk(error_cb_mu_);
    error_cb_ = std::move(cb);
}

void VideoCapture::report_error_(Error e)
{
    if (e == Error::None)
        return;
    Error expected = Error::None;
    if (!error_.compare_exchange_strong(expected, e, std::memory_order_acq_rel))
        return;
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lk(error_cb_mu_);
        cb = error_cb_;
    }
    if (cb)
        cb(e);
}

std::string VideoCapture::describe(Error e)
{
    switch (e)
    {
    case Error::None:
        return {};
    case Error::NoDevice:
        return tr("No camera found.");
    case Error::Busy:
        return tr("The camera is being used by another app.");
    case Error::PermissionDenied:
        return tr("Camera access is blocked. Check your system privacy settings.");
    case Error::Failed:
        break;
    }
    return tr("The camera could not be started.");
}

} // namespace tk
