// macOS audio capture backend for tk::AudioCapture.
// Uses AVAudioEngine + AVAudioInputNode. Installs a tap on the input node
// to receive PCM buffers at 48kHz/16-bit/mono.
// Requests microphone permission synchronously before starting.

#include "audio_capture.h"

#include <tesseract/settings.h>

#import <AVFoundation/AVFoundation.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureMacOS : public tk::AudioCapture
{
public:
    explicit AudioCaptureMacOS(PostFn post)
        : post_(std::move(post)), alive_(std::make_shared<bool>(true))
    {
    }

    ~AudioCaptureMacOS() override
    {
        *alive_ = false;
        cancel();
    }

    void start() override
    {
        if (recording_)
            return;

        // Mark recording early to block re-entry during the async permission
        // dialog. start_tp_ is set in start_engine_() so duration reflects
        // actual recording time, not the permission wait.
        pcm_.clear();
        waveform_.clear();
        window_buf_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_ = true;

        // Request mic permission asynchronously. Blocking the main thread
        // here with a semaphore prevents the system permission sheet from
        // being presented (it also needs the run loop).
        //
        // Capture a weak_ptr sentinel rather than raw `this`: the permission
        // dialog can take many seconds, and the object may be destroyed while
        // it is pending. Checking alive before (and inside) the post_ lambda
        // prevents a use-after-free if that happens.
        std::weak_ptr<bool> walive = alive_;
        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeAudio
                    completionHandler:^(BOOL auth) {
                        auto alive = walive.lock();
                        if (!alive || !*alive)
                            return;
                        if (!auth)
                        {
                            post_([this, walive]() {
                                auto alive2 = walive.lock();
                                if (!alive2 || !*alive2)
                                    return;
                                recording_ = false;
                                fire_error_();
                            });
                            return;
                        }
                        post_([this, walive]() {
                            auto alive2 = walive.lock();
                            if (!alive2 || !*alive2)
                                return;
                            if (recording_)
                                start_engine_();
                        });
                    }];
    }

    void stop() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/false);
    }

    bool is_recording() const override { return recording_; }

#ifdef TESSERACT_CALLS_ENABLED
    void set_frame_callback(
        std::function<void(const std::int16_t*, std::size_t)> cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        frame_callback_ = std::move(cb);
    }
    void clear_frame_callback() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        frame_callback_ = nullptr;
    }
#endif

    std::uint64_t duration_ms() const override
    {
        if (!recording_)
            return 0;
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());
    }

private:
    void handle_buffer_(AVAudioPCMBuffer* buf)
    {
        const std::uint32_t frame_count = buf.frameLength;
        const auto* s16 =
            reinterpret_cast<const int16_t*>(buf.int16ChannelData[0]);
        const std::size_t bytes = frame_count * 2;

#ifdef TESSERACT_CALLS_ENABLED
        std::function<void(const std::int16_t*, std::size_t)> frame_cb;
#endif
        {
            std::lock_guard<std::mutex> lk(mu_);
            pcm_.insert(pcm_.end(),
                        reinterpret_cast<const uint8_t*>(s16),
                        reinterpret_cast<const uint8_t*>(s16) + bytes);
            window_buf_.insert(window_buf_.end(), s16, s16 + frame_count);
            window_byte_count_ += bytes;
#ifdef TESSERACT_CALLS_ENABLED
            frame_cb = frame_callback_;
#endif

            if (window_byte_count_ >= 9600)
            {
                int16_t peak = 0;
                for (int16_t v : window_buf_)
                    peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
                std::uint16_t amp =
                    static_cast<uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);
                waveform_.push_back(amp);
                window_buf_.clear();
                window_byte_count_ = 0;

                if (on_amplitude)
                {
                    auto cb = on_amplitude;
                    post_([cb, amp]() { cb(amp); });
                }
            }
        }
#ifdef TESSERACT_CALLS_ENABLED
        if (frame_cb)
            frame_cb(s16, frame_count);
#endif
    }

    void start_engine_()
    {
        // Called on the UI thread after mic permission is granted.
        start_tp_ = std::chrono::steady_clock::now();

        engine_ = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = engine_.inputNode;

        // Select the preferred input device if one is configured.
        // The stored ID is an AVFoundation UUID string (e.g. "AppleHDAEngineInput:1B,0,1,1:0"),
        // NOT a decimal integer. Enumerate CoreAudio devices, find the one whose
        // kAudioDevicePropertyDeviceUID matches, then set it on the audio unit.
        const std::string& pref = tesseract::Settings::instance().audio_input_device_id;
        if (!pref.empty())
        {
            AudioDeviceID dev_id = kAudioObjectUnknown;

            // Get the list of all audio devices.
            AudioObjectPropertyAddress addr{
                kAudioHardwarePropertyDevices,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 data_size = 0;
            if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr,
                                              0, nullptr, &data_size) == noErr
                && data_size > 0)
            {
                const UInt32 count = data_size / sizeof(AudioDeviceID);
                std::vector<AudioDeviceID> devices(count);
                if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                               0, nullptr, &data_size,
                                               devices.data()) == noErr)
                {
                    NSString* target_uid =
                        [NSString stringWithUTF8String:pref.c_str()];
                    for (AudioDeviceID candidate : devices)
                    {
                        AudioObjectPropertyAddress uid_addr{
                            kAudioDevicePropertyDeviceUID,
                            kAudioObjectPropertyScopeGlobal,
                            kAudioObjectPropertyElementMain
                        };
                        CFStringRef uid_cf = nullptr;
                        UInt32 uid_size = sizeof(uid_cf);
                        if (AudioObjectGetPropertyData(candidate, &uid_addr,
                                                       0, nullptr, &uid_size,
                                                       &uid_cf) == noErr
                            && uid_cf != nullptr)
                        {
                            bool match = [(__bridge NSString*)uid_cf
                                isEqualToString:target_uid];
                            CFRelease(uid_cf);
                            if (match)
                            {
                                dev_id = candidate;
                                break;
                            }
                        }
                    }
                }
            }

            if (dev_id != kAudioObjectUnknown)
            {
                AudioUnit au = input.audioUnit;
                AudioUnitSetProperty(au,
                                     kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global,
                                     0, &dev_id, sizeof(dev_id));
            }
            // If not found, fall back to the default input device (no action needed).
        }

        AVAudioFormat* fmt = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatInt16
                     sampleRate:48000
                       channels:1
                    interleaved:YES];

        [input installTapOnBus:0
                    bufferSize:4800
                        format:fmt
                         block:^(AVAudioPCMBuffer* buf, AVAudioTime*) {
                             handle_buffer_(buf);
                         }];

        NSError* err = nil;
        [engine_ startAndReturnError:&err];
        if (err)
        {
            [input removeTapOnBus:0];
            engine_ = nil;
            recording_ = false;
            fire_error_();
        }
    }

    void finish_(bool send)
    {
        if (engine_)
        {
            [engine_.inputNode removeTapOnBus:0];
            [engine_ stop];
            engine_ = nil;
        }
        recording_ = false;

        using namespace std::chrono;
        const std::uint64_t dur = static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());

        if (send && on_stopped)
        {
            std::vector<uint8_t> pcm;
            std::vector<uint16_t> wf;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pcm = std::move(pcm_);
                wf  = std::move(waveform_);
            }
            auto cb = on_stopped;
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }
    }

    void fire_error_()
    {
        if (on_stopped)
        {
            auto cb = on_stopped;
            post_([cb]() mutable { cb({}, {}, 0); });
        }
    }

    PostFn post_;
    std::shared_ptr<bool> alive_;
    bool recording_ = false;
    AVAudioEngine* __strong engine_ = nil;
    std::chrono::steady_clock::time_point start_tp_;

    std::mutex mu_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_buf_;
    std::size_t                window_byte_count_ = 0;
#ifdef TESSERACT_CALLS_ENABLED
    std::function<void(const std::int16_t*, std::size_t)> frame_callback_;
#endif
};

} // namespace

namespace tk
{

std::unique_ptr<AudioCapture>
make_audio_capture_macos(AudioCapturePostFn post)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AVCaptureDeviceDiscoverySession* session = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInMicrophone,
                                           AVCaptureDeviceTypeExternalUnknown ]
                              mediaType:AVMediaTypeAudio
                               position:AVCaptureDevicePositionUnspecified];
#pragma clang diagnostic pop
    if (session.devices.count == 0)
        return nullptr;
    return std::make_unique<AudioCaptureMacOS>(std::move(post));
}

} // namespace tk
