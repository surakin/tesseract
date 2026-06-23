#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include "audio_capture.h"
#include <cstdint>
#include <functional>

namespace tk
{

// RAII adapter that routes live PCM frames from an AudioCapture device into a
// MatrixRTC call session.  Constructed when a call starts; destroyed on hang-up.
//
// Installs a per-frame callback on the capture device that fires on the capture
// thread for every incoming PCM chunk. The provided FrameFn is typically a
// lambda that calls the FFI's rtc_push_audio_samples().
//
// The existing on_stopped / on_amplitude paths are not disturbed, so a future
// UI element that shows the mic level during a call still works.
class AudioCaptureCallRouter
{
public:
    using FrameFn = std::function<void(const std::int16_t*, std::size_t)>;

    AudioCaptureCallRouter(tk::AudioCapture* capture, FrameFn fn)
        : capture_(capture)
    {
        capture_->set_frame_callback(std::move(fn));
    }

    ~AudioCaptureCallRouter() { capture_->clear_frame_callback(); }

    // Non-copyable, non-movable: owns the callback registration.
    AudioCaptureCallRouter(const AudioCaptureCallRouter&)            = delete;
    AudioCaptureCallRouter& operator=(const AudioCaptureCallRouter&) = delete;

private:
    tk::AudioCapture* capture_; // non-owning
};

} // namespace tk

#endif // TESSERACT_CALLS_ENABLED
