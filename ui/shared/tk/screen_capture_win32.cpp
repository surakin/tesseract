// Win32 screen capture backend for tk::ScreenCapture.
// Uses DXGI Desktop Duplication (IDXGIOutputDuplication) for full-monitor
// capture, and PrintWindow + DIB section for application window capture.
//
// Frames are captured at ~15 fps and delivered as I420 on a background thread.
// The BGRA output from DXGI is converted to I420 with a plain BT.601 loop.
//
// Permission model: DXGI Desktop Duplication requires no special privilege for
// the current user's desktop. Protected/elevated windows (UAC dialogs) appear
// black — this is a Windows security constraint, not a bug.
#ifdef TESSERACT_CALLS_ENABLED

#include "screen_capture.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{

/// Software BGRA (B,G,R,A byte order) → I420 conversion using BT.601 full-range.
/// dst_y/dst_u/dst_v must be pre-allocated to exactly w*h, w_uv*h_uv each.
void bgra_to_i420(const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h,
                  std::uint8_t* dst_y, std::uint8_t* dst_u, std::uint8_t* dst_v,
                  std::uint32_t src_stride = 0)
{
    if (src_stride == 0)
        src_stride = w * 4;
    const std::uint32_t w_uv = (w + 1) / 2;
    for (std::uint32_t row = 0; row < h; ++row)
    {
        const std::uint8_t* src = bgra + row * src_stride;
        std::uint8_t*       dy  = dst_y + row * w;
        for (std::uint32_t col = 0; col < w; ++col, src += 4)
        {
            const int b = src[0], g = src[1], r = src[2];
            dy[col] = static_cast<std::uint8_t>(
                ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (std::uint32_t row = 0; row < h; row += 2)
    {
        const std::uint8_t* src = bgra + row * src_stride;
        std::uint8_t*       du  = dst_u + (row / 2) * w_uv;
        std::uint8_t*       dv  = dst_v + (row / 2) * w_uv;
        for (std::uint32_t col = 0; col < w; col += 2, src += 8)
        {
            const int b = src[0], g = src[1], r = src[2];
            du[col / 2] = static_cast<std::uint8_t>(
                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dv[col / 2] = static_cast<std::uint8_t>(
                ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

class ScreenCaptureWin32 : public tk::ScreenCapture
{
public:
    ~ScreenCaptureWin32() override { stop(); }

    std::vector<tk::ScreenSource> enumerate_sources() override
    {
        std::vector<tk::ScreenSource> sources;
        // Enumerate monitors via DXGI adapter output enumeration.
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return sources;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai)
        {
            IDXGIOutput* output = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi)
            {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)))
                {
                    int n = WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                                               nullptr, 0, nullptr, nullptr);
                    std::string name(static_cast<std::size_t>(n - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                                       name.data(), n, nullptr, nullptr);
                    // id = "monitor:<adapter>:<output>"
                    std::string id = "monitor:" + std::to_string(ai) + ":" + std::to_string(oi);
                    sources.push_back({id, "Display " + std::to_string(sources.size() + 1), false});
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();

        // Enumerate visible, non-minimized top-level windows.
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL
            {
                auto* list = reinterpret_cast<std::vector<tk::ScreenSource>*>(lp);
                if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
                    return TRUE;
                WCHAR title[256] = {};
                GetWindowTextW(hwnd, title, 256);
                if (title[0] == L'\0')
                    return TRUE;
                // Skip taskbar to avoid self-capture confusion.
                WCHAR cls[64] = {};
                GetClassNameW(hwnd, cls, 64);
                if (wcscmp(cls, L"Shell_TrayWnd") == 0)
                    return TRUE;
                int n = WideCharToMultiByte(CP_UTF8, 0, title, -1,
                                            nullptr, 0, nullptr, nullptr);
                std::string name(static_cast<std::size_t>(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, title, -1,
                                    name.data(), n, nullptr, nullptr);
                std::string id = "window:" + std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));
                list->push_back({id, name, true});
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&sources));
        return sources;
    }

    void set_source(const std::string& source_id) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_id_ = source_id;
    }

    void set_callback(tk::ScreenCapture::FrameCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
    }

    void start() override
    {
        if (running_.load())
            return;
        running_.store(true);
        thread_ = std::thread([this] { capture_loop_(); });
    }

    void stop() override
    {
        if (!running_.exchange(false))
            return;
        if (thread_.joinable())
            thread_.join();
    }

private:
    void capture_loop_()
    {
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sid = source_id_;
        }

        // Parse source id to determine monitor vs window.
        if (sid.rfind("window:", 0) == 0)
        {
            capture_window_loop_(sid);
            return;
        }

        // Default: capture first monitor (or the one specified).
        UINT adapter_idx = 0, output_idx = 0;
        if (sid.rfind("monitor:", 0) == 0)
        {
            // "monitor:<ai>:<oi>"
            auto rest = sid.substr(8);
            auto colon = rest.find(':');
            if (colon != std::string::npos)
            {
                adapter_idx = static_cast<UINT>(std::stoul(rest.substr(0, colon)));
                output_idx  = static_cast<UINT>(std::stoul(rest.substr(colon + 1)));
            }
        }

        // Create D3D11 device.
        ID3D11Device*        d3d_dev  = nullptr;
        ID3D11DeviceContext* d3d_ctx  = nullptr;
        D3D_FEATURE_LEVEL    feature_level;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &d3d_dev, &feature_level, &d3d_ctx)))
            return;

        IDXGIDevice*  dxgi_dev     = nullptr;
        IDXGIAdapter* dxgi_adapter = nullptr;
        IDXGIOutput*  dxgi_output  = nullptr;
        IDXGIOutput1* dxgi_out1    = nullptr;
        IDXGIOutputDuplication* duplication = nullptr;

        d3d_dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
        dxgi_dev->GetAdapter(&dxgi_adapter);

        // Advance to the requested adapter/output.
        IDXGIFactory1* factory = nullptr;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        IDXGIAdapter1* target_adapter = nullptr;
        factory->EnumAdapters1(adapter_idx, &target_adapter);
        factory->Release();
        if (target_adapter)
        {
            target_adapter->EnumOutputs(output_idx, &dxgi_output);
            target_adapter->Release();
        }
        else
        {
            dxgi_adapter->EnumOutputs(0, &dxgi_output);
        }

        if (!dxgi_output ||
            FAILED(dxgi_output->QueryInterface(IID_PPV_ARGS(&dxgi_out1))) ||
            FAILED(dxgi_out1->DuplicateOutput(d3d_dev, &duplication)))
        {
            if (dxgi_out1)    dxgi_out1->Release();
            if (dxgi_output)  dxgi_output->Release();
            if (dxgi_adapter) dxgi_adapter->Release();
            if (dxgi_dev)     dxgi_dev->Release();
            d3d_ctx->Release();
            d3d_dev->Release();
            return;
        }

        DXGI_OUTDUPL_DESC dup_desc{};
        duplication->GetDesc(&dup_desc);
        const UINT w = dup_desc.ModeDesc.Width;
        const UINT h = dup_desc.ModeDesc.Height;
        const UINT w_uv = (w + 1) / 2;
        const UINT h_uv = (h + 1) / 2;
        std::vector<std::uint8_t> bgra_buf(w * h * 4);
        std::vector<std::uint8_t> i420_buf(w * h + 2 * w_uv * h_uv);
        std::uint8_t* dst_y = i420_buf.data();
        std::uint8_t* dst_u = dst_y + w * h;
        std::uint8_t* dst_v = dst_u + w_uv * h_uv;

        // Create a staging texture for CPU readback.
        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width  = w;
        tex_desc.Height = h;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(d3d_dev->CreateTexture2D(&tex_desc, nullptr, &staging)))
        {
            duplication->Release();
            dxgi_out1->Release();
            dxgi_output->Release();
            dxgi_adapter->Release();
            dxgi_dev->Release();
            d3d_ctx->Release();
            d3d_dev->Release();
            return;
        }

        // ~15 fps capture loop.
        constexpr DWORD frame_ms = 67;
        while (running_.load())
        {
            IDXGIResource*       res   = nullptr;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            HRESULT hr = duplication->AcquireNextFrame(frame_ms, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                continue;
            if (FAILED(hr))
                break;

            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))))
            {
                d3d_ctx->CopyResource(staging, tex);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(d3d_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    const auto* src = reinterpret_cast<const std::uint8_t*>(mapped.pData);
                    for (UINT row = 0; row < h; ++row)
                        std::memcpy(bgra_buf.data() + row * w * 4,
                                    src + row * mapped.RowPitch, w * 4);
                    d3d_ctx->Unmap(staging, 0);
                    bgra_to_i420(bgra_buf.data(), w, h, dst_y, dst_u, dst_v);
                    tk::ScreenCapture::FrameCallback cb;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        cb = callback_;
                    }
                    if (cb)
                    {
                        tk::ScreenCapture::Frame f;
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
                tex->Release();
            }
            if (res) res->Release();
            duplication->ReleaseFrame();
        }

        staging->Release();
        duplication->Release();
        dxgi_out1->Release();
        dxgi_output->Release();
        dxgi_adapter->Release();
        dxgi_dev->Release();
        d3d_ctx->Release();
        d3d_dev->Release();
    }

    void capture_window_loop_(const std::string& sid)
    {
        // Parse HWND from "window:<decimal_ptr>"
        HWND hwnd = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(std::stoull(sid.substr(7))));
        if (!IsWindow(hwnd))
            return;

        constexpr DWORD frame_ms = 67;
        while (running_.load())
        {
            if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
            {
                Sleep(frame_ms);
                continue;
            }
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const UINT w = static_cast<UINT>(rc.right - rc.left);
            const UINT h = static_cast<UINT>(rc.bottom - rc.top);
            if (w == 0 || h == 0)
            {
                Sleep(frame_ms);
                continue;
            }
            const UINT w_uv = (w + 1) / 2;
            const UINT h_uv = (h + 1) / 2;

            HDC  hdc_win = GetDC(hwnd);
            HDC  hdc_mem = CreateCompatibleDC(hdc_win);
            HBITMAP bmp  = CreateCompatibleBitmap(hdc_win, static_cast<int>(w), static_cast<int>(h));
            SelectObject(hdc_mem, bmp);
            PrintWindow(hwnd, hdc_mem, PW_CLIENTONLY);

            BITMAPINFOHEADER bih{};
            bih.biSize        = sizeof(bih);
            bih.biWidth       = static_cast<LONG>(w);
            bih.biHeight      = -static_cast<LONG>(h); // top-down
            bih.biPlanes      = 1;
            bih.biBitCount    = 32;
            bih.biCompression = BI_RGB;

            std::vector<std::uint8_t> bgra_buf(w * h * 4);
            GetDIBits(hdc_mem, bmp, 0, h, bgra_buf.data(),
                      reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);

            std::vector<std::uint8_t> i420_buf(w * h + 2 * w_uv * h_uv);
            std::uint8_t* dst_y = i420_buf.data();
            std::uint8_t* dst_u = dst_y + w * h;
            std::uint8_t* dst_v = dst_u + w_uv * h_uv;
            bgra_to_i420(bgra_buf.data(), w, h, dst_y, dst_u, dst_v);

            tk::ScreenCapture::FrameCallback cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = callback_;
            }
            if (cb)
            {
                tk::ScreenCapture::Frame f;
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

            DeleteObject(bmp);
            DeleteDC(hdc_mem);
            ReleaseDC(hwnd, hdc_win);
            Sleep(frame_ms);
        }
    }

    std::atomic<bool>                     running_{false};
    std::thread                           thread_;
    std::mutex                            mu_;
    std::string                           source_id_;
    tk::ScreenCapture::FrameCallback      callback_;
};

} // namespace

namespace tk
{

std::unique_ptr<ScreenCapture> make_screen_capture_win32()
{
    // Require DXGI 1.2 (Win8+): verify IDXGIOutput1 is accessible.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;
    IDXGIAdapter1* adapter = nullptr;
    bool have_output1 = false;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
    {
        IDXGIOutput* out = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(0, &out)))
        {
            IDXGIOutput1* out1 = nullptr;
            have_output1 = SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out1)));
            if (out1) out1->Release();
            out->Release();
        }
        adapter->Release();
    }
    factory->Release();
    if (!have_output1)
        return nullptr;
    return std::make_unique<ScreenCaptureWin32>();
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
