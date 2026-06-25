// Win32 audio output backend for tk::AudioPlayback.
// Uses WASAPI IAudioRenderClient in shared mode with AUTOCONVERTPCM so Windows
// handles resampling from our 48kHz S16LE mono to the device's native mix
// format. A background thread drains the pending sample queue and writes to the
// render endpoint at the hardware buffer period (~10ms).

#include "audio_playback.h"

#include <tesseract/settings.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

// Target hardware buffer duration in 100ns units (200ms for jitter tolerance).
constexpr REFERENCE_TIME kBufferDuration = 2'000'000;

class AudioPlaybackWin32 : public tk::AudioPlayback
{
public:
    AudioPlaybackWin32(IAudioClient* client, IAudioRenderClient* renderer,
                       UINT32 buffer_frames)
        : audio_client_(client), renderer_(renderer),
          buffer_frames_(buffer_frames)
    {
        audio_client_->Start();
        render_thread_ = std::thread([this]() { render_loop_(); });
    }

    ~AudioPlaybackWin32() override
    {
        stop_.store(true, std::memory_order_release);
        if (render_thread_.joinable())
            render_thread_.join();
        audio_client_->Stop();
        renderer_->Release();
        audio_client_->Release();
    }

    void push_frame(const std::int16_t* samples,
                    std::size_t         sample_count,
                    std::uint32_t       /*sample_rate*/,
                    std::uint32_t       /*num_channels*/) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Cap to ~500ms to prevent unbounded growth if the render thread lags.
        if (pending_.size() + sample_count > 48000 / 2)
            return;
        pending_.insert(pending_.end(), samples, samples + sample_count);
    }

private:
    void render_loop_()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            Sleep(10);

            UINT32 padding = 0;
            if (FAILED(audio_client_->GetCurrentPadding(&padding)))
                continue;
            const UINT32 available = buffer_frames_ - padding;
            if (available == 0)
                continue;

            std::vector<std::int16_t> local;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::size_t n =
                    std::min(static_cast<std::size_t>(available),
                             pending_.size());
                if (n > 0)
                {
                    local.assign(pending_.begin(),
                                 pending_.begin() + static_cast<std::ptrdiff_t>(n));
                    pending_.erase(pending_.begin(),
                                   pending_.begin() + static_cast<std::ptrdiff_t>(n));
                }
            }

            BYTE* buf = nullptr;
            const UINT32 write_frames =
                local.empty()
                    ? std::min(available, static_cast<UINT32>(480)) // silence chunk
                    : static_cast<UINT32>(local.size());
            if (FAILED(renderer_->GetBuffer(write_frames, &buf)))
                continue;

            if (local.empty())
            {
                renderer_->ReleaseBuffer(write_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            else
            {
                std::memcpy(buf, local.data(),
                            local.size() * sizeof(std::int16_t));
                renderer_->ReleaseBuffer(write_frames, 0);
            }
        }
    }

    IAudioClient*       audio_client_;
    IAudioRenderClient* renderer_;
    UINT32              buffer_frames_;
    std::atomic<bool>   stop_{false};
    std::thread         render_thread_;
    std::mutex          mu_;
    std::vector<std::int16_t> pending_;
};

} // namespace

namespace tk
{

std::unique_ptr<tk::AudioPlayback> make_audio_playback_win32()
{
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;

    IMMDevice* device = nullptr;
    {
        const std::string& pref = tesseract::Settings::instance().audio_output_device_id;
        HRESULT hr2;
        if (pref.empty())
        {
            hr2 = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        else
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, pref.c_str(), -1, nullptr, 0);
            std::wstring wid(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pref.c_str(), -1, wid.data(), n);
            hr2 = enumerator->GetDevice(wid.c_str(), &device);
            if (FAILED(hr2))
                hr2 = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        enumerator->Release();
        if (FAILED(hr2))
            return nullptr;
    }

    IAudioClient* client = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr))
        return nullptr;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 2;
    wfx.nAvgBytesPerSec = 96000;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            kBufferDuration, 0, &wfx, nullptr);
    if (FAILED(hr))
    {
        client->Release();
        return nullptr;
    }

    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);

    IAudioRenderClient* renderer = nullptr;
    hr = client->GetService(IID_PPV_ARGS(&renderer));
    if (FAILED(hr))
    {
        client->Release();
        return nullptr;
    }

    return std::make_unique<AudioPlaybackWin32>(client, renderer, buffer_frames);
}

} // namespace tk
