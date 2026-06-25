// macOS audio output backend for tk::AudioPlayback.
// Uses AVAudioEngine with an AVAudioSourceNode (macOS 10.15+) which pulls
// Float32 PCM from a shared ring buffer on the audio render thread.
// Incoming S16LE frames are converted to Float32 on push_frame().

#include "audio_playback.h"

#include <tesseract/settings.h>

#import <AVFoundation/AVFoundation.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace
{

// Shared state between the C++ object and the ObjC render block. Using
// shared_ptr ensures the state outlives the AudioPlaybackMacOS if the engine's
// render thread calls the block while the destructor is running.
struct PlaybackState
{
    std::mutex          mu;
    std::deque<float>   ring;
    bool                stopped = false;
};

class AudioPlaybackMacOS : public tk::AudioPlayback
{
public:
    AudioPlaybackMacOS()
        : state_(std::make_shared<PlaybackState>())
    {
        engine_ = [[AVAudioEngine alloc] init];

        AVAudioFormat* fmt =
            [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                            sampleRate:48000
                                              channels:1
                                           interleaved:YES];

        auto state = state_; // shared ownership captured by the block
        src_ = [[AVAudioSourceNode alloc]
            initWithFormat:fmt
               renderBlock:^AUAudioUnitStatus(
                   BOOL*                 isSilence,
                   const AudioTimeStamp* /*timestamp*/,
                   AVAudioFrameCount     frameCount,
                   AudioBufferList*      outputData) {
                   float* dst =
                       static_cast<float*>(outputData->mBuffers[0].mData);
                   std::lock_guard<std::mutex> lk(state->mu);
                   if (state->stopped)
                   {
                       std::memset(dst, 0, frameCount * sizeof(float));
                       *isSilence = YES;
                       return noErr;
                   }
                   const std::size_t n =
                       std::min(static_cast<std::size_t>(frameCount),
                                state->ring.size());
                   for (std::size_t i = 0; i < n; ++i)
                   {
                       dst[i] = state->ring.front();
                       state->ring.pop_front();
                   }
                   if (n < frameCount)
                   {
                       std::memset(dst + n, 0,
                                   (frameCount - n) * sizeof(float));
                       *isSilence = (n == 0) ? YES : NO;
                   }
                   return noErr;
               }];

        [engine_ attachNode:src_];
        [engine_ connect:src_ to:engine_.mainMixerNode format:fmt];

        // Select the preferred output device if one is configured.
        // The stored ID is the CoreAudio AudioDeviceID rendered as a decimal string.
        const std::string& pref = tesseract::Settings::instance().audio_output_device_id;
        if (!pref.empty())
        {
            try
            {
                AudioDeviceID dev_id = static_cast<AudioDeviceID>(std::stoul(pref));
                AudioUnit au = engine_.outputNode.audioUnit;
                AudioUnitSetProperty(au,
                                     kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global,
                                     0, &dev_id, sizeof(dev_id));
            }
            catch (...) {} // ignore conversion errors — fall back to default
        }

        NSError* err = nil;
        if (![engine_ startAndReturnError:&err])
        {
            engine_ = nil;
            src_    = nil;
        }
    }

    ~AudioPlaybackMacOS() override
    {
        if (engine_)
        {
            // Signal the render block to stop producing audio before we tear
            // down the engine so there is no use-after-free on state_.
            {
                std::lock_guard<std::mutex> lk(state_->mu);
                state_->stopped = true;
            }
            [engine_ stop];
            engine_ = nil;
            src_    = nil;
        }
    }

    void push_frame(const std::int16_t* samples,
                    std::size_t         sample_count,
                    std::uint32_t       /*sample_rate*/,
                    std::uint32_t       /*num_channels*/) override
    {
        std::lock_guard<std::mutex> lk(state_->mu);
        if (state_->stopped)
            return;
        // Cap to ~500ms at 48kHz to prevent unbounded growth.
        const std::size_t kMaxSamples = 24000;
        if (state_->ring.size() + sample_count > kMaxSamples)
            return;
        for (std::size_t i = 0; i < sample_count; ++i)
            state_->ring.push_back(samples[i] / 32768.0f);
    }

private:
    std::shared_ptr<PlaybackState> state_;
    AVAudioEngine* __strong        engine_ = nil;
    AVAudioSourceNode* __strong    src_    = nil;
};

} // namespace

namespace tk
{

std::unique_ptr<tk::AudioPlayback> make_audio_playback_macos()
{
    // AVAudioSourceNode requires macOS 10.15+, which is our minimum deployment
    // target. If the engine fails to start (e.g. no output device), the
    // constructor leaves engine_ nil and push_frame() is a safe no-op.
    return std::make_unique<AudioPlaybackMacOS>();
}

} // namespace tk
