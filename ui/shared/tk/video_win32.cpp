// Win32 video backend for tk::VideoPlayer.
// Audio: IMFMediaEngine (audio-only), same pattern as audio_win32.cpp.
// Video frames: IMFSourceReader on a worker thread; each frame is converted
// to BGRA and wrapped as a D2DImage via make_image_from_bgra.
//
// Thread model:
//   - UI thread: all public methods, current_frame(), on_frame/on_progress.
//   - Decode thread: reads video samples, writes current_frame_ under a
//     mutex, then PostMessageW → UI thread to fire on_frame.
//   - Timer thread: progress ticks at ~60 ms via CreateTimerQueueTimer,
//     marshalled to UI thread the same way as audio_win32.cpp.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C" HRESULT STDAPICALLTYPE
MFCreateMFByteStreamOnStream(IStream* pStream, IMFByteStream** ppByteStream);
#endif

#include "video.h"
#include "canvas_d2d.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "shlwapi.lib")

namespace tk::win32
{

using PostFn = std::function<void(std::function<void()>)>;

namespace
{
void ensure_mf_started()
{
    static std::once_flag flag;
    std::call_once(flag,
                   []()
                   {
                       MFStartup(MF_VERSION, MFSTARTUP_LITE);
                   });
}

std::wstring mime_to_url(std::string_view mime)
{
    std::string ext;
    if (!mime.empty())
    {
        auto slash = mime.rfind('/');
        ext = (slash != std::string_view::npos)
                  ? std::string(mime.substr(slash + 1))
                  : std::string(mime);
        auto semi = ext.find(';');
        if (semi != std::string::npos)
        {
            ext.resize(semi);
        }
        while (!ext.empty() && ext.back() == ' ')
        {
            ext.pop_back();
        }
    }
    if (ext.empty())
    {
        ext = "mp4";
    }
    std::wstring url = L"video://clip.";
    for (char c : ext)
    {
        url += static_cast<wchar_t>(c);
    }
    return url;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  IMFMediaEngineNotify implementation (identical to audio_win32.cpp)
// ─────────────────────────────────────────────────────────────────────────
class VideoEngineNotify final : public IMFMediaEngineNotify
{
public:
    VideoEngineNotify(std::shared_ptr<std::atomic<bool>> alive, PostFn post)
        : alive_(std::move(alive)), post_(std::move(post))
    {
    }

    void set_progress(std::function<void()> cb)
    {
        progress_ = std::move(cb);
    }

    void set_error(std::function<void()> cb)
    {
        error_ = std::move(cb);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++ref_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref_;
        if (r == 0)
        {
            delete this;
        }
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify))
        {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR,
                                          DWORD) override
    {
        if (event != MF_MEDIA_ENGINE_EVENT_ENDED &&
            event != MF_MEDIA_ENGINE_EVENT_ERROR &&
            event != MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE)
        {
            return S_OK;
        }
        auto alive = alive_;
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
        {
            auto cb = error_;
            post_(
                [alive = std::move(alive), cb = std::move(cb)]()
                {
                    if (*alive && cb)
                    {
                        cb();
                    }
                });
        }
        else
        {
            auto cb = progress_;
            post_(
                [alive = std::move(alive), cb = std::move(cb)]()
                {
                    if (*alive && cb)
                    {
                        cb();
                    }
                });
        }
        return S_OK;
    }

private:
    std::shared_ptr<std::atomic<bool>> alive_;
    PostFn post_;
    std::function<void()> progress_;
    std::function<void()> error_;
    std::atomic<ULONG> ref_{1};
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32VideoPlayer
// ─────────────────────────────────────────────────────────────────────────
class Win32VideoPlayer final : public tk::VideoPlayer
{
public:
    Win32VideoPlayer(PostFn post, tk::d2d::Backend* backend)
        : post_(std::move(post)), backend_(backend),
          alive_(std::make_shared<std::atomic<bool>>(true))
    {
        ensure_mf_started();
        init_audio_engine();
    }

    ~Win32VideoPlayer() override
    {
        *alive_ = false;
        stop_decode_thread();
        stop_timer();
        if (engine_)
        {
            engine_->Pause();
            engine_->Shutdown();
            engine_->Release();
            engine_ = nullptr;
        }
        if (notify_)
        {
            notify_->set_progress(nullptr);
            notify_->Release();
            notify_ = nullptr;
        }
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view mime) override
    {
        if (!data || size == 0)
        {
            return;
        }
        stop_decode_thread();
        stop_timer();
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }

        bytes_.assign(data, data + size);
        mime_ = std::string(mime);

        // Start audio engine.
        if (engine_)
        {
            engine_->Pause();
            Microsoft::WRL::ComPtr<IStream> stream;
            stream.Attach(
                SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes_.data()),
                                  static_cast<UINT>(bytes_.size())));
            Microsoft::WRL::ComPtr<IMFByteStream> mf_stream;
            if (stream && SUCCEEDED(MFCreateMFByteStreamOnStream(
                              stream.Get(), mf_stream.GetAddressOf())))
            {
                std::wstring url = mime_to_url(mime);
                BSTR burl = SysAllocString(url.c_str());
                Microsoft::WRL::ComPtr<IMFMediaEngineEx> engine_ex;
                if (burl && SUCCEEDED(engine_->QueryInterface(
                                engine_ex.GetAddressOf())))
                {
                    engine_ex->SetSourceFromByteStream(mf_stream.Get(), burl);
                }
                SysFreeString(burl);
                engine_->SetPlaybackRate(static_cast<double>(rate_));
                engine_->Play();
            }
        }

        // Start video decode thread.
        start_decode_thread();
        start_timer();
    }

    void pause() override
    {
        if (engine_)
        {
            engine_->Pause();
        }
        stop_timer();
        if (on_progress)
        {
            on_progress();
        }
    }
    void resume() override
    {
        if (engine_)
        {
            engine_->SetPlaybackRate(static_cast<double>(rate_));
            engine_->Play();
        }
        start_timer();
    }
    void stop() override
    {
        stop_decode_thread();
        stop_timer();
        if (engine_)
        {
            engine_->Pause();
            engine_->SetCurrentTime(0.0);
        }
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        if (on_progress)
        {
            on_progress();
        }
    }
    void seek(std::uint64_t ms) override
    {
        if (!engine_)
        {
            return;
        }
        double target = static_cast<double>(ms) / 1000.0;
        double dur = engine_->GetDuration();
        if (dur > 0.0 && target > dur)
        {
            target = dur;
        }
        if (target < 0.0)
        {
            target = 0.0;
        }
        engine_->SetCurrentTime(target);
        if (on_progress)
        {
            on_progress();
        }
    }

    void set_playback_rate(float rate) override
    {
        if (rate < 0.25f)
        {
            rate = 0.25f;
        }
        if (rate > 4.0f)
        {
            rate = 4.0f;
        }
        rate_ = rate;
        if (engine_)
        {
            engine_->SetPlaybackRate(static_cast<double>(rate_));
        }
    }
    float playback_rate() const override
    {
        return rate_;
    }

    std::uint64_t position_ms() const override
    {
        if (!engine_)
        {
            return 0u;
        }
        double t = engine_->GetCurrentTime();
        return (t > 0.0) ? static_cast<std::uint64_t>(t * 1000.0) : 0u;
    }
    std::uint64_t duration_ms() const override
    {
        if (!engine_)
        {
            return 0u;
        }
        double d = engine_->GetDuration();
        return (d > 0.0 && d < 1.0e10) ? static_cast<std::uint64_t>(d * 1000.0)
                                       : 0u;
    }
    bool is_playing() const override
    {
        return engine_ && !engine_->IsPaused() && !engine_->IsEnded();
    }

    const tk::Image* current_frame() const override
    {
        std::lock_guard lk(frame_mutex_);
        return current_frame_.get();
    }

private:
    void init_audio_engine()
    {
        notify_ = new VideoEngineNotify(alive_, post_);
        notify_->set_progress(
            [this]()
            {
                if (on_progress)
                {
                    on_progress();
                }
            });
        notify_->set_error(
            [this]()
            {
                if (on_error)
                {
                    on_error();
                }
            });

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 3)))
        {
            return;
        }
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_);
        attrs->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, 0);

        Microsoft::WRL::ComPtr<IMFMediaEngineClassFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                    CLSCTX_ALL,
                                    IID_PPV_ARGS(factory.GetAddressOf()))))
        {
            return;
        }

        Microsoft::WRL::ComPtr<IMFMediaEngine> engine;
        if (FAILED(factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY,
                                           attrs.Get(), engine.GetAddressOf())))
        {
            return;
        }
        engine_ = engine.Detach();
    }

    void start_decode_thread()
    {
        stop_decode_thread();
        decode_running_ = true;
        decode_thread_ = std::thread(
            [this]()
            {
                decode_loop();
            });
    }

    void stop_decode_thread()
    {
        decode_running_ = false;
        if (decode_thread_.joinable())
        {
            decode_thread_.join();
        }
    }

    void decode_loop()
    {
        // Co-initialise for this thread (needed by IMFSourceReader).
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto fire_error = [this]()
        {
            auto alive = alive_;
            auto err   = on_error;
            post_(
                [alive = std::move(alive), err = std::move(err)]()
                {
                    if (*alive && err)
                    {
                        err();
                    }
                });
        };

        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(
            SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes_.data()),
                              static_cast<UINT>(bytes_.size())));
        if (!stream)
        {
            fire_error();
            CoUninitialize();
            return;
        }

        Microsoft::WRL::ComPtr<IMFByteStream> mf_stream;
        if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(),
                                                mf_stream.GetAddressOf())))
        {
            fire_error();
            CoUninitialize();
            return;
        }

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(attrs.GetAddressOf(), 2);
        if (attrs)
        {
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }

        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromByteStream(
                mf_stream.Get(), attrs.Get(), reader.GetAddressOf())))
        {
            fire_error();
            CoUninitialize();
            return;
        }

        // Configure video output format: RGB32 (BGRA on Windows).
        Microsoft::WRL::ComPtr<IMFMediaType> video_type;
        MFCreateMediaType(video_type.GetAddressOf());
        if (video_type)
        {
            video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, video_type.Get());
        }

        // Get actual frame size.
        Microsoft::WRL::ComPtr<IMFMediaType> actual_type;
        if (FAILED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                actual_type.GetAddressOf())))
        {
            fire_error();
            CoUninitialize();
            return;
        }
        UINT32 frame_w = 0, frame_h = 0;
        MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE, &frame_w,
                           &frame_h);
        if (frame_w == 0 || frame_h == 0)
        {
            fire_error();
            CoUninitialize();
            return;
        }

        // Media Foundation pads each row to a stride that the decoder picks for
        // alignment, which is often larger than frame_w*4 (e.g. a 200px-wide
        // video aligns to 208px → 832-byte rows, not the 800 a packed buffer
        // would have). MF_MT_DEFAULT_STRIDE carries the real stride (signed:
        // negative means a bottom-up buffer). We repack to a tightly-packed
        // BGRA buffer below; copying the raw buffer blindly would shear the
        // image diagonally.
        LONG src_stride = 0;
        {
            UINT32 s32 = 0;
            if (SUCCEEDED(
                    actual_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s32)))
            {
                src_stride = static_cast<LONG>(static_cast<INT32>(s32));
            }
        }

        // Read video samples at 60 Hz pace (no exact timing; just drain as fast
        // as practical so frames are up-to-date during scrub).
        while (decode_running_)
        {
            DWORD stream_idx = 0, flags = 0;
            LONGLONG ts = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                &stream_idx, &flags, &ts, sample.GetAddressOf());

            if (FAILED(hr))
            {
                fire_error();
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                break;
            }
            if (!sample)
            {
                continue;
            }

            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf())))
            {
                continue;
            }

            BYTE* ptr = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&ptr, nullptr, &len)))
            {
                continue;
            }

            // MFVideoFormat_RGB32 is BGRX; the unused 4th byte is 0x00. make_image_from_bgra
            // marks the resulting D2DImage opaque so D2D ignores the zero alpha.
            //
            // Repack from MF's (possibly row-padded) buffer into a tightly-
            // packed frame_w*4 buffer. Decoders pad each row for alignment
            // (e.g. a 200px-wide video → 208px → 832-byte rows, not 800), so
            // copying the buffer blindly shears the image diagonally.
            //
            // We derive the stride MAGNITUDE from the contiguous buffer length
            // (len == |stride| * height), which is the ground truth: the
            // advertised MF_MT_DEFAULT_STRIDE reports the *unpadded* width here
            // and cannot be trusted for the magnitude. MF_MT_DEFAULT_STRIDE is
            // still used for the SIGN (negative → bottom-up buffer).
            const UINT dst_stride = frame_w * 4u;
            UINT abs_stride = 0;
            if (frame_h != 0 && (len % frame_h) == 0)
            {
                abs_stride = len / frame_h;
            }
            else if (src_stride != 0)
            {
                abs_stride =
                    static_cast<UINT>(src_stride < 0 ? -src_stride : src_stride);
            }
            else
            {
                abs_stride = dst_stride;
            }
            const LONG stride =
                (src_stride < 0) ? -static_cast<LONG>(abs_stride)
                                 : static_cast<LONG>(abs_stride);
            // Guard against a malformed stride/length that would read past the
            // locked buffer.
            if (abs_stride < dst_stride ||
                static_cast<DWORD>(abs_stride) * frame_h > len)
            {
                buf->Unlock();
                continue;
            }
            std::vector<uint8_t> pixels(static_cast<size_t>(dst_stride) *
                                        frame_h);
            // Negative stride → bottom-up buffer: the first row in memory is the
            // last row of the image, so start at the bottom and walk back up.
            const BYTE* src_base =
                (stride < 0)
                    ? ptr + static_cast<size_t>(abs_stride) * (frame_h - 1)
                    : ptr;
            for (UINT y = 0; y < frame_h; ++y)
            {
                const BYTE* src_row =
                    src_base + static_cast<ptrdiff_t>(y) * stride;
                std::memcpy(pixels.data() +
                                static_cast<size_t>(y) * dst_stride,
                            src_row, dst_stride);
            }
            buf->Unlock();

            if (backend_)
            {
                auto img = tk::d2d::make_image_from_bgra(
                    *backend_, pixels.data(), static_cast<int>(frame_w),
                    static_cast<int>(frame_h));
                if (img)
                {
                    {
                        std::lock_guard lk(frame_mutex_);
                        current_frame_ = std::move(img);
                    }
                    auto alive = alive_;
                    auto on_frm = on_frame;
                    post_(
                        [alive = std::move(alive), on_frm = std::move(on_frm)]()
                        {
                            if (*alive && on_frm)
                            {
                                on_frm();
                            }
                        });
                }
            }

            // Throttle to ~60 fps (16 ms).
            Sleep(16);
        }

        CoUninitialize();
    }

    void start_timer()
    {
        if (timer_)
        {
            return;
        }
        CreateTimerQueueTimer(&timer_, nullptr, timer_cb_, this, 60, 60,
                              WT_EXECUTEDEFAULT);
    }
    void stop_timer()
    {
        if (!timer_)
        {
            return;
        }
        DeleteTimerQueueTimer(nullptr, timer_, INVALID_HANDLE_VALUE);
        timer_ = nullptr;
    }

    static void CALLBACK timer_cb_(PVOID ctx, BOOLEAN)
    {
        auto* self = static_cast<Win32VideoPlayer*>(ctx);
        auto alive = self->alive_;
        auto cb = self->on_progress;
        self->post_(
            [alive = std::move(alive), cb = std::move(cb)]()
            {
                if (*alive && cb)
                {
                    cb();
                }
            });
    }

    PostFn post_;
    tk::d2d::Backend* backend_; // borrowed
    std::shared_ptr<std::atomic<bool>> alive_;
    IMFMediaEngine* engine_ = nullptr;
    VideoEngineNotify* notify_ = nullptr;
    HANDLE timer_ = nullptr;
    float rate_ = 1.0f;
    std::string mime_;
    std::vector<uint8_t> bytes_;

    std::atomic<bool> decode_running_{false};
    std::thread decode_thread_;

    mutable std::mutex frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────
std::unique_ptr<tk::VideoPlayer>
make_video_player_win32(PostFn post, tk::d2d::Backend* backend)
{
    return std::make_unique<Win32VideoPlayer>(std::move(post), backend);
}

} // namespace tk::win32
