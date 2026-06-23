#pragma once

// Per-platform audio output abstraction for MatrixRTC call audio.
// Accepts decoded S16LE PCM frames and routes them to the system speaker.
// Thread-safe: push_frame() may be called from any thread.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace tk
{

class AudioPlayback
{
public:
    virtual ~AudioPlayback() = default;

    // Feed decoded S16LE PCM samples to the output device.
    // `sample_rate` and `num_channels` must stay constant across calls.
    // Called on the UI thread (post-marshalled by EventHandlerBase).
    virtual void push_frame(const std::int16_t* samples,
                            std::size_t         sample_count,
                            std::uint32_t       sample_rate,
                            std::uint32_t       num_channels) = 0;
};

// Factory declarations — each defined in audio_playback_<platform>.cpp.
std::unique_ptr<AudioPlayback> make_audio_playback_qt();
std::unique_ptr<AudioPlayback> make_audio_playback_gtk();
std::unique_ptr<AudioPlayback> make_audio_playback_win32();
std::unique_ptr<AudioPlayback> make_audio_playback_macos();

} // namespace tk
