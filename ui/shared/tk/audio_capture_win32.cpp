// Win32 audio capture backend for tk::AudioCapture.
// Uses WASAPI in shared mode (IAudioCaptureClient). No new system dependencies.

#include "audio_capture.h"

#define WIN32_LEAN_AND_MEAN
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

using PostFn = tk::AudioCapturePostFn;

// 48kHz / 16-bit / mono frame size in bytes.
constexpr std::size_t kBytesPerFrame = 2;
// Target engine period in 100ns units (10ms).
constexpr REFERENCE_TIME kBufferDuration = 100'000;

class AudioCaptureWin32 : public tk::AudioCapture
{
public:
    explicit AudioCaptureWin32(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureWin32() override { cancel(); }

    void start() override
    {
        if (recording_.load())
            return;

        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            fire_error_();
            return;
        }

        IMMDevice* device = nullptr;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                         &device);
        enumerator->Release();
        if (FAILED(hr))
        {
            fire_error_();
            return;
        }

        IAudioClient* client = nullptr;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client));
        device->Release();
        if (FAILED(hr))
        {
            fire_error_();
            return;
        }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = 48000;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = 96000;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                0, kBufferDuration, 0, &wfx, nullptr);
        if (FAILED(hr))
        {
            client->Release();
            fire_error_();
            return;
        }

        IAudioCaptureClient* capture = nullptr;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr))
        {
            client->Release();
            fire_error_();
            return;
        }

        client->Start();
        audio_client_   = client;
        capture_client_ = capture;

        pcm_.clear();
        waveform_.clear();
        window_buf_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_.store(true);
        stop_requested_.store(false);

        capture_thread_ = std::thread([this]() { capture_loop_(); });
    }

    void stop() override
    {
        if (!recording_.load())
            return;
        stop_requested_.store(true);
        if (capture_thread_.joinable())
            capture_thread_.join();
        flush_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_.load())
            return;
        stop_requested_.store(true);
        if (capture_thread_.joinable())
            capture_thread_.join();
        flush_(/*send=*/false);
    }

    bool is_recording() const override { return recording_.load(); }

    std::uint64_t duration_ms() const override
    {
        if (!recording_.load())
            return 0;
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());
    }

private:
    void capture_loop_()
    {
        while (!stop_requested_.load())
        {
            Sleep(10);
            UINT32 packet_size = 0;
            while (SUCCEEDED(
                       capture_client_->GetNextPacketSize(&packet_size)) &&
                   packet_size > 0)
            {
                BYTE*  data   = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                if (FAILED(capture_client_->GetBuffer(&data, &frames, &flags,
                                                       nullptr, nullptr)))
                    break;

                const std::size_t bytes = frames * kBytesPerFrame;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (silent)
                    {
                        pcm_.insert(pcm_.end(), bytes, 0);
                        window_byte_count_ += bytes;
                    }
                    else
                    {
                        pcm_.insert(pcm_.end(), data, data + bytes);
                        const auto* s16 =
                            reinterpret_cast<const int16_t*>(data);
                        window_buf_.insert(window_buf_.end(),
                                           s16, s16 + frames);
                        window_byte_count_ += bytes;
                    }

                    // Emit amplitude every ~100ms (9600 bytes).
                    if (window_byte_count_ >= 9600)
                    {
                        int16_t peak = 0;
                        for (int16_t v : window_buf_)
                            peak = std::max(peak,
                                            static_cast<int16_t>(std::abs(v)));
                        std::uint16_t amp = static_cast<uint16_t>(
                            static_cast<uint32_t>(peak) * 1000 / 32767);
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
                capture_client_->ReleaseBuffer(frames);
            }
        }
    }

    void flush_(bool send)
    {
        if (audio_client_)
        {
            audio_client_->Stop();
            capture_client_->Release();
            audio_client_->Release();
            capture_client_ = nullptr;
            audio_client_   = nullptr;
        }

        using namespace std::chrono;
        const std::uint64_t dur = static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());

        recording_.store(false);
        stop_requested_.store(false);

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
    std::atomic<bool> recording_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread capture_thread_;
    IAudioClient*        audio_client_   = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    std::chrono::steady_clock::time_point start_tp_;

    std::mutex mu_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_buf_;
    std::size_t                window_byte_count_ = 0;
};

} // namespace

namespace tk::win32
{

std::unique_ptr<tk::AudioCapture>
make_audio_capture_win32(tk::AudioCapturePostFn post)
{
    // Quick availability check: if no capture endpoint exists, return nullptr.
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                     &device);
    enumerator->Release();
    if (FAILED(hr))
        return nullptr;
    device->Release();
    return std::make_unique<AudioCaptureWin32>(std::move(post));
}

} // namespace tk::win32
