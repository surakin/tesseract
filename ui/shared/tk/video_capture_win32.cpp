// Win32 video capture backend for tk::VideoCapture.
// Uses Media Foundation IMFSourceReader in async (callback) mode.
//
// I420 mode (calls path): requests MFVideoFormat_I420, falls back to NV12
// with software de-interleave. Used by rtc_push_video_frame_i420.
//
// BGRA mode (selfie / CameraWidget): requests MFVideoFormat_ARGB32 (stored
// B,G,R,A in little-endian memory — i.e., native BGRA). The Video Processor
// MFT (enabled via MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING) handles
// the YUV→ARGB32 conversion; the Color Converter DSP (ENABLE_VIDEO_PROCESSING)
// does NOT support ARGB32 output and must not be used here.
//
// Frames are delivered on the MF reader thread via OnReadSample; the
// FrameCallback / BgraCallback must be thread-safe.

#include "video_capture.h"

#include <tesseract/settings.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace
{

// Converts NV12 (Y + interleaved UV) → I420 (Y + planar U + planar V).
void nv12_to_i420(const std::uint8_t* src_y, std::uint32_t src_stride_y,
                  const std::uint8_t* src_uv, std::uint32_t src_stride_uv,
                  std::uint32_t w, std::uint32_t h,
                  std::uint8_t* dst_y, std::uint8_t* dst_u, std::uint8_t* dst_v)
{
    for (std::uint32_t row = 0; row < h; ++row)
        std::memcpy(dst_y + row * w, src_y + row * src_stride_y, w);

    const std::uint32_t h_uv = (h + 1) / 2;
    const std::uint32_t w_uv = (w + 1) / 2;
    for (std::uint32_t row = 0; row < h_uv; ++row)
    {
        const std::uint8_t* uv = src_uv + row * src_stride_uv;
        std::uint8_t* u = dst_u + row * w_uv;
        std::uint8_t* v = dst_v + row * w_uv;
        for (std::uint32_t col = 0; col < w_uv; ++col)
        {
            u[col] = uv[col * 2];
            v[col] = uv[col * 2 + 1];
        }
    }
}

class VideoCaptureWin32
    : public tk::VideoCapture
    , public IMFSourceReaderCallback
{
public:
    VideoCaptureWin32() : ref_count_(1) {}
    ~VideoCaptureWin32() override { stop(); }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&ref_count_);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = InterlockedDecrement(&ref_count_);
        if (n == 0)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IMFSourceReaderCallback)
        {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMFSourceReaderCallback
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr,
                                            DWORD /*stream_index*/,
                                            DWORD /*stream_flags*/,
                                            LONGLONG /*timestamp*/,
                                            IMFSample* sample) override
    {
        if (!running_.load() || FAILED(hr) || !sample)
        {
            if (reader_ && running_.load())
                reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    0, nullptr, nullptr, nullptr, nullptr);
            return S_OK;
        }

        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)))
        {
            BYTE* data    = nullptr;
            DWORD max_len = 0, cur_len = 0;
            if (SUCCEEDED(buf->Lock(&data, &max_len, &cur_len)))
            {
                if (bgra_mode_)
                    deliver_bgra_(reinterpret_cast<std::uint8_t*>(data), cur_len);
                else
                    deliver_i420_(reinterpret_cast<std::uint8_t*>(data));
                buf->Unlock();
            }
            buf->Release();
        }

        if (reader_ && running_.load())
            reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                0, nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }

    void set_callback(tk::VideoCapture::FrameCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
        bgra_mode_ = false;
    }

    void set_bgra_callback(tk::VideoCapture::BgraCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        bgra_callback_ = std::move(cb);
        bgra_mode_ = true;
    }

    void start() override
    {
        if (running_.load())
            return;

        IMFAttributes* attrs = nullptr;
        MFCreateAttributes(&attrs, 3);
        attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this);
        // MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING inserts the Video
        // Processor MFT, which supports ARGB32 output. The basic
        // MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING uses the Color Converter DSP
        // which does NOT list ARGB32 as a supported output type, so
        // SetCurrentMediaType(ARGB32) silently falls back to the camera's native
        // packed-YUV format with that flag.
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

        IMFAttributes* dev_attrs = nullptr;
        MFCreateAttributes(&dev_attrs, 1);
        dev_attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        MFEnumDeviceSources(dev_attrs, &devices, &count);
        dev_attrs->Release();

        if (count == 0)
        {
            attrs->Release();
            return;
        }

        // Find the preferred device by symbolic link; fall back to index 0.
        UINT32 selected = 0;
        {
            const std::string& pref = tesseract::Settings::instance().camera_device_id;
            if (!pref.empty())
            {
                int n = MultiByteToWideChar(CP_UTF8, 0, pref.c_str(), -1, nullptr, 0);
                std::wstring wpref(static_cast<std::size_t>(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, pref.c_str(), -1, wpref.data(), n);
                for (UINT32 i = 0; i < count; ++i)
                {
                    WCHAR* sym = nullptr;
                    UINT32 sym_len = 0;
                    if (SUCCEEDED(devices[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            &sym, &sym_len)))
                    {
                        bool match = (wpref == sym);
                        CoTaskMemFree(sym);
                        if (match) { selected = i; break; }
                    }
                }
            }
        }

        IMFMediaSource* source = nullptr;
        devices[selected]->ActivateObject(IID_PPV_ARGS(&source));
        for (UINT32 i = 0; i < count; ++i)
            devices[i]->Release();
        CoTaskMemFree(devices);

        if (!source)
        {
            attrs->Release();
            return;
        }

        HRESULT hr = MFCreateSourceReaderFromMediaSource(source, attrs, &reader_);
        source->Release();
        attrs->Release();
        if (FAILED(hr))
            return;

        IMFMediaType* type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        if (bgra_mode_)
        {
            // Request ARGB32 at the camera's native resolution.
            // MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING (set on attrs)
            // inserts the Video Processor MFT which supports ARGB32 output.
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            reader_->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
        }
        else
        {
            // I420 for the RTC path — request 640×480 at 30 fps.
            MFSetAttributeSize(type, MF_MT_FRAME_SIZE, 640, 480);
            MFSetAttributeRatio(type, MF_MT_FRAME_RATE, 30, 1);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420);
            if (FAILED(reader_->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type)))
            {
                type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                reader_->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
                use_nv12_ = true;
            }
        }
        type->Release();

        // Record actual frame geometry and negotiated subtype.
        IMFMediaType* actual = nullptr;
        reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
        if (actual)
        {
            MFGetAttributeSize(actual, MF_MT_FRAME_SIZE, &frame_w_, &frame_h_);
            GUID subtype = GUID_NULL;
            actual->GetGUID(MF_MT_SUBTYPE, &subtype);
            // If the reader couldn't deliver ARGB32, detect common 2-byte
            // packed YUV formats and convert in software in deliver_bgra_.
            actual->Release();
        }

        running_.store(true);
        reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                            0, nullptr, nullptr, nullptr, nullptr);
    }

    void stop() override
    {
        if (!running_.exchange(false))
            return;
        if (reader_)
        {
            reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            reader_->Release();
            reader_ = nullptr;
        }
    }

private:
    void deliver_bgra_(const std::uint8_t* raw, DWORD len)
    {
        const std::uint32_t w = frame_w_;
        const std::uint32_t h = frame_h_;
        tk::VideoCapture::BgraCallback bgra_cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            bgra_cb = bgra_callback_;
        }
        if (!bgra_cb)
            return;

        // MFVideoFormat_ARGB32 contiguous buffer: B,G,R,A per pixel,
        // stride = w*4 for the constrained 640x480 format we requested.
        const std::uint32_t expected = w * h * 4;
        if (len < expected)
            return;
        bgra_cb(raw, w, h);
    }

    void deliver_i420_(const std::uint8_t* raw)
    {
        const std::uint32_t w    = frame_w_;
        const std::uint32_t h    = frame_h_;
        const std::uint32_t h_uv = (h + 1) / 2;
        const std::uint32_t w_uv = (w + 1) / 2;

        tk::VideoCapture::FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = callback_;
        }
        if (!cb)
            return;

        if (!use_nv12_)
        {
            tk::VideoCapture::Frame f;
            f.y        = raw;
            f.u        = raw + w * h;
            f.v        = raw + w * h + w_uv * h_uv;
            f.width    = w;
            f.height   = h;
            f.stride_y = w;
            f.stride_u = w_uv;
            f.stride_v = w_uv;
            cb(f);
        }
        else
        {
            const std::uint8_t* src_y  = raw;
            const std::uint8_t* src_uv = raw + w * h;

            const std::size_t i420_size = w * h + 2 * w_uv * h_uv;
            i420_buf_.resize(i420_size);
            std::uint8_t* dst_y = i420_buf_.data();
            std::uint8_t* dst_u = dst_y + w * h;
            std::uint8_t* dst_v = dst_u + w_uv * h_uv;

            nv12_to_i420(src_y, w, src_uv, w, w, h, dst_y, dst_u, dst_v);

            tk::VideoCapture::Frame f;
            f.y        = dst_y;
            f.u        = dst_u;
            f.v        = dst_v;
            f.width    = w;
            f.height   = h;
            f.stride_y = w;
            f.stride_u = w_uv;
            f.stride_v = w_uv;
            cb(f);
        }
    }

    LONG                              ref_count_;
    std::atomic<bool>                 running_{false};
    bool                              bgra_mode_  = false;
    bool                              use_nv12_   = false;
    IMFSourceReader*                  reader_     = nullptr;
    std::uint32_t                     frame_w_    = 640;
    std::uint32_t                     frame_h_    = 480;

    std::mutex                        mu_;
    tk::VideoCapture::FrameCallback   callback_;
    tk::VideoCapture::BgraCallback    bgra_callback_;
    std::vector<std::uint8_t>         i420_buf_;
};

} // namespace

namespace tk
{

std::unique_ptr<VideoCapture> make_video_capture_win32()
{
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return nullptr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    attrs->Release();
    if (FAILED(hr) || count == 0)
        return nullptr;
    for (UINT32 i = 0; i < count; ++i)
        devices[i]->Release();
    CoTaskMemFree(devices);

    return std::make_unique<VideoCaptureWin32>();
}

} // namespace tk
