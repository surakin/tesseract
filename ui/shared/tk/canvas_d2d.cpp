#include "canvas_d2d.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

// Matches IDR_EMOJI_FONT in ui/windows/src/resource.h
static constexpr int kEmojiFontResourceId = 201;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tk::d2d
{

// Points added on top of the Windows system UI font size. The Win32 system
// font (lfMessageFont) is Segoe UI 9pt — the classic desktop default used by
// Explorer, dialogs and menus. That reads small next to modern chat clients,
// whose body text sits around 11pt (WinUI body is ~10.5pt; Electron apps such
// as Slack/Discord/VS Code default to ~14-15px ≈ 10.5-11pt). This offset nudges
// Tesseract into that range. Because every font role is an additive offset from
// the body base (see font_role_pt), bumping the base scales the whole UI.
// Set to 0 to track the system font exactly.
static constexpr int kBodyFontPtOffset = 2;

// Returns the body font size in pt: the Windows system font from
// SPI_GETNONCLIENTMETRICS (which reflects Accessibility → Text size) plus
// kBodyFontPtOffset. Cached on first call.
int win32_system_base_pt()
{
    static int base = []() -> int {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return 9;
        int h = ncm.lfMessageFont.lfHeight;
        if (h == 0) return 9;
        // lfHeight is in device pixels at the system DPI. Dividing by 96
        // instead of the real DPI double-counts the scale on HiDPI displays.
        HDC screen = GetDC(nullptr);
        const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
        if (screen) ReleaseDC(nullptr, screen);
        int pt = MulDiv(std::abs(h), 72, dpi);
        return pt > 0 ? pt : 9;
    }();
    return base + kBodyFontPtOffset;
}

namespace
{

// ── Small helpers ─────────────────────────────────────────────────────────

inline D2D1_COLOR_F to_d2d(Color c)
{
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

inline D2D1_RECT_F to_d2d(Rect r)
{
    return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
}

inline D2D1_POINT_2F to_d2d(Point p)
{
    return D2D1::Point2F(p.x, p.y);
}

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::string("d2d: ") + what + " failed");
    }
}

std::wstring utf8_to_wide(std::string_view s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// Extract the first 1–2 grapheme starts for an initials disc. UTF-8 aware
// at the byte level: a new character begins on any byte that is not a
// continuation byte (0b10xxxxxx). Two-letter initials are picked from
// the first word + the first letter of the next word; otherwise one
// letter from the first word.
// The word-split policy is shared (tk::initials_of); apply Win32's
// locale-aware uppercasing (towupper) to the result before drawing.
std::wstring initials_upper(std::string_view name)
{
    std::wstring out = utf8_to_wide(initials_of(name));
    for (wchar_t& ch : out)
    {
        ch = static_cast<wchar_t>(towupper(ch));
    }
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Font roles → DWrite text format
// ─────────────────────────────────────────────────────────────────────────
//
// Sizes follow client/include/tesseract/visual.h. Weight follows the
// "semibold for emphasis" rule in docs/UI-PARITY.md. The family is the
// modern Segoe UI Variable face on Win11; older systems fall back to
// "Segoe UI" via the system font fallback chain that DWrite already
// applies when the requested family is unavailable.

struct FontDesc
{
    const wchar_t* family;
    float size_pt;
    DWRITE_FONT_WEIGHT weight;
};

static FontDesc desc_for(FontRole r)
{
    const float pt = static_cast<float>(font_role_pt(r, win32_system_base_pt()));
    const DWRITE_FONT_WEIGHT weight = font_role_is_semibold(r)
                                          ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                          : DWRITE_FONT_WEIGHT_REGULAR;
    // Title uses the Display optical face; all other roles use Text.
    const wchar_t* family = (r == FontRole::Title)
                                ? L"Segoe UI Variable Display"
                                : L"Segoe UI Variable Text";
    return {family, pt, weight};
}

// ─────────────────────────────────────────────────────────────────────────
//  Backend::Impl — D2D + DWrite + WIC singletons
// ─────────────────────────────────────────────────────────────────────────

struct Backend::Impl
{
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<IDWriteFactory2> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFontFallback> font_fallback;
    ComPtr<IDWriteFontFileLoader> mem_font_loader; // keeps Twemoji alive
    ComPtr<IDWriteFontFace> noto_emoji_face;        // Noto Color Emoji IDWriteFontFace
    bool com_initialised_here = false;

    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice1> dxgi_dev;
    ComPtr<ID2D1Device> d2d_dev;
    bool device_lost_ = false;

    std::unordered_map<int, ComPtr<IDWriteTextFormat>> text_formats;

    void create_d3d_device()
    {
        d2d_dev.Reset();
        dxgi_dev.Reset();
        d3d.Reset();
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            d3d.GetAddressOf(), nullptr, nullptr);
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d.GetAddressOf(),
                                   nullptr, nullptr);
            check(hr, "D3D11CreateDevice");
        }
        check(d3d->QueryInterface(IID_PPV_ARGS(dxgi_dev.GetAddressOf())),
              "QI IDXGIDevice1");
        check(d2d->CreateDevice(dxgi_dev.Get(), d2d_dev.GetAddressOf()),
              "ID2D1Factory1::CreateDevice");
        device_lost_ = false;
    }

    IDWriteTextFormat* text_format_for(FontRole role)
    {
        auto it = text_formats.find(static_cast<int>(role));
        if (it != text_formats.end())
        {
            return it->second.Get();
        }

        FontDesc fd = desc_for(role);
        ComPtr<IDWriteTextFormat> tf;
        // 1 pt = 1/72 inch; DWrite measures size in DIPs (1 DIP = 1/96 inch),
        // so multiply by 96/72 = 4/3 to convert.
        float size_dip = fd.size_pt * (96.0f / 72.0f);
        HRESULT hr = dwrite->CreateTextFormat(
            fd.family, nullptr, fd.weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size_dip, L"", tf.GetAddressOf());
        check(hr, "CreateTextFormat");
        text_formats.emplace(static_cast<int>(role), tf);
        return tf.Get();
    }
};

// Loads the Noto Color Emoji TTF from the embedded RCDATA resource and builds
// an IDWriteFontFallback that maps all major Unicode emoji blocks to Noto Color
// Emoji, then chains the system fallback for everything else.
// Returns nullptr (and leaves mem_loader_out untouched) on any failure — the
// caller then keeps using the system-only fallback.
static ComPtr<IDWriteFontFallback>
build_emoji_fallback(ComPtr<IDWriteFactory2>& dwrite,
                     ComPtr<IDWriteFontFallback> system_fallback,
                     ComPtr<IDWriteFontFileLoader>& mem_loader_out,
                     ComPtr<IDWriteFontFace>& face_out)
{
    // 1. Read font bytes from the embedded RCDATA resource.
    HMODULE hmod = GetModuleHandleW(nullptr);
    HRSRC hrsrc = FindResourceW(hmod, MAKEINTRESOURCEW(kEmojiFontResourceId),
                                RT_RCDATA);
    if (!hrsrc)
    {
        return nullptr;
    }
    HGLOBAL hglob = LoadResource(hmod, hrsrc);
    if (!hglob)
    {
        return nullptr;
    }
    const void* data = LockResource(hglob);
    DWORD size = SizeofResource(hmod, hrsrc);
    if (!data || !size)
    {
        return nullptr;
    }

    // 2. QI to IDWriteFactory5 (Win10 1709+) for CreateInMemoryFontFileLoader.
    //    Falls back to system-only fallback on older builds.
    ComPtr<IDWriteFactory5> dwrite5;
    if (FAILED(dwrite.As(&dwrite5)))
    {
        return nullptr;
    }

    // 3. Register an in-memory font file loader and create a file reference.
    ComPtr<IDWriteInMemoryFontFileLoader> mem_loader;
    if (FAILED(dwrite5->CreateInMemoryFontFileLoader(&mem_loader)))
    {
        return nullptr;
    }
    if (FAILED(dwrite5->RegisterFontFileLoader(mem_loader.Get())))
    {
        return nullptr;
    }

    ComPtr<IDWriteFontFile> font_file;
    if (FAILED(mem_loader->CreateInMemoryFontFileReference(
            dwrite5.Get(), data, size, nullptr, &font_file)))
    {
        return nullptr;
    }

    // 4. Build a font set containing just Twemoji, then a font collection.
    //    IDWriteFactory5 inherits IDWriteFactory3, so we use it directly.
    ComPtr<IDWriteFontFaceReference> face_ref;
#ifdef __MINGW32__
    // MinGW renames the IDWriteFontFile* overload with a trailing underscore to
    // avoid C++ ambiguity with the path-string overload; MSVC uses overloading.
    if (FAILED(dwrite5->CreateFontFaceReference_(
            font_file.Get(), 0, DWRITE_FONT_SIMULATIONS_NONE, &face_ref)))
#else
    if (FAILED(dwrite5->CreateFontFaceReference(
            font_file.Get(), 0, DWRITE_FONT_SIMULATIONS_NONE, &face_ref)))
#endif
    {
        return nullptr;
    }

    // MinGW's IDWriteFactory5::CreateFontSetBuilder takes IDWriteFontSetBuilder1**
    // (matching a newer SDK revision); MSVC inherits the IDWriteFactory3 version
    // that takes IDWriteFontSetBuilder**. IDWriteFontSetBuilder1 : IDWriteFontSetBuilder,
    // so AddFontFaceReference / CreateFontSet are available on both paths.
#ifdef __MINGW32__
    ComPtr<IDWriteFontSetBuilder1> set_builder;
#else
    ComPtr<IDWriteFontSetBuilder> set_builder;
#endif
    if (FAILED(dwrite5->CreateFontSetBuilder(&set_builder)))
    {
        return nullptr;
    }
    if (FAILED(set_builder->AddFontFaceReference(face_ref.Get())))
    {
        return nullptr;
    }

    ComPtr<IDWriteFontSet> font_set;
    if (FAILED(set_builder->CreateFontSet(&font_set)))
    {
        return nullptr;
    }

    ComPtr<IDWriteFontCollection1> emoji_coll;
    if (FAILED(dwrite5->CreateFontCollectionFromFontSet(font_set.Get(),
                                                        &emoji_coll)))
    {
        return nullptr;
    }

    // Extract an IDWriteFontFace so the windowless RichEdit host can supply it
    // via IProvideFontInfo::GetFontFace to route emoji text runs to Noto Color
    // Emoji instead of the system Segoe UI Emoji.  Non-fatal: if this fails,
    // face_out is left null and the fallback still works for canvas rendering.
    {
        ComPtr<IDWriteFontFamily1> emoji_family;
        if (SUCCEEDED(emoji_coll->GetFontFamily(0, &emoji_family)))
        {
            ComPtr<IDWriteFont> emoji_font;
            if (SUCCEEDED(emoji_family->GetFirstMatchingFont(
                    DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, &emoji_font)))
            {
                emoji_font->CreateFontFace(&face_out);
            }
        }
    }

    // 5. Build a fallback: all emoji blocks → Noto Color Emoji, everything else → system.
    // DWrite falls through to the next mapping for any codepoint Noto lacks.
    static const DWRITE_UNICODE_RANGE kEmojiRanges[] = {
        // Scattered BMP emoji (©, ®, ™, arrows, clocks, symbols, dingbats …)
        {0x00A9, 0x00A9},  // ©
        {0x00AE, 0x00AE},  // ®
        {0x203C, 0x203C},  // ‼
        {0x2049, 0x2049},  // ⁉
        {0x20E3, 0x20E3},  // ⃣ combining enclosing keycap
        {0x2122, 0x2122},  // ™
        {0x2139, 0x2139},  // ℹ
        {0x2194, 0x21AA},  // various arrows
        {0x231A, 0x231B},  // watch, hourglass
        {0x23CF, 0x23CF},  // eject
        {0x23E9, 0x23F3},  // rewind / fast-forward / clocks
        {0x23F8, 0x23FA},  // pause / stop / record
        {0x24C2, 0x24C2},  // Ⓜ
        {0x25AA, 0x25AB},  // small black/white squares
        {0x25B6, 0x25B6},  // ▶ play
        {0x25C0, 0x25C0},  // ◀ reverse
        {0x25FB, 0x25FE},  // medium squares
        {0x2600, 0x2604},  // sun, cloud, etc.
        {0x260E, 0x260E},  // ☎
        {0x2611, 0x2611},  // ☑
        {0x2614, 0x2615},  // umbrella, hot beverage
        {0x2618, 0x2618},  // shamrock
        {0x261D, 0x261D},  // index pointing up
        {0x2620, 0x2620},  // skull and crossbones
        {0x2622, 0x2623},  // radioactive, biohazard
        {0x2626, 0x2626},  // orthodox cross
        {0x262A, 0x262A},  // star and crescent
        {0x262E, 0x262F},  // peace, yin yang
        {0x2638, 0x263A},  // wheel of dharma, smiley
        {0x2640, 0x2640},  // female sign
        {0x2642, 0x2642},  // male sign
        {0x2648, 0x2653},  // zodiac signs
        {0x265F, 0x2660},  // chess pawn, spade
        {0x2663, 0x2663},  // club
        {0x2665, 0x2666},  // heart, diamond
        {0x2668, 0x2668},  // hot springs
        {0x267B, 0x267B},  // recycling
        {0x267E, 0x267F},  // infinity, wheelchair
        {0x2692, 0x2697},  // tools
        {0x2699, 0x2699},  // gear
        {0x269B, 0x269C},  // atom, fleur-de-lis
        {0x26A0, 0x26A1},  // warning, lightning
        {0x26A7, 0x26A7},  // male-female sign
        {0x26AA, 0x26AB},  // circles
        {0x26B0, 0x26B1},  // coffin, funeral urn
        {0x26BD, 0x26BE},  // soccer, baseball
        {0x26C4, 0x26C5},  // snowman, sun behind cloud
        {0x26CE, 0x26CF},  // ophiuchus, pick
        {0x26D1, 0x26D1},  // rescue worker helmet
        {0x26D3, 0x26D4},  // chains, no entry
        {0x26E9, 0x26EA},  // shinto shrine, church
        {0x26F0, 0x26F5},  // mountain, camping, etc.
        {0x26F7, 0x26FA},  // skier, etc.
        {0x26FD, 0x26FD},  // fuel pump
        {0x2702, 0x2702},  // scissors
        {0x2705, 0x2705},  // check mark (green)
        {0x2708, 0x270D},  // plane, umbrella, pencil, hand
        {0x270F, 0x270F},  // pencil
        {0x2712, 0x2712},  // black nib
        {0x2714, 0x2714},  // heavy check mark
        {0x2716, 0x2716},  // heavy ✖
        {0x271D, 0x271D},  // latin cross
        {0x2721, 0x2721},  // star of david
        {0x2728, 0x2728},  // sparkles
        {0x2733, 0x2734},  // asterisks
        {0x2744, 0x2744},  // snowflake
        {0x2747, 0x2747},  // sparkle
        {0x274C, 0x274C},  // cross mark
        {0x274E, 0x274E},  // cross mark
        {0x2753, 0x2755},  // question / exclamation ornaments
        {0x2757, 0x2757},  // exclamation mark
        {0x2763, 0x2764},  // hearts
        {0x2795, 0x2797},  // plus, minus, divide
        {0x27A1, 0x27A1},  // right arrow
        {0x27B0, 0x27B0},  // curly loop
        {0x27BF, 0x27BF},  // double curly loop
        {0x2934, 0x2935},  // curved arrows
        {0x2B05, 0x2B07},  // directional arrows
        {0x2B1B, 0x2B1C},  // black/white squares
        {0x2B50, 0x2B50},  // star
        {0x2B55, 0x2B55},  // hollow red circle
        {0x3030, 0x3030},  // wavy dash
        {0x303D, 0x303D},  // part alternation mark
        {0x3297, 0x3297},  // circled ideograph congratulation
        {0x3299, 0x3299},  // circled ideograph secret
        // Supplementary Multilingual Plane — all major emoji blocks
        {0x1F004, 0x1F004}, // mahjong red dragon
        {0x1F0CF, 0x1F0CF}, // playing card black joker
        {0x1F170, 0x1F171}, // A/B button (blood type)
        {0x1F17E, 0x1F17F}, // O/P button
        {0x1F18E, 0x1F18E}, // AB button
        {0x1F191, 0x1F19A}, // squared letters
        {0x1F1E0, 0x1F1FF}, // regional indicators (country flags)
        {0x1F201, 0x1F202}, // Japanese buttons
        {0x1F21A, 0x1F21A}, // free button
        {0x1F22F, 0x1F22F}, // reserved button
        {0x1F232, 0x1F23A}, // Japanese buttons
        {0x1F250, 0x1F251}, // Japanese buttons
        {0x1F300, 0x1F6FF}, // Misc Symbols & Pictographs, Emoticons, Transport…
        {0x1F7E0, 0x1F7FF}, // Geometric Shapes Extended (colored circles/squares)
        {0x1F900, 0x1F9FF}, // Supplemental Symbols and Pictographs
        {0x1FA00, 0x1FA6F}, // Chess Symbols
        {0x1FA70, 0x1FAFF}, // Symbols and Pictographs Extended-A
    };
    const wchar_t* family[] = {L"Noto Color Emoji"};

    ComPtr<IDWriteFontFallbackBuilder> fb_builder;
    if (FAILED(dwrite->CreateFontFallbackBuilder(&fb_builder)))
    {
        return nullptr;
    }
    if (FAILED(fb_builder->AddMapping(kEmojiRanges, ARRAYSIZE(kEmojiRanges),
                                      family, 1, emoji_coll.Get(), nullptr,
                                      nullptr, 1.0f)))
    {
        return nullptr;
    }
    // Chain the system fallback (Segoe UI Emoji etc.) for all other characters.
    if (FAILED(fb_builder->AddMappings(system_fallback.Get())))
    {
        return nullptr;
    }

    ComPtr<IDWriteFontFallback> result;
    if (FAILED(fb_builder->CreateFontFallback(&result)))
    {
        return nullptr;
    }

    mem_loader_out = std::move(mem_loader);
    return result;
}

Backend::Backend() : impl_(std::make_unique<Impl>())
{
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr_co))
    {
        impl_->com_initialised_here = true;
    }
    else if (hr_co != RPC_E_CHANGED_MODE && hr_co != S_FALSE)
    {
        check(hr_co, "CoInitializeEx");
    }

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
        reinterpret_cast<void**>(impl_->d2d.GetAddressOf()));
    check(hr, "D2D1CreateFactory");

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(impl_->dwrite.GetAddressOf()));
    check(hr, "DWriteCreateFactory");
    impl_->dwrite->GetSystemFontFallback(impl_->font_fallback.GetAddressOf());
    if (auto emoji = build_emoji_fallback(
            impl_->dwrite, impl_->font_fallback, impl_->mem_font_loader,
            impl_->noto_emoji_face))
    {
        impl_->font_fallback = std::move(emoji);
    }

    hr =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(impl_->wic.GetAddressOf()));
    check(hr, "WICImagingFactory");

    impl_->create_d3d_device();
}

Backend::~Backend()
{
    impl_->text_formats.clear();
    impl_->wic.Reset();
    // Release the Noto font face before unregistering the loader that backs it.
    impl_->noto_emoji_face.Reset();
    if (impl_->mem_font_loader)
    {
        impl_->dwrite->UnregisterFontFileLoader(impl_->mem_font_loader.Get());
    }
    impl_->mem_font_loader.Reset();
    impl_->dwrite.Reset();
    impl_->d2d_dev.Reset();
    impl_->dxgi_dev.Reset();
    impl_->d3d.Reset();
    impl_->d2d.Reset();
    if (impl_->com_initialised_here)
    {
        CoUninitialize();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  D2DImage — tk::Image
// ─────────────────────────────────────────────────────────────────────────

class D2DImage : public Image
{
public:
    D2DImage(ComPtr<IWICBitmap> source, int w, int h, bool opaque = false)
        : source_(std::move(source)), width_(w), height_(h), opaque_(opaque)
    {
    }

    int width() const override
    {
        return width_;
    }
    int height() const override
    {
        return height_;
    }

    std::size_t memory_bytes() const override
    {
        std::size_t bytes = 0;
        bool counted = false;
        if (source_)
        {
            WICRect rc{0, 0, width_, height_};
            ComPtr<IWICBitmapLock> lock;
            if (SUCCEEDED(source_->Lock(&rc, WICBitmapLockRead,
                                        lock.GetAddressOf())))
            {
                UINT stride = 0;
                if (SUCCEEDED(lock->GetStride(&stride)))
                {
                    bytes += static_cast<std::size_t>(stride) *
                             static_cast<std::size_t>(height_);
                    counted = true;
                }
            }
        }
        if (!counted)
        {
            bytes += static_cast<std::size_t>(width_) *
                     static_cast<std::size_t>(height_) * 4u;
        }
        // Count the per-render-target GPU copy too, when one is materialized.
        if (bitmap_)
        {
            const D2D1_SIZE_U px = bitmap_->GetPixelSize();
            bytes += static_cast<std::size_t>(px.width) *
                     static_cast<std::size_t>(px.height) * 4u;
        }
        return bytes;
    }

    // ID2D1Bitmap is bound to a particular ID2D1RenderTarget. We keep the
    // decoded pixels as the source of truth (in an in-memory IWICBitmap)
    // and lazily construct the D2D bitmap for the render target that
    // draws us. The cache is invalidated when the render target is
    // recreated (lost device).
    ID2D1Bitmap* bitmap_for(ID2D1RenderTarget* rt)
    {
        if (bitmap_ && rt_cached_ == rt)
        {
            return bitmap_.Get();
        }
        bitmap_.Reset();
        rt_cached_ = nullptr;
        HRESULT hr;
        if (opaque_)
        {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            hr = rt->CreateBitmapFromWicBitmap(source_.Get(), &props,
                                               bitmap_.GetAddressOf());
        }
        else
        {
            hr = rt->CreateBitmapFromWicBitmap(source_.Get(), nullptr,
                                               bitmap_.GetAddressOf());
        }
        if (FAILED(hr))
        {
            return nullptr;
        }
        rt_cached_ = rt;
        return bitmap_.Get();
    }

    void release_device_resources()
    {
        bitmap_.Reset();
        rt_cached_ = nullptr;
    }

private:
    // IWICBitmap owns its pixel buffer, so the image survives the input
    // byte span used at decode time. IWICStream::InitializeFromMemory
    // does NOT copy its input — keeping a format-converter / frame-decode
    // chain as the source would leave WIC dereferencing freed memory at
    // paint time, which is what crashed the Win32 build previously.
    ComPtr<IWICBitmap> source_;
    int width_;
    int height_;
    bool opaque_ = false;
    ComPtr<ID2D1Bitmap> bitmap_;
    ID2D1RenderTarget* rt_cached_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────
//  DWriteLayout — tk::TextLayout
// ─────────────────────────────────────────────────────────────────────────

class DWriteLayout : public TextLayout
{
public:
    struct UrlRange
    {
        UINT32 start, end;
        std::string url;
    };

    struct ColorRange
    {
        UINT32 start, end;
        Color  color;
    };

    DWriteLayout(ComPtr<IDWriteTextLayout> layout,
                 std::wstring wtext,
                 std::string utf8,
                 std::vector<UrlRange> url_ranges = {},
                 std::vector<ColorRange> color_ranges = {})
        : layout_(std::move(layout)), wtext_(std::move(wtext)),
          utf8_(std::move(utf8)), url_ranges_(std::move(url_ranges)),
          color_ranges_(std::move(color_ranges))
    {
        DWRITE_TEXT_METRICS m{};
        layout_->GetMetrics(&m);
        size_ = Size{m.widthIncludingTrailingWhitespace, m.height};
        line_count_ = static_cast<int>(m.lineCount);

        // Segoe UI Emoji fills the full DirectWrite line-box (ascent +
        // descent), matching Apple Color Emoji on macOS.  Return the full
        // measured height so centering formulas (reaction chips, icon
        // buttons) land correctly, identical to the CoreGraphics backend.
        ascent_ = size_.h;
    }

    Size measure() const override
    {
        return size_;
    }
    int line_count() const override
    {
        return line_count_;
    }
    float ascent() const override
    {
        return ascent_;
    }

    std::string link_at(Point local) const override
    {
        if (url_ranges_.empty())
        {
            return {};
        }
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        layout_->HitTestPoint(local.x, local.y, &trailing, &inside, &m);
        if (!inside)
        {
            return {};
        }
        for (const auto& r : url_ranges_)
        {
            if (m.textPosition >= r.start && m.textPosition < r.end)
            {
                return r.url;
            }
        }
        return {};
    }

    int char_index_at(Point local) const override
    {
        if (wtext_.empty())
            return -1;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        layout_->HitTestPoint(local.x, local.y, &trailing, &inside, &m);
        UINT32 u16pos = m.textPosition + (trailing ? m.length : 0);
        u16pos = std::min(u16pos, static_cast<UINT32>(wtext_.size()));
        return utf16_to_utf8_byte(u16pos);
    }

    std::vector<Rect> selection_rects(int start_byte, int end_byte) const override
    {
        if (start_byte >= end_byte || wtext_.empty())
            return {};
        UINT32 u16start = utf8_to_utf16_unit(start_byte);
        UINT32 u16end   = utf8_to_utf16_unit(end_byte);
        if (u16start >= u16end)
            return {};
        UINT32 actual = 0;
        layout_->HitTestTextRange(u16start, u16end - u16start,
                                   0.f, 0.f, nullptr, 0, &actual);
        if (actual == 0)
            return {};
        std::vector<DWRITE_HIT_TEST_METRICS> hits(actual);
        layout_->HitTestTextRange(u16start, u16end - u16start,
                                   0.f, 0.f, hits.data(), actual, &actual);
        std::vector<Rect> out;
        out.reserve(actual);
        for (UINT32 i = 0; i < actual; ++i)
            out.push_back({hits[i].left, hits[i].top,
                           hits[i].width, hits[i].height});
        return out;
    }

    std::string text_range(int start_byte, int end_byte) const override
    {
        if (start_byte >= end_byte || utf8_.empty())
            return {};
        int lo = std::max(0, start_byte);
        int hi = std::min(end_byte, static_cast<int>(utf8_.size()));
        if (lo >= hi)
            return {};
        return utf8_.substr(lo, hi - lo);
    }

    IDWriteTextLayout* raw() const
    {
        return layout_.Get();
    }

    const std::vector<ColorRange>& color_ranges() const
    {
        return color_ranges_;
    }

private:
    // Convert a UTF-8 byte offset into the number of UTF-16 code units that
    // precede it (i.e. the UTF-16 position DWrite expects).
    UINT32 utf8_to_utf16_unit(int byte_offset) const
    {
        if (byte_offset <= 0)
            return 0;
        int clamped = std::min(byte_offset, static_cast<int>(utf8_.size()));
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8_.c_str(), clamped,
                                    nullptr, 0);
        return n > 0 ? static_cast<UINT32>(n) : 0;
    }

    // Convert a UTF-16 code-unit offset into the corresponding UTF-8 byte offset.
    int utf16_to_utf8_byte(UINT32 utf16_pos) const
    {
        if (utf16_pos == 0)
            return 0;
        UINT32 clamped = std::min(utf16_pos,
                                   static_cast<UINT32>(wtext_.size()));
        int n = WideCharToMultiByte(CP_UTF8, 0, wtext_.c_str(), clamped,
                                    nullptr, 0, nullptr, nullptr);
        return n > 0 ? n : 0;
    }

    ComPtr<IDWriteTextLayout> layout_;
    std::wstring wtext_;
    std::string  utf8_;
    std::vector<UrlRange> url_ranges_;
    std::vector<ColorRange> color_ranges_;
    Size size_{};
    int line_count_ = 0;
    float ascent_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  CubicEmojiTextRenderer — IDWriteTextRenderer that rasterises color
//  bitmap (CBDT/PNG) glyphs via DrawBitmap with HIGH_QUALITY_CUBIC.
//
//  D2D's built-in DrawTextLayout (and DrawColorBitmapGlyphRun under the
//  hood) hard-code D2D1_BITMAP_INTERPOLATION_MODE_LINEAR for bitmap colour
//  glyphs, which makes Noto Color Emoji look blocky when its 136 px CBDT
//  source is scaled down to ~19 px message-body size. Outline / COLR / SVG
//  runs fall through to the default ID2D1RenderTarget::DrawGlyphRun path.
// ─────────────────────────────────────────────────────────────────────────

class CubicEmojiTextRenderer final : public IDWriteTextRenderer
{
public:
    using BitmapCache = std::unordered_map<std::uint64_t, ComPtr<ID2D1Bitmap>>;

    CubicEmojiTextRenderer(ID2D1RenderTarget* rt, ID2D1DeviceContext* dc,
                           IDWriteFactory4* dw4, IWICImagingFactory* wic,
                           BitmapCache* cache, ID2D1Brush* fg)
        : rt_(rt), dc_(dc), dw4_(dw4), wic_(wic), cache_(cache), fg_(fg)
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer))
        {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IDWritePixelSnapping
    STDMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        *disabled = FALSE;
        return S_OK;
    }
    STDMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* m) override
    {
        D2D1_MATRIX_3X2_F t;
        rt_->GetTransform(&t);
        m->m11 = t._11; m->m12 = t._12;
        m->m21 = t._21; m->m22 = t._22;
        m->dx  = t.dx;  m->dy  = t.dy;
        return S_OK;
    }
    STDMETHODIMP GetPixelsPerDip(void*, FLOAT* p) override
    {
        FLOAT dpix, dpiy;
        rt_->GetDpi(&dpix, &dpiy);
        *p = dpix / 96.0f;
        return S_OK;
    }

    // IDWriteTextRenderer
    STDMETHODIMP DrawGlyphRun(void*, FLOAT bx, FLOAT by,
                               DWRITE_MEASURING_MODE mm,
                               const DWRITE_GLYPH_RUN* run,
                               const DWRITE_GLYPH_RUN_DESCRIPTION* desc,
                               IUnknown* effect) override
    {
        // A per-range drawing effect (set by draw_text for syntax-highlighted
        // code) overrides the default foreground brush for plain glyph runs.
        ID2D1Brush* fg = fg_;
        ComPtr<ID2D1SolidColorBrush> effect_brush;
        if (effect &&
            SUCCEEDED(effect->QueryInterface(IID_PPV_ARGS(&effect_brush))) &&
            effect_brush)
        {
            fg = effect_brush.Get();
        }

        // Without IDWriteFactory4 we can't enumerate colour runs — keep the
        // default linear behaviour so the text still renders.
        if (!dw4_)
        {
            rt_->DrawGlyphRun({bx, by}, run, fg, mm);
            return S_OK;
        }

        const DWRITE_GLYPH_IMAGE_FORMATS desired =
            DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
            DWRITE_GLYPH_IMAGE_FORMATS_CFF |
            DWRITE_GLYPH_IMAGE_FORMATS_COLR |
            DWRITE_GLYPH_IMAGE_FORMATS_PNG |
            DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
            DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
            DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8 |
            DWRITE_GLYPH_IMAGE_FORMATS_SVG;

        ComPtr<IDWriteColorGlyphRunEnumerator1> en;
        HRESULT hr = dw4_->TranslateColorGlyphRun(
            {bx, by}, run, desc, desired, mm, nullptr, 0, &en);
        if (hr == DWRITE_E_NOCOLOR || FAILED(hr) || !en)
        {
            rt_->DrawGlyphRun({bx, by}, run, fg, mm);
            return S_OK;
        }

        constexpr DWRITE_GLYPH_IMAGE_FORMATS bitmap_mask =
            DWRITE_GLYPH_IMAGE_FORMATS_PNG |
            DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
            DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
            DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;

        for (;;)
        {
            BOOL have = FALSE;
            if (FAILED(en->MoveNext(&have)) || !have) break;
            const DWRITE_COLOR_GLYPH_RUN1* cr = nullptr;
            if (FAILED(en->GetCurrentRun(&cr)) || !cr) break;

            if (cr->glyphImageFormat & bitmap_mask)
            {
                draw_bitmap_color_run_(cr);
            }
            else
            {
                // Outline (TrueType / CFF) or COLR overlay layer.
                ID2D1Brush* brush = fg;
                ComPtr<ID2D1SolidColorBrush> palette_brush;
                if (cr->paletteIndex != 0xFFFFu)
                {
                    if (SUCCEEDED(rt_->CreateSolidColorBrush(
                            cr->runColor, &palette_brush)) &&
                        palette_brush)
                    {
                        brush = palette_brush.Get();
                    }
                }
                rt_->DrawGlyphRun({cr->baselineOriginX, cr->baselineOriginY},
                                  &cr->glyphRun, brush, mm);
            }
        }
        return S_OK;
    }

    STDMETHODIMP DrawUnderline(void*, FLOAT bx, FLOAT by,
                                const DWRITE_UNDERLINE* u, IUnknown*) override
    {
        D2D1_RECT_F r{bx, by + u->offset,
                      bx + u->width, by + u->offset + u->thickness};
        rt_->FillRectangle(r, fg_);
        return S_OK;
    }
    STDMETHODIMP DrawStrikethrough(void*, FLOAT bx, FLOAT by,
                                    const DWRITE_STRIKETHROUGH* s,
                                    IUnknown*) override
    {
        D2D1_RECT_F r{bx, by + s->offset,
                      bx + s->width, by + s->offset + s->thickness};
        rt_->FillRectangle(r, fg_);
        return S_OK;
    }
    STDMETHODIMP DrawInlineObject(void* ctx, FLOAT bx, FLOAT by,
                                   IDWriteInlineObject* o,
                                   BOOL is_sw, BOOL is_rtl,
                                   IUnknown* effect) override
    {
        return o ? o->Draw(ctx, this, bx, by, is_sw, is_rtl, effect) : S_OK;
    }

private:
    void draw_bitmap_color_run_(const DWRITE_COLOR_GLYPH_RUN1* cr)
    {
        if (!cr->glyphRun.fontFace) return;
        ComPtr<IDWriteFontFace4> face4;
        if (FAILED(cr->glyphRun.fontFace->QueryInterface(
                IID_PPV_ARGS(&face4))) ||
            !face4)
        {
            return;
        }

        FLOAT x       = cr->baselineOriginX;
        const FLOAT y = cr->baselineOriginY;
        const float emSize = cr->glyphRun.fontEmSize;
        const bool isRtl   = (cr->glyphRun.bidiLevel & 1) != 0;

        for (UINT32 i = 0; i < cr->glyphRun.glyphCount; ++i)
        {
            const UINT16 g = cr->glyphRun.glyphIndices[i];
            const FLOAT adv =
                cr->glyphRun.glyphAdvances ? cr->glyphRun.glyphAdvances[i] : 0;
            const FLOAT offX = cr->glyphRun.glyphOffsets
                                   ? cr->glyphRun.glyphOffsets[i].advanceOffset
                                   : 0;
            const FLOAT offY = cr->glyphRun.glyphOffsets
                                   ? cr->glyphRun.glyphOffsets[i].ascenderOffset
                                   : 0;

            const UINT32 requested =
                static_cast<UINT32>(std::ceil(emSize));

            DWRITE_GLYPH_IMAGE_DATA data{};
            void* dataCtx = nullptr;
            if (FAILED(face4->GetGlyphImageData(g, requested,
                                                 cr->glyphImageFormat,
                                                 &data, &dataCtx)) ||
                !data.imageData || data.imageDataSize == 0 ||
                data.pixelsPerEm == 0)
            {
                if (dataCtx) face4->ReleaseGlyphImageData(dataCtx);
                x += isRtl ? -adv : adv;
                continue;
            }

            ID2D1Bitmap* bmp =
                get_or_make_bitmap_(data, cr->glyphImageFormat);
            if (bmp)
            {
                const float scale =
                    emSize / static_cast<float>(data.pixelsPerEm);
                // horizontalLeftOrigin follows OpenType bearing convention:
                // X is positive going right (same as D2D); Y is positive going
                // up from baseline (opposite of D2D Y-down), so we subtract.
                const float dstX = x + offX +
                                   data.horizontalLeftOrigin.x * scale;
                const float dstY = y - offY -
                                   data.horizontalLeftOrigin.y * scale;
                const float dstW = data.pixelSize.width  * scale;
                const float dstH = data.pixelSize.height * scale;
                const D2D1_RECT_F dstR{dstX, dstY, dstX + dstW, dstY + dstH};

                if (dc_)
                {
                    dc_->DrawBitmap(
                        bmp, dstR, 1.0f,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                }
                else
                {
                    rt_->DrawBitmap(
                        bmp, dstR, 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
            }

            if (dataCtx) face4->ReleaseGlyphImageData(dataCtx);
            x += isRtl ? -adv : adv;
        }
    }

    ID2D1Bitmap*
    get_or_make_bitmap_(const DWRITE_GLYPH_IMAGE_DATA& data,
                         DWRITE_GLYPH_IMAGE_FORMATS format)
    {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(data.uniqueDataId) << 32) |
            static_cast<std::uint64_t>(data.pixelsPerEm);

        auto it = cache_->find(key);
        if (it != cache_->end()) return it->second.Get();

        ComPtr<ID2D1Bitmap> bitmap;
        if (format == DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8)
        {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                   D2D1_ALPHA_MODE_PREMULTIPLIED));
            rt_->CreateBitmap(
                D2D1_SIZE_U{data.pixelSize.width, data.pixelSize.height},
                data.imageData, data.pixelSize.width * 4, &props, &bitmap);
        }
        else if (wic_)
        {
            ComPtr<IWICStream> stream;
            if (FAILED(wic_->CreateStream(&stream)) || !stream) return nullptr;
            if (FAILED(stream->InitializeFromMemory(
                    static_cast<BYTE*>(const_cast<void*>(data.imageData)),
                    data.imageDataSize)))
                return nullptr;
            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wic_->CreateDecoderFromStream(
                    stream.Get(), nullptr,
                    WICDecodeMetadataCacheOnLoad, &decoder)) ||
                !decoder)
                return nullptr;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return nullptr;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic_->CreateFormatConverter(&converter)) ||
                !converter)
                return nullptr;
            if (FAILED(converter->Initialize(
                    frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeMedianCut)))
                return nullptr;
            rt_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
        }

        if (!bitmap) return nullptr;
        auto* raw = bitmap.Get();
        cache_->emplace(key, std::move(bitmap));
        return raw;
    }

    ULONG ref_ = 1;
    ID2D1RenderTarget* rt_;
    ID2D1DeviceContext* dc_;     // may be null
    IDWriteFactory4* dw4_;       // may be null (Win10 1607+ only)
    IWICImagingFactory* wic_;
    BitmapCache* cache_;
    ID2D1Brush* fg_;
};

// ─────────────────────────────────────────────────────────────────────────
//  D2DCanvas — tk::Canvas implementation
// ─────────────────────────────────────────────────────────────────────────

class D2DCanvas : public Canvas
{
public:
    D2DCanvas(Backend::Impl& backend, ID2D1RenderTarget* rt)
        : backend_(backend), rt_(rt)
    {
        backend_.dwrite.As(&dw4_); // may be null on pre-1607 Windows 10
        update_dc(rt);
    }

    void rebind(ID2D1RenderTarget* rt)
    {
        rt_ = rt;
        update_dc(rt);
        brush_cache_.clear();
        emoji_bitmap_cache_.clear();
    }

    void clear(Color c) override
    {
        rt_->Clear(to_d2d(c));
    }

    void fill_rect(Rect r, Color c) override
    {
        rt_->FillRectangle(to_d2d(r), brush(c));
    }

    void fill_rounded_rect(Rect r, float radius, Color c) override
    {
        D2D1_ROUNDED_RECT rr{to_d2d(r), radius, radius};
        rt_->FillRoundedRectangle(rr, brush(c));
    }

    void stroke_rect(Rect r, Color c, float width) override
    {
        // D2D strokes are centred on the path: inset by half the width so
        // a 1 px stroke at (0,0,w,h) lands inside the rect.
        D2D1_RECT_F dr = to_d2d(r);
        float half = width * 0.5f;
        dr.left += half;
        dr.top += half;
        dr.right -= half;
        dr.bottom -= half;
        rt_->DrawRectangle(dr, brush(c), width);
    }

    void stroke_rounded_rect(Rect r, float radius, Color c,
                             float width) override
    {
        D2D1_RECT_F dr = to_d2d(r);
        float half = width * 0.5f;
        dr.left += half;
        dr.top += half;
        dr.right -= half;
        dr.bottom -= half;
        D2D1_ROUNDED_RECT rr{dr, radius, radius};
        rt_->DrawRoundedRectangle(rr, brush(c), width);
    }

    void draw_image(const Image& image, Rect dst) override
    {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp)
        {
            return;
        }
        D2D1_RECT_F d = to_d2d(dst);
        if (dc_)
            dc_->DrawBitmap(bmp, &d, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else
            rt_->DrawBitmap(bmp, &d, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void draw_image_subregion(const Image& image, Rect src, Rect dst) override
    {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp)
        {
            return;
        }
        D2D1_RECT_F srcr = to_d2d(src);
        D2D1_RECT_F dstr = to_d2d(dst);
        if (dc_)
            dc_->DrawBitmap(bmp, &dstr, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcr);
        else
            rt_->DrawBitmap(bmp, &dstr, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcr);
    }

    void draw_circle_image(const Image& image, Point centre,
                           float diameter) override
    {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp)
        {
            return;
        }

        ComPtr<ID2D1EllipseGeometry> ellipse;
        D2D1_ELLIPSE e =
            D2D1::Ellipse(to_d2d(centre), diameter * 0.5f, diameter * 0.5f);
        backend_.d2d->CreateEllipseGeometry(e, ellipse.GetAddressOf());
        if (!ellipse)
        {
            return;
        }

        D2D1_LAYER_PARAMETERS lp =
            D2D1::LayerParameters(D2D1::InfiniteRect(), ellipse.Get(),
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        rt_->PushLayer(lp, nullptr);
        Rect dst{centre.x - diameter * 0.5f, centre.y - diameter * 0.5f,
                 diameter, diameter};
        D2D1_RECT_F d = to_d2d(dst);
        if (dc_)
            dc_->DrawBitmap(bmp, &d, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else
            rt_->DrawBitmap(bmp, &d, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt_->PopLayer();
    }

    void draw_initials_circle(std::string_view name, Point centre,
                              float diameter, Color bg, Color fg) override
    {
        D2D1_ELLIPSE e =
            D2D1::Ellipse(to_d2d(centre), diameter * 0.5f, diameter * 0.5f);
        rt_->FillEllipse(e, brush(bg));

        std::wstring initials = initials_upper(name);
        // Pick a font size proportional to the diameter — matches the
        // GDI+ initials-disc code that used to live in MainWindow.cpp.
        float font_dip = diameter * kAvatarInitialsFontRatio;
        ComPtr<IDWriteTextFormat> tf;
        HRESULT hr = backend_.dwrite->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_dip, L"",
            tf.GetAddressOf());
        if (FAILED(hr))
        {
            return;
        }
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_RECT_F r =
            D2D1::RectF(centre.x - diameter * 0.5f, centre.y - diameter * 0.5f,
                        centre.x + diameter * 0.5f, centre.y + diameter * 0.5f);
        rt_->DrawText(initials.c_str(), static_cast<UINT32>(initials.size()),
                      tf.Get(), r, brush(fg), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    void draw_text(const TextLayout& layout, Point origin, Color c) override
    {
        auto& dl = static_cast<const DWriteLayout&>(layout);
        // Route through CubicEmojiTextRenderer so colour-bitmap (CBDT/PNG)
        // emoji glyphs render with HIGH_QUALITY_CUBIC. D2D's native
        // DrawTextLayout / DrawColorBitmapGlyphRun hard-code LINEAR, which
        // looks blocky when Noto Color Emoji's 136-px source is scaled to
        // ~19-px body size. Outline / COLR runs still use DrawGlyphRun.
        // Per-span syntax-highlight colours: attach a solid brush as the
        // drawing effect on each coloured range so DrawGlyphRun can pick it up.
        for (const auto& cr : dl.color_ranges())
        {
            dl.raw()->SetDrawingEffect(
                brush(cr.color), DWRITE_TEXT_RANGE{cr.start, cr.end - cr.start});
        }

        ComPtr<IDWriteTextRenderer> renderer;
        renderer.Attach(new CubicEmojiTextRenderer(
            rt_, dc_, dw4_.Get(), backend_.wic.Get(),
            &emoji_bitmap_cache_, brush(c)));
        dl.raw()->Draw(nullptr, renderer.Get(), origin.x, origin.y);
    }

    void push_clip_rect(Rect r) override
    {
        rt_->PushAxisAlignedClip(to_d2d(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        clip_stack_.push_back(ClipKind::AxisAligned);
    }

    void push_clip_rounded_rect(Rect r, float radius) override
    {
        ComPtr<ID2D1RoundedRectangleGeometry> geom;
        D2D1_ROUNDED_RECT rr{to_d2d(r), radius, radius};
        backend_.d2d->CreateRoundedRectangleGeometry(rr, geom.GetAddressOf());
        if (!geom)
        {
            // Degrade to axis-aligned clip so the stack stays balanced.
            rt_->PushAxisAlignedClip(to_d2d(r),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            clip_stack_.push_back(ClipKind::AxisAligned);
            return;
        }
        D2D1_LAYER_PARAMETERS lp =
            D2D1::LayerParameters(D2D1::InfiniteRect(), geom.Get(),
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->PushLayer(lp, nullptr);
        clip_stack_.push_back(ClipKind::Layer);
    }

    void pop_clip() override
    {
        if (clip_stack_.empty())
        {
            return;
        }
        ClipKind k = clip_stack_.back();
        clip_stack_.pop_back();
        if (k == ClipKind::AxisAligned)
        {
            rt_->PopAxisAlignedClip();
        }
        else
        {
            rt_->PopLayer();
        }
    }

    float scale_factor() const override
    {
        float dpi_x = 96.0f, dpi_y = 96.0f;
        rt_->GetDpi(&dpi_x, &dpi_y);
        return dpi_x / 96.0f;
    }

private:
    enum class ClipKind
    {
        AxisAligned,
        Layer
    };

    void update_dc(ID2D1RenderTarget* rt)
    {
        dc_ = nullptr;
        void* p = nullptr;
        if (SUCCEEDED(rt->QueryInterface(__uuidof(ID2D1DeviceContext), &p)))
        {
            dc_ = static_cast<ID2D1DeviceContext*>(p);
            dc_->Release(); // rt_ already holds the ref
        }
    }

    ID2D1SolidColorBrush* brush(Color c)
    {
        std::uint32_t key = (std::uint32_t(c.r) << 24) |
                            (std::uint32_t(c.g) << 16) |
                            (std::uint32_t(c.b) << 8) | std::uint32_t(c.a);
        auto it = brush_cache_.find(key);
        if (it != brush_cache_.end())
        {
            return it->second.Get();
        }
        ComPtr<ID2D1SolidColorBrush> b;
        rt_->CreateSolidColorBrush(to_d2d(c), b.GetAddressOf());
        ID2D1SolidColorBrush* raw = b.Get();
        brush_cache_.emplace(key, std::move(b));
        return raw;
    }

    Backend::Impl& backend_;
    ID2D1RenderTarget* rt_;
    ID2D1DeviceContext* dc_ = nullptr; // non-owning; valid iff rt_ IS a DeviceContext
    ComPtr<IDWriteFactory4> dw4_;      // null on pre-1607 Windows 10
    std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>>
        brush_cache_;
    // Glyph-image bitmap cache for CubicEmojiTextRenderer, keyed by
    // (uniqueDataId, pixelsPerEm). Each entry is an ID2D1Bitmap owned by rt_,
    // so the cache must be cleared on rebind() — the bitmaps are tied to the
    // old render target.
    CubicEmojiTextRenderer::BitmapCache emoji_bitmap_cache_;
    std::vector<ClipKind> clip_stack_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface::Impl
// ─────────────────────────────────────────────────────────────────────────

struct Surface::Impl
{
    Backend::Impl& backend;
    HWND hwnd;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID2D1Bitmap1> target_bmp;
    std::unique_ptr<D2DCanvas> canvas;
    bool painting = false;
    bool transparent = false;

    Impl(Backend::Impl& b, HWND h, bool t = false)
        : backend(b), hwnd(h), transparent(t)
    {
    }

    void create_target_bitmap()
    {
        ComPtr<IDXGISurface> surf;
        check(swap_chain->GetBuffer(0, IID_PPV_ARGS(surf.GetAddressOf())),
              "IDXGISwapChain1::GetBuffer");
        float dpi = static_cast<float>(GetDpiForWindow(hwnd));
        if (dpi == 0.0f)
        {
            dpi = 96.0f;
        }
        const D2D1_ALPHA_MODE bmp_alpha = transparent
                                              ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                              : D2D1_ALPHA_MODE_IGNORE;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, bmp_alpha), dpi, dpi);
        check(dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp,
                                              target_bmp.GetAddressOf()),
              "CreateBitmapFromDxgiSurface");
        dc->SetTarget(target_bmp.Get());
        dc->SetDpi(dpi, dpi);
    }

    void ensure_target()
    {
        if (dc)
        {
            return;
        }
        if (backend.device_lost_)
        {
            backend.create_d3d_device();
        }

        check(backend.d2d_dev->CreateDeviceContext(
                  D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.GetAddressOf()),
              "ID2D1Device::CreateDeviceContext");

        ComPtr<IDXGIAdapter> adapter;
        check(backend.dxgi_dev->GetAdapter(adapter.GetAddressOf()),
              "IDXGIDevice1::GetAdapter");
        ComPtr<IDXGIFactory2> factory2;
        check(adapter->GetParent(IID_PPV_ARGS(factory2.GetAddressOf())),
              "IDXGIAdapter::GetParent");

        RECT rc{};
        GetClientRect(hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(std::max<LONG>(rc.right, 1));
        desc.Height = static_cast<UINT>(std::max<LONG>(rc.bottom, 1));
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = transparent ? DXGI_ALPHA_MODE_PREMULTIPLIED
                                     : DXGI_ALPHA_MODE_IGNORE;
        check(factory2->CreateSwapChainForHwnd(backend.d3d.Get(), hwnd, &desc,
                                               nullptr, nullptr,
                                               swap_chain.GetAddressOf()),
              "IDXGIFactory2::CreateSwapChainForHwnd");

        create_target_bitmap();
        canvas = std::make_unique<D2DCanvas>(backend, dc.Get());
    }

    void resize(int w, int h)
    {
        if (!dc || !swap_chain)
        {
            return;
        }
        dc->SetTarget(nullptr);
        target_bmp.Reset();
        check(swap_chain->ResizeBuffers(0, static_cast<UINT>(std::max(w, 1)),
                                        static_cast<UINT>(std::max(h, 1)),
                                        DXGI_FORMAT_UNKNOWN, 0),
              "IDXGISwapChain1::ResizeBuffers");
        create_target_bitmap();
    }

    void drop_target(bool device_removed = false)
    {
        canvas.reset();
        if (dc)
        {
            dc->SetTarget(nullptr);
        }
        target_bmp.Reset();
        swap_chain.Reset();
        dc.Reset();
        if (device_removed)
        {
            backend.device_lost_ = true;
        }
    }
};

Surface::Surface(Backend& b, HWND h, bool transparent)
    : impl_(std::make_unique<Impl>(b.impl(), h, transparent))
{
}

Surface::~Surface() = default;

void Surface::resize(int w, int h)
{
    impl_->resize(w, h);
}

Canvas& Surface::begin_paint()
{
    impl_->ensure_target();
    impl_->dc->BeginDraw();
    impl_->painting = true;
    impl_->canvas->rebind(impl_->dc.Get());
    return *impl_->canvas;
}

bool Surface::end_paint()
{
    if (!impl_->painting)
    {
        return false;
    }
    impl_->painting = false;
    HRESULT hr = impl_->dc->EndDraw();
    if (SUCCEEDED(hr))
    {
        hr = impl_->swap_chain->Present(0, 0);
    }
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->drop_target();
        return true;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        impl_->drop_target(true);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────

class D2DFactory : public CanvasFactory
{
public:
    explicit D2DFactory(Backend& b) : owner_(b), backend_(b.impl())
    {
    }

    std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) override
    {
        return tk::d2d::decode_image(owner_, bytes);
    }

    std::unique_ptr<Image> create_image_rgba(const std::uint8_t* pixels, int w,
                                             int h) override
    {
        if (!pixels || w <= 0 || h <= 0)
        {
            return nullptr;
        }
        // Premultiply + swizzle straight RGBA → premultiplied BGRA for WIC.
        const int n = w * h * 4;
        std::vector<std::uint8_t> bgra(static_cast<size_t>(n));
        for (int i = 0; i < w * h; ++i)
        {
            const unsigned a = pixels[i * 4 + 3];
            bgra[static_cast<size_t>(i * 4 + 0)] =
                static_cast<std::uint8_t>((pixels[i * 4 + 2] * a + 127) / 255); // B
            bgra[static_cast<size_t>(i * 4 + 1)] =
                static_cast<std::uint8_t>((pixels[i * 4 + 1] * a + 127) / 255); // G
            bgra[static_cast<size_t>(i * 4 + 2)] =
                static_cast<std::uint8_t>((pixels[i * 4 + 0] * a + 127) / 255); // R
            bgra[static_cast<size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(a);
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(backend_.wic->CreateBitmapFromMemory(
                static_cast<UINT>(w), static_cast<UINT>(h),
                GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(w * 4),
                static_cast<UINT>(n), bgra.data(), bitmap.GetAddressOf())))
        {
            return nullptr;
        }
        return std::make_unique<D2DImage>(std::move(bitmap), w, h);
    }

    std::unique_ptr<AnimatedImage>
    decode_animated_image(std::span<const std::uint8_t> bytes,
                          int max_px) override
    {
        auto raw = tk::d2d::decode_animation(owner_, bytes);
        if (raw.empty())
            return nullptr;

        std::vector<std::unique_ptr<Image>> frames;
        std::vector<int> delays;
        frames.reserve(raw.size());
        delays.reserve(raw.size());

        for (auto& f : raw)
        {
            std::unique_ptr<Image> img = std::move(f.image);
            if (!img)
                continue;
            if (auto scaled = scale_image(*img, max_px, max_px))
                img = std::move(scaled);
            frames.push_back(std::move(img));
            delays.push_back(f.delay_ms);
        }

        if (frames.size() < 2)
            return nullptr;
        return std::make_unique<AnimatedImage>(std::move(frames),
                                              std::move(delays));
    }

    std::unique_ptr<TextLayout> build_text(std::string_view utf8,
                                           const TextStyle& s) override
    {
        // A wrap=false layout must stay on one line; DirectWrite honours hard
        // breaks even with NO_WRAP, so fold them out first (see
        // tk::fold_hard_breaks_utf8).
        std::wstring wide =
            s.wrap ? utf8_to_wide(utf8)
                   : utf8_to_wide(fold_hard_breaks_utf8(utf8));
        IDWriteTextFormat* tf = backend_.text_format_for(s.role);
        if (!tf)
        {
            return nullptr;
        }

        float max_w = s.max_width >= 0 ? s.max_width : 8192.0f;
        float max_h = s.max_height >= 0 ? s.max_height : 8192.0f;

        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = backend_.dwrite->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), tf, max_w, max_h,
            layout.GetAddressOf());
        if (FAILED(hr) || !layout)
        {
            return nullptr;
        }

        if (backend_.font_fallback)
        {
            ComPtr<IDWriteTextLayout2> layout2;
            if (SUCCEEDED(layout.As(&layout2)))
            {
                layout2->SetFontFallback(backend_.font_fallback.Get());
            }
        }

        DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING;
        switch (s.halign)
        {
        case TextHAlign::Leading:
            a = DWRITE_TEXT_ALIGNMENT_LEADING;
            break;
        case TextHAlign::Center:
            a = DWRITE_TEXT_ALIGNMENT_CENTER;
            break;
        case TextHAlign::Trailing:
            a = DWRITE_TEXT_ALIGNMENT_TRAILING;
            break;
        }
        layout->SetTextAlignment(a);

        DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        switch (s.valign)
        {
        case TextVAlign::Top:
            pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            break;
        case TextVAlign::Center:
            pa = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
            break;
        case TextVAlign::Bottom:
            pa = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
            break;
        }
        layout->SetParagraphAlignment(pa);

        layout->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP
                                       : DWRITE_WORD_WRAPPING_NO_WRAP);

        if (s.trim == TextTrim::Ellipsis)
        {
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis_sign;
            backend_.dwrite->CreateEllipsisTrimmingSign(
                tf, ellipsis_sign.GetAddressOf());
            layout->SetTrimming(&tr, ellipsis_sign.Get());
        }

        return std::make_unique<DWriteLayout>(std::move(layout),
                                              std::move(wide),
                                              std::string(utf8));
    }

    std::unique_ptr<TextLayout> build_rich_text(std::span<const TextSpan> spans,
                                                const TextStyle& s) override
    {
        // Concatenate all span text to UTF-16 and plain UTF-8, tracking ranges.
        std::wstring wide;
        std::string plain_utf8;
        struct WideRange
        {
            UINT32 start, end;
            const TextSpan* sp;
        };
        std::vector<WideRange> ranges;
        ranges.reserve(spans.size());
        for (const auto& sp : spans)
        {
            UINT32 start = static_cast<UINT32>(wide.size());
            wide += utf8_to_wide(sp.text);
            plain_utf8 += sp.text;
            ranges.push_back({start, static_cast<UINT32>(wide.size()), &sp});
        }

        IDWriteTextFormat* tf = backend_.text_format_for(s.role);
        if (!tf)
        {
            return nullptr;
        }

        float max_w = s.max_width >= 0 ? s.max_width : 8192.0f;
        float max_h = s.max_height >= 0 ? s.max_height : 8192.0f;

        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = backend_.dwrite->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), tf, max_w, max_h,
            layout.GetAddressOf());
        if (FAILED(hr) || !layout)
        {
            return nullptr;
        }

        if (backend_.font_fallback)
        {
            ComPtr<IDWriteTextLayout2> layout2;
            if (SUCCEEDED(layout.As(&layout2)))
            {
                layout2->SetFontFallback(backend_.font_fallback.Get());
            }
        }

        const float emoji_size_dip =
            static_cast<float>(font_role_pt(FontRole::InlineEmoji,
                                            win32_system_base_pt())) *
            (96.0f / 72.0f);

        // Apply per-span formatting.
        std::vector<DWriteLayout::UrlRange> url_ranges;
        std::vector<DWriteLayout::ColorRange> color_ranges;
        for (const auto& wr : ranges)
        {
            const TextSpan& sp = *wr.sp;
            DWRITE_TEXT_RANGE tr{wr.start, wr.end - wr.start};
            if (sp.is_emoji_run)
            {
                layout->SetFontSize(emoji_size_dip, tr);
            }
            if (sp.bold)
            {
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, tr);
            }
            if (sp.semibold)
            {
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, tr);
            }
            if (sp.italic)
            {
                layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, tr);
            }
            if (sp.code)
            {
                layout->SetFontFamilyName(L"Cascadia Code", tr);
            }
            if (sp.strikethrough)
            {
                layout->SetStrikethrough(TRUE, tr);
            }
            if (!sp.url.empty())
            {
                // Mention pills keep their hit-test range but render without the
                // link underline (accent colour via has_color, pill background
                // drawn by the view).
                if (!sp.is_mention)
                {
                    layout->SetUnderline(TRUE, tr);
                }
                url_ranges.push_back({wr.start, wr.end, sp.url});
            }
            if (sp.has_color)
            {
                // Foreground applied at draw time via SetDrawingEffect, so the
                // brush can be created from the live render target.
                color_ranges.push_back({wr.start, wr.end, sp.color});
            }
        }

        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP
                                       : DWRITE_WORD_WRAPPING_NO_WRAP);

        if (s.trim == TextTrim::Ellipsis)
        {
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> sign;
            backend_.dwrite->CreateEllipsisTrimmingSign(tf,
                                                        sign.GetAddressOf());
            layout->SetTrimming(&tr, sign.Get());
        }

        return std::make_unique<DWriteLayout>(std::move(layout),
                                              std::move(wide),
                                              std::move(plain_utf8),
                                              std::move(url_ranges),
                                              std::move(color_ranges));
    }

private:
    Backend& owner_;
    Backend::Impl& backend_;
};

std::unique_ptr<CanvasFactory> make_factory(Backend& b)
{
    return std::make_unique<D2DFactory>(b);
}

std::unique_ptr<Canvas> make_canvas(Backend& b, ID2D1RenderTarget* rt)
{
    return std::make_unique<D2DCanvas>(b.impl(), rt);
}

Factories factories(Backend& b)
{
    Backend::Impl& impl = b.impl();
    return Factories{impl.d2d.Get(), impl.dwrite.Get(), impl.wic.Get(),
                     impl.font_fallback.Get(), impl.noto_emoji_face.Get(),
                     impl.d2d_dev.Get(), impl.d3d.Get()};
}

// ─────────────────────────────────────────────────────────────────────────
//  make_image_from_bgra — create tk::Image from raw BGRA pixel buffer
// ─────────────────────────────────────────────────────────────────────────

std::unique_ptr<Image>
make_image_from_bgra(Backend& b, const std::uint8_t* pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0)
    {
        return nullptr;
    }
    Backend::Impl& impl = b.impl();
    if (!impl.wic)
    {
        return nullptr;
    }

    const UINT stride = static_cast<UINT>(w) * 4u;
    const UINT size = stride * static_cast<UINT>(h);

    // IWICImagingFactory::CreateBitmapFromMemory copies the pixel data into
    // a new IWICBitmap, so the caller's buffer may be freed immediately.
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = impl.wic->CreateBitmapFromMemory(
        static_cast<UINT>(w), static_cast<UINT>(h),
        GUID_WICPixelFormat32bppBGRA, stride, size,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(pixels)),
        bmp.GetAddressOf());
    if (FAILED(hr) || !bmp)
    {
        return nullptr;
    }

    // MFVideoFormat_RGB32 is BGRX: the 4th byte is unused (0x00 from MF).
    // opaque=true tells D2D to ignore it instead of treating it as alpha=0.
    return std::make_unique<D2DImage>(std::move(bmp), w, h, /*opaque=*/true);
}

// ─────────────────────────────────────────────────────────────────────────
//  decode_animation — multi-frame WIC decode (GIF/APNG/animated WebP)
// ─────────────────────────────────────────────────────────────────────────

namespace
{

// Pull a per-frame delay (in ms) from the WIC metadata query reader.
// Falls back to 100 ms when no codec-specific delay is found. The 20 ms
// floor matches what browsers use to keep tight-loop GIFs from burning
// CPU on encoders that wrote 0 ms.
int read_frame_delay_ms(IWICBitmapFrameDecode* frame)
{
    int delay_ms = 100;

    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf())) || !reader)
    {
        return delay_ms;
    }

    auto read_uint = [&](const wchar_t* path, UINT& out) -> bool
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        bool ok = false;
        if (SUCCEEDED(reader->GetMetadataByName(path, &var)))
        {
            switch (var.vt)
            {
            case VT_UI2:
                out = var.uiVal;
                ok = true;
                break;
            case VT_UI4:
                out = var.uintVal;
                ok = true;
                break;
            case VT_I2:
                out = static_cast<UINT>(var.iVal);
                ok = true;
                break;
            case VT_I4:
                out = static_cast<UINT>(var.intVal);
                ok = true;
                break;
            default:
                break;
            }
        }
        PropVariantClear(&var);
        return ok;
    };

    // GIF: graphics-control extension's Delay is in 1/100 s.
    UINT gif_delay = 0;
    if (read_uint(L"/grctlext/Delay", gif_delay) && gif_delay > 0)
    {
        delay_ms = static_cast<int>(gif_delay) * 10;
    }
    else
    {
        // APNG: fcTL chunk carries DelayNumerator / DelayDenominator. WIC
        // exposes them on Windows 10 1809+. Denominator 0 means 1/100 s
        // per the PNG spec.
        UINT num = 0, denom = 100;
        if (read_uint(L"/fcTL/delay_num", num))
        {
            (void)read_uint(L"/fcTL/delay_den", denom);
            if (denom == 0)
            {
                denom = 100;
            }
            if (num > 0)
            {
                delay_ms = static_cast<int>(num * 1000u / denom);
            }
        }
        // WebP: ANMF chunk's Duration is in ms directly. The path varies
        // across codec versions; both are tried best-effort.
        UINT webp_ms = 0;
        if (delay_ms == 100 &&
            (read_uint(L"/ANMF/FrameDuration", webp_ms) ||
             read_uint(L"/ANMF/Duration", webp_ms)) &&
            webp_ms > 0)
        {
            delay_ms = static_cast<int>(webp_ms);
        }
    }

    if (delay_ms <= 0)
    {
        delay_ms = 100;
    }
    if (delay_ms < 20)
    {
        delay_ms = 20; // browsers' floor
    }
    return delay_ms;
}

} // namespace

std::unique_ptr<Image> decode_image(Backend& b,
                                    std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return nullptr;
    }

    Backend::Impl& impl = b.impl();

    ComPtr<IWICStream> stream;
    if (FAILED(impl.wic->CreateStream(stream.GetAddressOf())))
    {
        return nullptr;
    }
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))))
    {
        return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(impl.wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnLoad,
                                                 decoder.GetAddressOf())))
    {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return nullptr;
    }

    // Read EXIF orientation tag (274). Try the JPEG IFD path first, then the
    // TIFF/PNG path. Missing or unreadable metadata leaves the transform as the
    // identity (Rotate0) and the rotator is skipped entirely.
    WICBitmapTransformOptions exif_transform = WICBitmapTransformRotate0;
    {
        ComPtr<IWICMetadataQueryReader> meta;
        if (SUCCEEDED(frame->GetMetadataQueryReader(meta.GetAddressOf())))
        {
            auto try_path = [&](const wchar_t* path) -> bool
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                bool found = false;
                if (SUCCEEDED(meta->GetMetadataByName(path, &pv)) &&
                    pv.vt == VT_UI2)
                {
                    found = true;
                    switch (pv.uiVal)
                    {
                    case 2: exif_transform = WICBitmapTransformFlipHorizontal; break;
                    case 3: exif_transform = WICBitmapTransformRotate180; break;
                    case 4: exif_transform = WICBitmapTransformFlipVertical; break;
                    case 5:
                        exif_transform = static_cast<WICBitmapTransformOptions>(
                            WICBitmapTransformRotate90 |
                            WICBitmapTransformFlipHorizontal);
                        break;
                    case 6: exif_transform = WICBitmapTransformRotate90; break;
                    case 7:
                        exif_transform = static_cast<WICBitmapTransformOptions>(
                            WICBitmapTransformRotate270 |
                            WICBitmapTransformFlipHorizontal);
                        break;
                    case 8: exif_transform = WICBitmapTransformRotate270; break;
                    default: break; // 1 = normal; unknown values are ignored
                    }
                }
                PropVariantClear(&pv);
                return found;
            };
            if (!try_path(L"/app1/ifd/{ushort=274}"))
                try_path(L"/ifd/{ushort=274}");
        }
    }

    // When a non-identity orientation is present, interpose an
    // IWICBitmapFlipRotator between the frame and the format converter.
    // The rotator implements IWICBitmapSource, so it drops in transparently;
    // GetSize() on the rotated source already returns post-rotation dimensions.
    ComPtr<IWICBitmapFlipRotator> rotator;
    IWICBitmapSource* decode_source = frame.Get();
    if (exif_transform != WICBitmapTransformRotate0)
    {
        if (SUCCEEDED(impl.wic->CreateBitmapFlipRotator(
                rotator.GetAddressOf())) &&
            SUCCEEDED(rotator->Initialize(frame.Get(), exif_transform)))
        {
            decode_source = rotator.Get();
        }
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(impl.wic->CreateFormatConverter(converter.GetAddressOf())))
    {
        return nullptr;
    }
    if (FAILED(converter->Initialize(decode_source, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0f,
                                     WICBitmapPaletteTypeMedianCut)))
    {
        return nullptr;
    }

    // Force an eager decode into an in-memory IWICBitmap. The
    // decoder/converter chain otherwise reads pixels lazily from the
    // IWICStream, which was initialised from caller-owned memory that
    // does not outlive this call.
    ComPtr<IWICBitmap> cached;
    if (FAILED(impl.wic->CreateBitmapFromSource(
            converter.Get(), WICBitmapCacheOnLoad, cached.GetAddressOf())))
    {
        return nullptr;
    }

    UINT w = 0, h = 0;
    cached->GetSize(&w, &h);
    return std::make_unique<D2DImage>(std::move(cached), static_cast<int>(w),
                                      static_cast<int>(h));
}

// GIF compositor: each WIC GIF frame is a delta region (sub-rect of the
// canvas).  We blit each frame onto a persistent PBGRA canvas buffer,
// snapshot the composited result, then apply the disposal method before
// moving to the next frame.  This produces full-canvas bitmaps that the
// animation cache can display directly without any additional compositing.
static std::vector<AnimatedFrame> decode_gif_animation(Backend::Impl& impl,
                                                       IWICBitmapDecoder* decoder,
                                                       UINT frame_count)
{
    std::vector<AnimatedFrame> result;

    // Read canvas dimensions from the GIF logical screen descriptor.
    UINT canvas_w = 0, canvas_h = 0;
    {
        ComPtr<IWICMetadataQueryReader> dmeta;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(dmeta.GetAddressOf())))
        {
            auto read_ui = [&](const wchar_t* path) -> UINT
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                UINT val = 0;
                if (SUCCEEDED(dmeta->GetMetadataByName(path, &pv)))
                {
                    if (pv.vt == VT_UI2) val = pv.uiVal;
                    else if (pv.vt == VT_UI4) val = pv.uintVal;
                }
                PropVariantClear(&pv);
                return val;
            };
            canvas_w = read_ui(L"/logscrdesc/Width");
            canvas_h = read_ui(L"/logscrdesc/Height");
        }
    }

    // Fallback: decode frame 0 to get the canvas size from its bitmap.
    if (canvas_w == 0 || canvas_h == 0)
    {
        ComPtr<IWICBitmapFrameDecode> f0;
        if (FAILED(decoder->GetFrame(0, f0.GetAddressOf())))
            return result;
        f0->GetSize(&canvas_w, &canvas_h);
    }
    if (canvas_w == 0 || canvas_h == 0)
        return result;

    // Compositing canvas: PBGRA, initially fully transparent.
    const UINT stride = canvas_w * 4;
    std::vector<std::uint8_t> canvas(stride * canvas_h, 0);

    result.reserve(frame_count);

    for (UINT i = 0; i < frame_count; ++i)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(i, frame.GetAddressOf())))
            continue;

        // Read per-frame rect and disposal method from GIF metadata.
        UINT left = 0, top = 0, disposal = 0;
        {
            ComPtr<IWICMetadataQueryReader> fmeta;
            if (SUCCEEDED(frame->GetMetadataQueryReader(fmeta.GetAddressOf())))
            {
                auto read_ui = [&](const wchar_t* path) -> UINT
                {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    UINT val = 0;
                    if (SUCCEEDED(fmeta->GetMetadataByName(path, &pv)))
                    {
                        if (pv.vt == VT_UI1) val = pv.bVal;
                        else if (pv.vt == VT_UI2) val = pv.uiVal;
                        else if (pv.vt == VT_UI4) val = pv.uintVal;
                    }
                    PropVariantClear(&pv);
                    return val;
                };
                left     = read_ui(L"/imgdesc/Left");
                top      = read_ui(L"/imgdesc/Top");
                disposal = read_ui(L"/grctlext/Disposal");
            }
        }

        // Decode the frame's sub-region to PBGRA pixels.
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(impl.wic->CreateFormatConverter(converter.GetAddressOf())))
            continue;
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeMedianCut)))
            continue;

        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (fw == 0 || fh == 0)
            continue;

        // Clamp frame rect to canvas bounds.
        if (left >= canvas_w || top >= canvas_h)
            continue;
        fw = std::min(fw, canvas_w - left);
        fh = std::min(fh, canvas_h - top);

        const UINT fstride = fw * 4;
        std::vector<std::uint8_t> frame_px(fstride * fh);
        if (FAILED(converter->CopyPixels(nullptr, fstride,
                                         static_cast<UINT>(frame_px.size()),
                                         frame_px.data())))
            continue;

        // For RESTORE_PREVIOUS (disposal==3): snapshot the region before we
        // write to it so we can put it back after this frame is stored.
        std::vector<std::uint8_t> saved;
        if (disposal == 3)
        {
            saved.resize(fstride * fh);
            for (UINT y = 0; y < fh; ++y)
                std::memcpy(saved.data() + y * fstride,
                            canvas.data() + (top + y) * stride + left * 4,
                            fstride);
        }

        // Blit frame onto canvas.  PBGRA: alpha==0 means transparent; any
        // non-zero alpha replaces (GIF pixels are either opaque or absent).
        for (UINT y = 0; y < fh; ++y)
        {
            const auto* src =
                reinterpret_cast<const std::uint32_t*>(frame_px.data() + y * fstride);
            auto* dst = reinterpret_cast<std::uint32_t*>(
                canvas.data() + (top + y) * stride + left * 4);
            for (UINT x = 0; x < fw; ++x)
                if (src[x] >> 24)
                    dst[x] = src[x];
        }

        // Snapshot the composited canvas into an independent IWICBitmap.
        // CreateBitmapFromMemory wraps (aliases) the buffer, so we
        // immediately copy it via CreateBitmapFromSource+CacheOnLoad.
        ComPtr<IWICBitmap> alias;
        if (FAILED(impl.wic->CreateBitmapFromMemory(
                canvas_w, canvas_h, GUID_WICPixelFormat32bppPBGRA,
                stride, static_cast<UINT>(canvas.size()), canvas.data(),
                alias.GetAddressOf())))
            continue;
        ComPtr<IWICBitmap> snap;
        if (FAILED(impl.wic->CreateBitmapFromSource(
                alias.Get(), WICBitmapCacheOnLoad, snap.GetAddressOf())))
            continue;

        AnimatedFrame af;
        af.image = std::make_unique<D2DImage>(std::move(snap),
                                              static_cast<int>(canvas_w),
                                              static_cast<int>(canvas_h));
        af.delay_ms = read_frame_delay_ms(frame.Get());
        result.push_back(std::move(af));

        // Apply disposal method for the next frame.
        if (disposal == 2) // restore to background (transparent)
        {
            for (UINT y = 0; y < fh; ++y)
                std::memset(canvas.data() + (top + y) * stride + left * 4, 0,
                            fstride);
        }
        else if (disposal == 3) // restore to previous state
        {
            for (UINT y = 0; y < fh; ++y)
                std::memcpy(canvas.data() + (top + y) * stride + left * 4,
                            saved.data() + y * fstride,
                            fstride);
        }
        // disposal 0/1: leave canvas as-is
    }

    if (result.size() < 2)
        result.clear();

    return result;
}

std::vector<AnimatedFrame> decode_animation(Backend& b,
                                            std::span<const std::uint8_t> bytes)
{
    std::vector<AnimatedFrame> result;
    if (bytes.empty())
    {
        return result;
    }

    Backend::Impl& impl = b.impl();

    ComPtr<IWICStream> stream;
    if (FAILED(impl.wic->CreateStream(stream.GetAddressOf())))
    {
        return result;
    }
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))))
    {
        return result;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(impl.wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnLoad,
                                                 decoder.GetAddressOf())))
    {
        return result;
    }

    GUID container = {};
    decoder->GetContainerFormat(&container);

    // PNG: WIC on Windows 11 exposes APNG as multi-frame but returns
    // un-composited delta frames.  Skip the animation path entirely;
    // decode_image() fetches frame 0 (the IDAT default) correctly.
    if (container == GUID_ContainerFormatPng)
        return result;

    UINT frame_count = 0;
    if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count <= 1)
        return result;

    // GIF: delta frames require full compositing — delegate to the
    // dedicated compositor that handles offsets and disposal methods.
    if (container == GUID_ContainerFormatGif)
        return decode_gif_animation(impl, decoder.Get(), frame_count);

    // Other animated formats (WebP, etc.): each WIC frame is already a
    // full-canvas bitmap; store them directly.
    result.reserve(frame_count);
    for (UINT i = 0; i < frame_count; ++i)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(i, frame.GetAddressOf())))
            continue;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(impl.wic->CreateFormatConverter(converter.GetAddressOf())))
            continue;
        if (FAILED(converter->Initialize(frame.Get(),
                                         GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeMedianCut)))
            continue;

        ComPtr<IWICBitmap> cached;
        if (FAILED(impl.wic->CreateBitmapFromSource(
                converter.Get(), WICBitmapCacheOnLoad, cached.GetAddressOf())))
            continue;

        UINT w = 0, h = 0;
        cached->GetSize(&w, &h);
        if (w == 0 || h == 0)
            continue;

        AnimatedFrame af;
        af.image = std::make_unique<D2DImage>(
            std::move(cached), static_cast<int>(w), static_cast<int>(h));
        af.delay_ms = read_frame_delay_ms(frame.Get());
        result.push_back(std::move(af));
    }

    if (result.size() < 2)
        result.clear();

    return result;
}

} // namespace tk::d2d
