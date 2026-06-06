// Off-thread GIF-strip MP4 frame extractor — Win32 Media Foundation backend.
//
// Uses IMFSourceReader (synchronous ReadSample loop) — the same mechanism
// already used in video_win32.cpp's streaming decode thread. All COM and MF
// calls happen on the calling thread; CoInitializeEx is invoked internally so
// the caller does not need to initialise COM first.
//
// Pixel repacking follows the same stride/alignment strategy as video_win32.cpp
// to avoid diagonal shearing caused by GPU row-padding.

#include "video_decode.h"

// Keep NOMINMAX / WIN32_LEAN_AND_MEAN consistent with the rest of the Win32
// targets (set by the toolkit's CMake definitions).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <stdint.h>
#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "shlwapi.lib")

// mingw-w64's <mfidl.h> omits this declaration, though the symbol is exported
// by the linked mfplat import library. Declare it so the cross-compile builds;
// the real Windows SDK provides it, so the guard makes this inert there.
#ifdef __MINGW32__
extern "C" HRESULT STDAPICALLTYPE
MFCreateMFByteStreamOnStream(IStream* pStream, IMFByteStream** ppByteStream);
#endif

namespace tk
{

namespace
{
constexpr int kMaxFrames = 300;

// Common GPU row-alignment values used by Media Foundation decoders.
static const UINT kAligns[] = {64u, 128u, 256u, 512u, 1024u, 2048u, 4096u};
} // namespace

DecodedVideoFrames decode_video_frames(const std::uint8_t* data,
                                       std::size_t size,
                                       int max_w, int max_h)
{
    DecodedVideoFrames result;
    if (!data || size == 0)
    {
        return result;
    }

    // COM must be initialised on this thread for MF APIs. Calling with
    // COINIT_MULTITHREADED is safe even if it was already initialised on this
    // thread with the same mode (returns S_FALSE and still functions).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_init_needed = SUCCEEDED(hr);

    auto cleanup_com = [&]()
    {
        if (com_init_needed)
        {
            CoUninitialize();
        }
    };

    // MF must be started per-process; calling again is harmless if already up.
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    // Wrap the raw bytes in an IStream / IMFByteStream.
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(
        reinterpret_cast<const BYTE*>(data), static_cast<UINT>(size)));
    if (!stream)
    {
        MFShutdown();
        cleanup_com();
        return result;
    }

    Microsoft::WRL::ComPtr<IMFByteStream> mf_stream;
    if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(),
                                            mf_stream.GetAddressOf())))
    {
        MFShutdown();
        cleanup_com();
        return result;
    }

    // Enable software video processing (colour conversion, etc.).
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
        MFShutdown();
        cleanup_com();
        return result;
    }

    // Request RGB32 output (BGRX on Windows — alpha byte is 0x00, treated as
    // opaque on the D2D side).
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

    Microsoft::WRL::ComPtr<IMFMediaType> actual_type;
    if (FAILED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            actual_type.GetAddressOf())))
    {
        MFShutdown();
        cleanup_com();
        return result;
    }

    UINT32 frame_w = 0, frame_h = 0;
    MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE, &frame_w, &frame_h);
    if (frame_w == 0 || frame_h == 0)
    {
        MFShutdown();
        cleanup_com();
        return result;
    }

    // Default stride from the media type (signed: negative → bottom-up buffer).
    LONG src_stride = static_cast<LONG>(frame_w) * 4;
    {
        UINT32 s32 = 0;
        if (SUCCEEDED(actual_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s32)))
        {
            src_stride = static_cast<LONG>(static_cast<INT32>(s32));
        }
    }

    // Per-frame delay from the declared frame rate.
    int frame_delay_ms = 33; // 30 fps fallback
    {
        UINT32 fps_num = 0, fps_den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(actual_type.Get(), MF_MT_FRAME_RATE,
                                          &fps_num, &fps_den)) &&
            fps_num > 0 && fps_den > 0)
        {
            const int ms = static_cast<int>(
                static_cast<UINT64>(fps_den) * 1000u / fps_num);
            if (ms >= 1 && ms <= 500)
            {
                frame_delay_ms = ms;
            }
        }
    }

    const UINT dst_stride = frame_w * 4u;

    // Read all frames synchronously.
    while (static_cast<int>(result.frames.size()) < kMaxFrames)
    {
        DWORD stream_idx = 0, flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
            &stream_idx, &flags, &ts, sample.GetAddressOf());

        if (FAILED(hr))
        {
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

        // Try Lock2D on the primary buffer first (preserves original stride
        // metadata). Fall back to ConvertToContiguousBuffer + flat lock.
        BYTE* src_ptr      = nullptr;
        LONG  pitch        = 0;
        bool  locked_2d    = false;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        Microsoft::WRL::ComPtr<IMF2DBuffer>    buf2d;
        {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> direct;
            if (SUCCEEDED(sample->GetBufferByIndex(0, direct.GetAddressOf())))
            {
                Microsoft::WRL::ComPtr<IMF2DBuffer> d2d;
                if (SUCCEEDED(direct->QueryInterface(d2d.GetAddressOf())))
                {
                    if (SUCCEEDED(d2d->Lock2D(&src_ptr, &pitch)))
                    {
                        buf       = std::move(direct);
                        buf2d     = std::move(d2d);
                        locked_2d = true;
                    }
                }
            }
        }

        DWORD flat_len = 0;
        if (!locked_2d)
        {
            if (FAILED(sample->ConvertToContiguousBuffer(
                    buf.ReleaseAndGetAddressOf())))
            {
                continue;
            }
            if (FAILED(buf->Lock(&src_ptr, nullptr, &flat_len)))
            {
                continue;
            }

            // Derive stride from buffer length (same heuristic as video_win32.cpp).
            const UINT target = dst_stride;
            UINT abs_p = 0;
            if (flat_len % frame_h == 0)
            {
                abs_p = flat_len / frame_h;
            }
            else
            {
                for (UINT al : kAligns)
                {
                    const UINT s = (target + al - 1u) / al * al;
                    if (flat_len % s == 0 && flat_len / s >= frame_h)
                    {
                        abs_p = s;
                        break;
                    }
                }
                if (abs_p == 0)
                {
                    abs_p = target;
                }
            }
            pitch = (src_stride < 0) ? -static_cast<LONG>(abs_p)
                                      : static_cast<LONG>(abs_p);
        }

        const UINT abs_pitch = pitch < 0 ? static_cast<UINT>(-pitch)
                                         : static_cast<UINT>(pitch);
        const bool pitch_ok =
            abs_pitch >= dst_stride &&
            (locked_2d ||
             static_cast<std::uint64_t>(abs_pitch) * frame_h <=
                 static_cast<std::uint64_t>(flat_len));
        if (!pitch_ok)
        {
            if (locked_2d) buf2d->Unlock2D();
            else           buf->Unlock();
            continue;
        }

        // Repack into a tightly-packed BGRA row buffer.
        VideoFrame f;
        f.w        = static_cast<int>(frame_w);
        f.h        = static_cast<int>(frame_h);
        f.delay_ms = frame_delay_ms;
        f.bgra.resize(static_cast<std::size_t>(dst_stride) * frame_h);

        const BYTE* src_base = (pitch < 0)
            ? src_ptr + static_cast<std::size_t>(abs_pitch) * (frame_h - 1)
            : src_ptr;
        for (UINT y = 0; y < frame_h; ++y)
        {
            const BYTE* src_row =
                src_base + static_cast<ptrdiff_t>(y) * pitch;
            std::memcpy(f.bgra.data() + static_cast<std::size_t>(y) * dst_stride,
                        src_row, dst_stride);
        }

        if (locked_2d) buf2d->Unlock2D();
        else           buf->Unlock();

        // Scale to the preview cell size so the cached frames stay small.
        result.frames.push_back(downscale_bgra(std::move(f), max_w, max_h));
    }

    MFShutdown();
    cleanup_com();
    return result;
}

} // namespace tk
