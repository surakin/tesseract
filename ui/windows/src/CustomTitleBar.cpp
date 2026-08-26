#include "CustomTitleBar.h"
#include "Theme.h"
#include "icons.h"
#include "svg.h"
#include "tk/host_win32.h"

#include <BetterText/BetterText.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace win32
{

namespace
{
// Windows 11's close-button hover/press convention — a fixed system red
// rather than a theme color, matching what every other app on the desktop
// does regardless of its own palette.
constexpr COLORREF kCloseHoverBg = RGB(0xC4, 0x2B, 0x1C);
constexpr COLORREF kClosePressedBg = RGB(0xC1, 0x5C, 0x51);
constexpr COLORREF kCloseGlyphOnRed = RGB(0xFF, 0xFF, 0xFF);

// COLORREF (0x00BBGGRR) -> BetterTextTheme's 0xRRGGBBAA, fully opaque.
constexpr std::uint32_t bt_rgba(COLORREF c)
{
    return (static_cast<std::uint32_t>(GetRValue(c)) << 24) |
           (static_cast<std::uint32_t>(GetGValue(c)) << 16) |
           (static_cast<std::uint32_t>(GetBValue(c)) << 8) | 0xFFu;
}

enum class Glyph
{
    Minimize,
    Maximize,
    Restore,
    Close,
    Count,
};

std::span<const std::uint8_t> glyph_svg(Glyph g)
{
    switch (g)
    {
    case Glyph::Minimize:
        return kTitlebarMinimizeSvg;
    case Glyph::Maximize:
        return kTitlebarMaximizeSvg;
    case Glyph::Restore:
        return kTitlebarRestoreSvg;
    case Glyph::Close:
        return kTitlebarCloseSvg;
    default:
        return {};
    }
}

// Rasterized-icon cache, keyed by (glyph, size, color) and shared by every
// CustomTitleBar instance (MainWindow's and each RoomWindow's) — the icons
// are identical regardless of which window is drawing them, so there is no
// reason to rasterize the same SVG once per window. A fixed 4-slot array
// (one per Glyph) rather than a growable container: indexing by Glyph is
// O(1) and never reallocates, so the Gdiplus::Bitmap objects (which alias
// `pixels`'s buffer directly rather than copying it — see the constructor
// docs) never have their backing storage moved out from under them.
struct IconSlot
{
    int px = -1;
    COLORREF color = 0;
    // Must outlive `bitmap`: the Gdiplus::Bitmap(w,h,stride,format,scan0)
    // constructor used below wraps this buffer rather than copying it.
    std::vector<std::uint8_t> pixels;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
};

std::array<IconSlot, static_cast<std::size_t>(Glyph::Count)>& icon_cache()
{
    static std::array<IconSlot, static_cast<std::size_t>(Glyph::Count)> cache;
    return cache;
}

Gdiplus::Bitmap* icon_bitmap(Glyph g, int px, COLORREF color)
{
    IconSlot& slot = icon_cache()[static_cast<std::size_t>(g)];
    if (slot.bitmap && slot.px == px && slot.color == color)
    {
        return slot.bitmap.get();
    }

    const tk::Color tint{GetRValue(color), GetGValue(color), GetBValue(color),
                         255};
    std::vector<std::uint8_t> rgba =
        tk::rasterize_svg_rgba(glyph_svg(g), px, tint);
    if (rgba.empty())
    {
        return nullptr;
    }
    // nanosvg's rasterizer produces straight-alpha R,G,B,A; GDI+'s
    // PixelFormat32bppARGB expects the same straight alpha but in B,G,R,A
    // byte order.
    for (std::size_t i = 0; i + 2 < rgba.size(); i += 4)
    {
        std::swap(rgba[i], rgba[i + 2]);
    }

    slot.pixels = std::move(rgba);
    slot.bitmap = std::make_unique<Gdiplus::Bitmap>(
        px, px, px * 4, PixelFormat32bppARGB, slot.pixels.data());
    slot.px = px;
    slot.color = color;
    return slot.bitmap.get();
}

} // namespace

void CustomTitleBar::shutdown_icon_cache()
{
    for (IconSlot& slot : icon_cache())
    {
        slot.bitmap.reset();
        slot.pixels.clear();
        slot.px = -1;
    }
}

CustomTitleBar::~CustomTitleBar()
{
    if (text_hwnd_)
    {
        DestroyWindow(text_hwnd_);
    }
}

void CustomTitleBar::attach(HWND owner)
{
    // BetterTextRegisterControl is safe to call more than once (from
    // multiple CustomTitleBar instances / alongside host_win32.cpp's own
    // BetterTextField/Area registration) but there's no reason to pay for
    // the repeat call.
    static bool registered =
        BetterTextRegisterControl(
            reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr))) != FALSE;
    (void)registered;

    text_hwnd_ = CreateWindowExW(
        0, BETTERTEXT_CLASS_NAME, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
        owner, nullptr, reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr)),
        nullptr);
    if (!text_hwnd_)
    {
        return;
    }
    // Keep it fully invisible/non-hit-tested on screen — its rendering is
    // captured and composited manually into this strip's HDC in paint()
    // instead (see BetterTextRequestCaptureBGRA/ReadCaptureBGRA there).
    // WS_EX_LAYERED was tried for this same purpose elsewhere in this app
    // and reverted: it broke BetterText's swap-chain presentation on real
    // hardware (see BetterTextField's constructor, host_win32.cpp).
    SetWindowRgn(text_hwnd_, CreateRectRgn(0, 0, 0, 0), TRUE);
    BetterTextSetPresentEnabled(text_hwnd_, FALSE);
    BetterTextSetStatic(text_hwnd_, TRUE);
    BetterTextSetFontProvider(text_hwnd_, &tk::win32::noto_emoji_font_provider());
}

void CustomTitleBar::on_dpi_changed(UINT dpi)
{
    dpi_ = dpi > 0 ? dpi : 96;
    height_px_ = static_cast<int>(std::lround(kHeightDip * dpi_ / 96.0f));
}

void CustomTitleBar::compute_button_rects(int client_w_px, RECT& btn_min,
                                          RECT& btn_max, RECT& btn_close) const
{
    const int btn_w =
        static_cast<int>(std::lround(kButtonWidthDip * dpi_ / 96.0f));
    btn_close = RECT{client_w_px - btn_w, 0, client_w_px, height_px_};
    btn_max = RECT{btn_close.left - btn_w, 0, btn_close.left, height_px_};
    btn_min = RECT{btn_max.left - btn_w, 0, btn_max.left, height_px_};
}

void CustomTitleBar::invalidate_strip(HWND hwnd) const
{
    // bErase=TRUE: both MainWindow and RoomWindow paint their own
    // background (and this strip) from WM_ERASEBKGND rather than WM_PAINT
    // — BeginPaint only sends WM_ERASEBKGND when some pending invalidation
    // in the update region requested an erase, so FALSE here would mean
    // hover/press changes never actually got redrawn.
    RECT rc{};
    GetClientRect(hwnd, &rc);
    rc.bottom = height_px_;
    InvalidateRect(hwnd, &rc, TRUE);
}

void CustomTitleBar::adjust_nccalcsize(HWND hwnd, WPARAM wParam,
                                       LPARAM lParam) const
{
    if (wParam != TRUE)
    {
        return;
    }
    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
    RECT& rc = params->rgrc[0];
    const LONG original_top = rc.top;
    DefWindowProcW(hwnd, WM_NCCALCSIZE, wParam, lParam);
    rc.top = original_top;
    if (IsZoomed(hwnd))
    {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        rc.top += frame;
    }
}

std::optional<LRESULT> CustomTitleBar::dwm_hittest_hook(HWND hwnd, UINT msg,
                                                         WPARAM wParam,
                                                         LPARAM lParam)
{
    LRESULT result = 0;
    if (DwmDefWindowProc(hwnd, msg, wParam, lParam, &result))
    {
        return result;
    }
    return std::nullopt;
}

LRESULT CustomTitleBar::handle_nchittest(HWND hwnd, LPARAM lParam) const
{
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);
    if (pt.y < 0 || pt.y >= height_px_)
    {
        return HTNOWHERE;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    if (pt.x < 0 || pt.x >= client.right)
    {
        return HTNOWHERE;
    }
    RECT btn_min{}, btn_max{}, btn_close{};
    compute_button_rects(client.right, btn_min, btn_max, btn_close);
    if (PtInRect(&btn_close, pt))
    {
        return HTCLOSE;
    }
    if (PtInRect(&btn_max, pt))
    {
        return HTMAXBUTTON;
    }
    if (PtInRect(&btn_min, pt))
    {
        return HTMINBUTTON;
    }
    return HTCAPTION;
}

void CustomTitleBar::handle_ncmousemove(HWND hwnd, WPARAM ht)
{
    const UINT code = static_cast<UINT>(ht);
    if (hovered_ht_ == code)
    {
        return;
    }
    hovered_ht_ = code;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
    TrackMouseEvent(&tme);
    invalidate_strip(hwnd);
}

void CustomTitleBar::handle_ncmouseleave(HWND hwnd)
{
    if (hovered_ht_ == HTNOWHERE)
    {
        return;
    }
    hovered_ht_ = HTNOWHERE;
    invalidate_strip(hwnd);
}

bool CustomTitleBar::handle_nclbuttondown(HWND hwnd, WPARAM ht)
{
    if (ht != HTMINBUTTON && ht != HTMAXBUTTON && ht != HTCLOSE)
    {
        return false;
    }
    pressed_ht_ = static_cast<UINT>(ht);
    invalidate_strip(hwnd);
    return true;
}

bool CustomTitleBar::handle_nclbuttonup(HWND hwnd, WPARAM ht)
{
    if (ht != HTMINBUTTON && ht != HTMAXBUTTON && ht != HTCLOSE)
    {
        return false;
    }
    const bool fire = (pressed_ht_ == static_cast<UINT>(ht));
    pressed_ht_ = HTNOWHERE;
    invalidate_strip(hwnd);
    if (fire)
    {
        switch (ht)
        {
        case HTMINBUTTON:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case HTMAXBUTTON:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case HTCLOSE:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        }
    }
    return true;
}

bool CustomTitleBar::show_system_menu(HWND hwnd, WPARAM ht,
                                      LPARAM lParam) const
{
    if (ht != HTCAPTION)
    {
        return false;
    }
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu)
    {
        return false;
    }
    const bool zoomed = IsZoomed(hwnd) != FALSE;
    EnableMenuItem(menu, SC_RESTORE,
                  MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MOVE,
                  MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE,
                  MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MAXIMIZE,
                  MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);

    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                   0, hwnd, nullptr);
    if (cmd)
    {
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
    }
    return true;
}

void CustomTitleBar::paint(HDC hdc, HWND hwnd, const RECT& client_rc,
                           bool active)
{
    RECT strip = client_rc;
    strip.bottom = strip.top + height_px_;
    const auto& pal = theme::palette();

    FillRect(hdc, &strip, theme::brush(pal.chrome_bg));

    RECT btn_min{}, btn_max{}, btn_close{};
    compute_button_rects(client_rc.right, btn_min, btn_max, btn_close);

    // Cosmetic — tweak freely. Lucide icons carry their own internal
    // padding, so this reads as noticeably smaller than the button box.
    constexpr float kIconDip = 14.0f;
    const int icon_px =
        std::max(1, static_cast<int>(std::lround(kIconDip * dpi_ / 96.0f)));

    struct ButtonSpec
    {
        const RECT& rc;
        UINT ht;
        Glyph glyph;
    };
    const bool zoomed = IsZoomed(hwnd) != FALSE;
    const ButtonSpec specs[3] = {
        {btn_min, static_cast<UINT>(HTMINBUTTON), Glyph::Minimize},
        {btn_max, static_cast<UINT>(HTMAXBUTTON),
         zoomed ? Glyph::Restore : Glyph::Maximize},
        {btn_close, static_cast<UINT>(HTCLOSE), Glyph::Close},
    };

    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    for (const auto& s : specs)
    {
        const bool is_close = (s.ht == HTCLOSE);
        const bool is_pressed = (s.ht == pressed_ht_);
        const bool is_hovered = (s.ht == hovered_ht_);

        HBRUSH bg_brush;
        bool bg_owned = false;
        if (is_pressed)
        {
            bg_brush = is_close ? CreateSolidBrush(kClosePressedBg)
                                : theme::brush(pal.subtle_pressed);
            bg_owned = is_close;
        }
        else if (is_hovered)
        {
            bg_brush = is_close ? CreateSolidBrush(kCloseHoverBg)
                                : theme::brush(pal.subtle_hover);
            bg_owned = is_close;
        }
        else
        {
            bg_brush = theme::brush(pal.chrome_bg);
        }
        FillRect(hdc, &s.rc, bg_brush);
        if (bg_owned)
        {
            DeleteObject(bg_brush);
        }

        const bool glyph_on_red = is_close && (is_pressed || is_hovered);
        const COLORREF glyph_color = glyph_on_red
                                         ? kCloseGlyphOnRed
                                         : (active ? pal.text_primary
                                                  : pal.text_secondary);
        if (Gdiplus::Bitmap* bmp = icon_bitmap(s.glyph, icon_px, glyph_color))
        {
            const int x =
                s.rc.left + ((s.rc.right - s.rc.left) - icon_px) / 2;
            const int y =
                s.rc.top + ((s.rc.bottom - s.rc.top) - icon_px) / 2;
            gfx.DrawImage(bmp, x, y, icon_px, icon_px);
        }
    }

    // Drawn last so it isn't overwritten by a hovered/pressed button's own
    // background fill, which spans the button's full height including this
    // row.
    RECT sep{strip.left, strip.bottom - 1, strip.right, strip.bottom};
    FillRect(hdc, &sep, theme::brush(pal.separator));

    HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    const int icon_size = GetSystemMetricsForDpi(SM_CXSMICON, dpi_);
    const int pad = static_cast<int>(std::lround(8.0f * dpi_ / 96.0f));
    int text_left = strip.left + pad;
    if (icon)
    {
        const int icon_y = strip.top + (height_px_ - icon_size) / 2;
        DrawIconEx(hdc, text_left, icon_y, icon, icon_size, icon_size, 0,
                  nullptr, DI_NORMAL);
        text_left += icon_size + pad;
    }

    wchar_t title[256];
    const int n = GetWindowTextW(hwnd, title, 256);
    if (text_hwnd_ && n > 0)
    {
        RECT text_rc{text_left, strip.top, btn_min.left - pad, strip.bottom};
        if (text_rc.right > text_rc.left)
        {
            const std::wstring text(title, static_cast<std::size_t>(n));
            const COLORREF fg = active ? pal.text_primary : pal.text_secondary;
            const bool text_changed = text != last_text_;
            const bool color_changed = fg != last_fg_;
            const bool rect_changed = !EqualRect(&text_rc, &last_rect_);

            // paint() runs on every strip repaint, including every
            // hover-move over the caption buttons — none of which touch
            // the title text. Only go anywhere near text_hwnd_ when
            // something it actually renders has changed; otherwise just
            // re-blit the cached capture below. SetWindowPos matters here
            // even for an unchanged rect: Windows still dispatches WM_SIZE
            // for it, and BetterText's handler unconditionally tears down
            // and recreates its render target on every WM_SIZE (see
            // ResizeRenderTarget, BetterTextControl.cpp) — doing that once
            // per mouse-move tick was the actual cause of flicker.
            if (text_changed || color_changed || rect_changed ||
                !last_bitmap_valid_)
            {
                if (text_changed)
                {
                    BetterTextSetText(text_hwnd_, text.c_str());
                    last_text_ = text;
                }
                if (color_changed)
                {
                    // Color comes from BetterTextTheme, not
                    // BetterTextTextStyle — matches host_win32.cpp's
                    // bt_apply_default_font/bt_theme_from_palette split.
                    // Transparent background (alpha 0) so only the glyph
                    // pixels composite into hdc below; selection/caret/
                    // placeholder colors are irrelevant (this control is
                    // static — see attach()) but set anyway for a
                    // fully-defined theme struct.
                    BetterTextTheme bt_theme{};
                    bt_theme.background_rgba = 0;
                    bt_theme.foreground_rgba = bt_rgba(fg);
                    bt_theme.selection_rgba = bt_theme.foreground_rgba;
                    bt_theme.caret_rgba = bt_theme.foreground_rgba;
                    bt_theme.placeholder_rgba = bt_theme.foreground_rgba;
                    BetterTextSetTheme(text_hwnd_, &bt_theme);
                    last_fg_ = fg;
                }
                if (rect_changed)
                {
                    const theme::FontDesc fd =
                        theme::font_desc(theme::FontRole::Ui);
                    BetterTextTextStyle style{};
                    style.font_family = fd.family;
                    // pt -> DIP: DirectWrite/BetterText font sizes are in
                    // DIPs (96/inch); theme::font_desc reports points
                    // (72/inch) — same conversion bt_apply_default_font
                    // (host_win32.cpp) uses. Only needs setting alongside
                    // a resize since it never otherwise changes at
                    // runtime.
                    style.font_size = fd.size_pt * (96.0f / 72.0f);
                    style.font_weight = fd.weight;
                    BetterTextSetDefaultTextStyle(text_hwnd_, &style);

                    SetWindowPos(text_hwnd_, nullptr, text_rc.left,
                                text_rc.top, text_rc.right - text_rc.left,
                                text_rc.bottom - text_rc.top,
                                SWP_NOZORDER | SWP_NOACTIVATE);
                    last_rect_ = text_rc;
                }

                int cap_w = 0, cap_h = 0;
                if (BetterTextRequestCaptureBGRA(text_hwnd_, &cap_w,
                                                 &cap_h) &&
                    cap_w > 0 && cap_h > 0)
                {
                    // Capture into a scratch buffer first — only replace
                    // the cache (and last_bitmap_valid_) on an actual
                    // successful read. A transient failure here (e.g.
                    // right around window activation) must not discard a
                    // still-good previous frame and blank the title text
                    // for a tick until the next repaint happens to recover
                    // it; just keep showing the stale-but-valid one.
                    std::vector<std::uint8_t> new_pixels(
                        static_cast<std::size_t>(cap_w) * cap_h * 4);
                    if (BetterTextReadCaptureBGRA(
                            text_hwnd_, new_pixels.data(),
                            static_cast<int>(new_pixels.size()), &cap_w,
                            &cap_h))
                    {
                        last_bitmap_pixels_ = std::move(new_pixels);
                        last_bitmap_w_ = cap_w;
                        last_bitmap_h_ = cap_h;
                        last_bitmap_valid_ = true;
                    }
                }
            }

            if (last_bitmap_valid_)
            {
                // Premultiplied alpha, BGRA byte order — matches
                // PixelFormat32bppPArgb directly (see BetterText's
                // ReadCaptureBGRA doc comment).
                Gdiplus::Bitmap bmp(last_bitmap_w_, last_bitmap_h_,
                                   last_bitmap_w_ * 4, PixelFormat32bppPARGB,
                                   last_bitmap_pixels_.data());
                gfx.DrawImage(&bmp, text_rc.left, text_rc.top,
                             last_bitmap_w_, last_bitmap_h_);
            }
        }
    }
}

} // namespace win32
