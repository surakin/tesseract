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
// Rotate a tightly-packed BGRA pixel buffer in place, updating w and h.
// deg must be 90, 180, or 270; any other value is a no-op.
static void rotate_pixels_inplace(std::vector<uint8_t>& pixels,
                                  UINT32& w, UINT32& h, UINT32 deg)
{
    if (deg == 0 || w == 0 || h == 0)
        return;
    const UINT32 sw = w, sh = h;
    if (deg == 180)
    {
        std::vector<uint8_t> out(pixels.size());
        for (UINT32 y = 0; y < sh; ++y)
            for (UINT32 x = 0; x < sw; ++x)
            {
                const uint8_t* s =
                    pixels.data() +
                    (static_cast<size_t>(y) * sw + x) * 4;
                uint8_t* d =
                    out.data() +
                    (static_cast<size_t>(sh - 1 - y) * sw +
                     (sw - 1 - x)) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            }
        pixels = std::move(out);
    }
    else // 90 or 270
    {
        w = sh; h = sw; // dimensions swap
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
        for (UINT32 sy = 0; sy < sh; ++sy)
            for (UINT32 sx = 0; sx < sw; ++sx)
            {
                const uint8_t* s =
                    pixels.data() +
                    (static_cast<size_t>(sy) * sw + sx) * 4;
                UINT32 dx, dy;
                if (deg == 90)
                {
                    dx = sh - 1 - sy;
                    dy = sx;
                }
                else // 270
                {
                    dx = sy;
                    dy = sw - 1 - sx;
                }
                uint8_t* d =
                    out.data() +
                    (static_cast<size_t>(dy) * w + dx) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            }
        pixels = std::move(out);
    }
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
                engine_->SetLoop(loop_ ? TRUE : FALSE);
                engine_->SetMuted(muted_ ? TRUE : FALSE);
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
            if (engine_->IsEnded())
            {
                engine_->SetCurrentTime(0.0);
                start_decode_thread();
            }
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

    void set_loop(bool loop) override
    {
        loop_ = loop;
        // The audio engine loops itself; the video decode thread restarts the
        // source reader on end-of-stream (see decode_loop). They loop
        // independently, which is fine for the short autoplay/GIF clips this
        // hint targets (those carry fi.mau.no_audio, so there is no audio to
        // drift against).
        if (engine_)
        {
            engine_->SetLoop(loop ? TRUE : FALSE);
        }
    }
    void set_muted(bool muted) override
    {
        muted_ = muted;
        if (engine_)
        {
            engine_->SetMuted(muted ? TRUE : FALSE);
        }
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
        // Swap the latest decoded frame into display_frame_ under the mutex,
        // then return a pointer into display_frame_.  display_frame_ is only
        // ever read or replaced here on the UI thread, so the raw pointer
        // stays valid for the entire paint call — the decode thread can write
        // current_frame_ at any point but never touches display_frame_.
        {
            std::lock_guard lk(frame_mutex_);
            if (current_frame_)
                display_frame_ = std::move(current_frame_);
        }
        return display_frame_.get();
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

        // Query rotation metadata. The attribute lives on the native source
        // type; fall back to the output type in case the decoder propagates it.
        UINT32 rotation_deg = 0;
        {
            Microsoft::WRL::ComPtr<IMFMediaType> native_type;
            if (SUCCEEDED(reader->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    0, native_type.GetAddressOf())))
            {
                native_type->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation_deg);
            }
            if (rotation_deg == 0)
                actual_type->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation_deg);
            rotation_deg %= 360;
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

        // Compute the per-frame sleep time from the declared frame rate so
        // playback respects the video's actual fps instead of a hardcoded 60.
        DWORD frame_sleep_ms = 16; // 60 fps fallback
        {
            UINT32 fps_num = 0, fps_den = 0;
            if (SUCCEEDED(MFGetAttributeRatio(actual_type.Get(),
                                              MF_MT_FRAME_RATE,
                                              &fps_num, &fps_den)) &&
                fps_num > 0 && fps_den > 0)
            {
                const DWORD ms = static_cast<DWORD>(
                    static_cast<UINT64>(fps_den) * 1000u / fps_num);
                if (ms >= 1u && ms <= 500u)
                    frame_sleep_ms = ms;
            }
        }

        // Read video samples and deliver them at the correct frame rate.
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
                if (loop_)
                {
                    // Rewind the source reader to the start and keep decoding
                    // so fi.mau.loop / fi.mau.gif videos play continuously.
                    PROPVARIANT pos;
                    PropVariantInit(&pos);
                    pos.vt = VT_I8;
                    pos.hVal.QuadPart = 0;
                    HRESULT seek_hr =
                        reader->SetCurrentPosition(GUID_NULL, pos);
                    PropVariantClear(&pos);
                    if (SUCCEEDED(seek_hr))
                    {
                        continue;
                    }
                }
                break;
            }
            if (!sample)
            {
                continue;
            }

            // MFVideoFormat_RGB32 is BGRX; the unused 4th byte is 0x00.
            // make_image_from_bgra marks the resulting D2DImage opaque so D2D
            // ignores the zero alpha.
            //
            // We must repack from MF's (possibly row-padded) buffer into a
            // tightly-packed frame_w*4 buffer.  Using the wrong row pitch shears
            // the image diagonally.
            //
            // Strategy: try IMF2DBuffer::Lock2D() on the sample's primary buffer
            // BEFORE calling ConvertToContiguousBuffer().  ConvertToContiguousBuffer
            // copies the padded data into a new flat MFCreateMemoryBuffer whose
            // IMF2DBuffer::Lock2D() does not carry 2D stride metadata and will
            // fail.  The original output buffer from the video processor MFT does
            // carry correct stride metadata (e.g. 1024 for a 250-px-wide video
            // whose GPU rows are padded to the next 64-byte boundary).

            BYTE* src_ptr   = nullptr;
            LONG  src_pitch = 0; // signed: negative → bottom-up buffer
            bool  locked_2d = false;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            Microsoft::WRL::ComPtr<IMF2DBuffer>    buf2d;

            {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> direct;
                if (SUCCEEDED(sample->GetBufferByIndex(0, direct.GetAddressOf())))
                {
                    Microsoft::WRL::ComPtr<IMF2DBuffer> d2d;
                    if (SUCCEEDED(direct->QueryInterface(d2d.GetAddressOf())))
                    {
                        LONG pitch = 0;
                        if (SUCCEEDED(d2d->Lock2D(&src_ptr, &pitch)))
                        {
                            buf       = std::move(direct);
                            buf2d     = std::move(d2d);
                            src_pitch = pitch;
                            locked_2d = true;
                        }
                    }
                }
            }

            DWORD flat_len = 0; // only valid when locked_2d == false
            if (!locked_2d)
            {
                // GetBufferByIndex + Lock2D failed (common when the video
                // processor outputs a flat MFCreateMemoryBuffer).  Fall back
                // to ConvertToContiguousBuffer + flat lock.
                if (FAILED(sample->ConvertToContiguousBuffer(
                        buf.ReleaseAndGetAddressOf())))
                {
                    continue;
                }
                if (FAILED(buf->Lock(&src_ptr, nullptr, &flat_len)))
                {
                    continue;
                }

                // Derive pitch magnitude from the buffer length.
                const UINT target = frame_w * 4u;
                UINT abs_p = 0;
                if (flat_len % frame_h == 0)
                {
                    // Buffer length is an exact multiple of the frame height:
                    // each row has len/frame_h bytes (may include alignment
                    // padding).
                    abs_p = flat_len / frame_h;
                }
                else
                {
                    // flat_len is NOT divisible by frame_h — the decoder has
                    // added height-alignment padding (e.g. 188 rows padded to
                    // 192 = next multiple of 16).  MF_MT_DEFAULT_STRIDE reports
                    // the unpadded width and is useless here.
                    //
                    // Probe common GPU row-alignment values to find the true
                    // stride: the smallest s >= frame_w*4 such that
                    //   flat_len % s == 0  AND  flat_len / s >= frame_h
                    static const UINT kAligns[] = {
                        64u, 128u, 256u, 512u, 1024u, 2048u, 4096u};
                    for (UINT al : kAligns)
                    {
                        const UINT s = (target + al - 1u) / al * al;
                        if (flat_len % s == 0 &&
                            flat_len / s >= frame_h)
                        {
                            abs_p = s;
                            break;
                        }
                    }
                    if (abs_p == 0)
                    {
                        abs_p = target; // last resort
                    }
                }
                src_pitch = (src_stride < 0) ? -static_cast<LONG>(abs_p)
                                             : static_cast<LONG>(abs_p);
            }

            const UINT dst_stride = frame_w * 4u;
            const UINT abs_pitch  = src_pitch < 0
                                        ? static_cast<UINT>(-src_pitch)
                                        : static_cast<UINT>(src_pitch);

            // Guard: pitch must cover a full output row; for flat-lock also
            // verify the total read stays within the locked buffer.
            const bool pitch_ok =
                abs_pitch >= dst_stride &&
                (locked_2d ||
                 static_cast<uint64_t>(abs_pitch) * frame_h <=
                     static_cast<uint64_t>(flat_len));
            if (!pitch_ok)
            {
                if (locked_2d) buf2d->Unlock2D();
                else           buf->Unlock();
                continue;
            }

            std::vector<uint8_t> pixels(static_cast<size_t>(dst_stride) *
                                        frame_h);
            // Negative pitch → bottom-up buffer: the first byte in memory is
            // the last row of the image, so start at the end and walk back up.
            const BYTE* src_base =
                (src_pitch < 0)
                    ? src_ptr + static_cast<size_t>(abs_pitch) * (frame_h - 1)
                    : src_ptr;
            for (UINT y = 0; y < frame_h; ++y)
            {
                const BYTE* src_row =
                    src_base + static_cast<ptrdiff_t>(y) * src_pitch;
                std::memcpy(pixels.data() +
                                static_cast<size_t>(y) * dst_stride,
                            src_row, dst_stride);
            }

            if (locked_2d) buf2d->Unlock2D();
            else           buf->Unlock();

            if (backend_)
            {
                // Apply stream rotation. disp_w/disp_h track the post-rotation
                // dimensions; frame_w/frame_h stay fixed for the next iteration's
                // buffer allocation.
                UINT32 disp_w = frame_w, disp_h = frame_h;
                if (rotation_deg != 0)
                    rotate_pixels_inplace(pixels, disp_w, disp_h, rotation_deg);

                auto img = tk::d2d::make_image_from_bgra(
                    *backend_, pixels.data(), static_cast<int>(disp_w),
                    static_cast<int>(disp_h));
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

            // Pace the decode thread to the video's actual frame rate.
            // For non-looping videos the audio engine is the reference clock:
            // sleep until the audio position reaches this frame's PTS so
            // video and audio stay in sync.  For looping GIFs (no audio) use
            // the sample's own duration; fall back to the static frame rate.
            {
                DWORD sleep_ms = frame_sleep_ms;

                // Per-sample duration is more accurate than the static rate
                // (handles variable frame-rate content).
                LONGLONG sample_dur = 0;
                if (SUCCEEDED(sample->GetSampleDuration(&sample_dur)) &&
                    sample_dur > 0)
                {
                    const DWORD dur_ms = static_cast<DWORD>(
                        std::min<LONGLONG>(sample_dur / 10000LL, 500LL));
                    if (dur_ms >= 1u)
                        sleep_ms = dur_ms;
                }

                // A/V sync: for non-looping videos with a running audio
                // engine, wait until the audio clock reaches this frame's
                // presentation timestamp.  Guard on audio > 0: the engine is
                // async and GetCurrentTime() returns 0 while it is still
                // buffering.  Without the guard every frame's PTS is compared
                // against 0, producing an ever-growing sleep that makes the
                // video appear to play in slow motion and stalls the progress
                // bar (which also reads GetCurrentTime()).
                if (!loop_.load() && engine_ &&
                    !engine_->IsPaused() && !engine_->IsEnded())
                {
                    const double pts   = static_cast<double>(ts) / 1.0e7;
                    const double audio = engine_->GetCurrentTime();
                    if (audio > 0.0)
                    {
                        const long wait =
                            static_cast<long>((pts - audio) * 1000.0);
                        if (wait > 0 && wait < 2000)
                            sleep_ms = static_cast<DWORD>(wait);
                        else if (wait <= 0)
                            sleep_ms = 0; // video behind audio — display now
                    }
                }

                if (sleep_ms > 0)
                    Sleep(sleep_ms);
            }
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
    std::atomic<bool> loop_{false}; // read by the decode thread
    bool muted_ = false;
    std::string mime_;
    std::vector<uint8_t> bytes_;

    std::atomic<bool> decode_running_{false};
    std::thread decode_thread_;

    mutable std::mutex frame_mutex_;
    mutable std::unique_ptr<tk::Image> current_frame_; // decode thread → UI thread handoff
    mutable std::unique_ptr<tk::Image> display_frame_; // UI thread only — never touched by decode thread
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
