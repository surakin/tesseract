#pragma once

// Per-platform microphone capture abstraction. Parallel to tk::AudioPlayer.
// All backends normalise to 48kHz / 16-bit signed LE / mono before firing
// on_stopped. Amplitude sampling happens inside each backend at ~100ms windows
// (max absolute sample value, normalised to [0, 1000]).
//
// Threading: implementations marshal on_amplitude and on_stopped to the UI
// thread via the PostFn passed to their factory function.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tk
{

class AudioCapture
{
public:
    virtual ~AudioCapture() = default;

    // Begin capture. No-op if already recording. On permission failure,
    // fires on_stopped with empty PCM immediately (no on_amplitude calls).
    virtual void start() = 0;

    // Stop recording and fire on_stopped with the full PCM buffer.
    // No-op if not recording.
    virtual void stop() = 0;

    // Stop recording and discard; on_stopped is NOT fired.
    // No-op if not recording.
    virtual void cancel() = 0;

    virtual bool is_recording() const = 0;

    // Elapsed recording time in milliseconds. 0 when not recording.
    virtual std::uint64_t duration_ms() const = 0;

    // Fired ~10x/sec on the UI thread while recording.
    // amplitude is in [0, 1000] (max absolute sample value in the window,
    // normalised from int16 range).
    std::function<void(std::uint16_t amplitude)> on_amplitude;

    // Fired on the UI thread after stop() completes.
    // pcm:      48kHz / 16-bit signed LE / mono raw samples.
    // waveform: amplitude samples collected during recording, one per ~100ms
    //           window, each in [0, 1000].
    // duration_ms: total clip length in milliseconds.
    // Empty pcm indicates a permission or device error (don't send).
    std::function<void(std::vector<std::uint8_t> pcm,
                       std::vector<std::uint16_t> waveform,
                       std::uint64_t duration_ms)>
        on_stopped;

#ifdef TESSERACT_CALLS_ENABLED
    // Live per-frame callback, reserved for future use.
    // Called on the capture thread with each incoming PCM chunk (48kHz/S16LE/
    // mono). May be called concurrently with on_amplitude/on_stopped dispatch.
    // Must not be called while a set_frame_callback/clear_frame_callback is in
    // progress on another thread — callers must serialize those transitions.
    virtual void set_frame_callback(
        std::function<void(const std::int16_t*, std::size_t)> cb) = 0;
    virtual void clear_frame_callback() = 0;
#endif
};

// Factory function declarations — each defined in audio_capture_<platform>.cpp.
// `post` is a thread-safe functor that schedules its argument on the UI thread.
using AudioCapturePostFn = std::function<void(std::function<void()>)>;

std::unique_ptr<AudioCapture> make_audio_capture_qt(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_gtk(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_win32(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_macos(AudioCapturePostFn post);

} // namespace tk
