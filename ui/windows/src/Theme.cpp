#include "Theme.h"

#include "tk/canvas_d2d.h"
#include "tk/canvas.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace win32::theme
{

namespace
{

// DWMWA_* constants added in Win10 1809 / Win11 22H2. Defined locally so the
// code compiles against older SDKs; runtime calls silently fail on older OS.
constexpr DWORD kDwmaUseImmersiveDarkMode_Pre20H1 = 19;
constexpr DWORD kDwmaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmaWindowCornerPreference = 33;
constexpr DWORD kDwmaBorderColor = 34;
constexpr DWORD kDwmaCaptionColor = 35;
constexpr DWORD kDwmaTextColor = 36;
constexpr DWORD kDwmaSystemBackdropType = 38;

constexpr int kDwmwcpRound = 2;      // DWMWCP_ROUND
constexpr int kDwmwcpRoundSmall = 3; // DWMWCP_ROUNDSMALL
constexpr int kDwmsbtMainWindow = 2; // DWMSBT_MAINWINDOW (Mica)

Mode g_mode = Mode::Light;
bool g_mode_initialised = false;
HWND g_main_hwnd = nullptr;

// Undocumented uxtheme.dll ordinals that make native popup menus
// (CreatePopupMenu/TrackPopupMenu — the "Copy" menu, the sticker save menu,
// the user-info panel menu) render dark, since there is no public/documented
// API for this. Same technique used by Windows Terminal and most other
// Win32 dark-mode implementations; stable since Win10 1809. Resolved lazily
// and cached; a null pointer just means an older Windows build, in which
// case menus silently keep following the OS-wide setting as before.
enum class PreferredAppMode
{
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

void apply_menu_theme(Mode m)
{
    static SetPreferredAppModeFn set_preferred_app_mode = []
    {
        HMODULE h = GetModuleHandleW(L"uxtheme.dll");
        return h ? reinterpret_cast<SetPreferredAppModeFn>(
                       GetProcAddress(h, MAKEINTRESOURCEA(135)))
                  : nullptr;
    }();
    static FlushMenuThemesFn flush_menu_themes = []
    {
        HMODULE h = GetModuleHandleW(L"uxtheme.dll");
        return h ? reinterpret_cast<FlushMenuThemesFn>(
                       GetProcAddress(h, MAKEINTRESOURCEA(136)))
                  : nullptr;
    }();
    if (!set_preferred_app_mode || !flush_menu_themes)
    {
        return;
    }
    set_preferred_app_mode(m == Mode::Dark ? PreferredAppMode::AllowDark
                                           : PreferredAppMode::Default);
    flush_menu_themes();
}

std::unordered_map<COLORREF, HBRUSH> g_brush_cache;
HFONT g_fonts[5] = {}; // indexed by FontRole

const Palette& light_palette()
{
    // Re-tuned light palette: warmer chrome, brighter accent, gentler border.
    static const Palette p{
        /* window_bg               */ RGB(0xFF, 0xFF, 0xFF),
        /* sidebar_bg              */ RGB(0xF3, 0xF3, 0xF3),
        /* sidebar_sel_bg          */ RGB(0xE5, 0xEE, 0xFB),
        /* sidebar_hover_bg        */ RGB(0xEB, 0xEB, 0xEB),
        /* chrome_bg               */ RGB(0xFB, 0xFB, 0xFB),
        /* compose_card_bg         */ RGB(0xF7, 0xF7, 0xF7),
        /* border                  */ RGB(0xE1, 0xE1, 0xE1),
        /* separator               */ RGB(0xEC, 0xEC, 0xEC),
        /* text_primary            */ RGB(0x1B, 0x1B, 0x1B),
        /* text_secondary          */ RGB(0x60, 0x60, 0x60),
        /* text_muted              */ RGB(0x8E, 0x8E, 0x93),
        /* text_on_accent          */ RGB(0xFF, 0xFF, 0xFF),
        /* accent                  */ RGB(0x00, 0x67, 0xC0),
        /* accent_hover            */ RGB(0x00, 0x55, 0x9C),
        /* accent_pressed          */ RGB(0x00, 0x47, 0x82),
        /* subtle_hover            */ RGB(0xEA, 0xEA, 0xEA),
        /* subtle_pressed          */ RGB(0xDE, 0xDE, 0xDE),
        /* reaction_chip_bg        */ RGB(0xED, 0xEF, 0xF1),
        /* reaction_chip_bg_me     */ RGB(0xD6, 0xE4, 0xFF),
        /* reaction_chip_border    */ RGB(0xD0, 0xD4, 0xD9),
        /* reaction_chip_border_me */ RGB(0x35, 0x78, 0xE5),
        /* reaction_chip_text      */ RGB(0x1A, 0x1A, 0x2E),
        /* reaction_chip_text_me   */ RGB(0x0B, 0x3A, 0xA1),
        /* unread_badge_bg         */ RGB(0x00, 0x84, 0xFF),
        /* unread_badge_text       */ RGB(0xFF, 0xFF, 0xFF),
    };
    return p;
}

const Palette& dark_palette()
{
    // Win11 Fluent dark: charcoal chrome, near-white text, light-blue accent.
    static const Palette p{
        /* window_bg               */ RGB(0x1F, 0x1F, 0x1F),
        /* sidebar_bg              */ RGB(0x17, 0x17, 0x17),
        /* sidebar_sel_bg          */ RGB(0x2E, 0x3A, 0x52),
        /* sidebar_hover_bg        */ RGB(0x26, 0x26, 0x26),
        /* chrome_bg               */ RGB(0x1B, 0x1B, 0x1B),
        /* compose_card_bg         */ RGB(0x2A, 0x2A, 0x2A),
        /* border                  */ RGB(0x33, 0x33, 0x33),
        /* separator               */ RGB(0x2A, 0x2A, 0x2A),
        /* text_primary            */ RGB(0xF0, 0xF0, 0xF0),
        /* text_secondary          */ RGB(0xB8, 0xB8, 0xB8),
        /* text_muted              */ RGB(0x90, 0x90, 0x90),
        /* text_on_accent          */ RGB(0x00, 0x1A, 0x2E),
        /* accent                  */ RGB(0x60, 0xCD, 0xFF),
        /* accent_hover            */ RGB(0x8A, 0xDB, 0xFF),
        /* accent_pressed          */ RGB(0x4A, 0xA8, 0xD6),
        /* subtle_hover            */ RGB(0x2D, 0x2D, 0x2D),
        /* subtle_pressed          */ RGB(0x38, 0x38, 0x38),
        /* reaction_chip_bg        */ RGB(0x2B, 0x2B, 0x2B),
        /* reaction_chip_bg_me     */ RGB(0x1D, 0x3A, 0x5C),
        /* reaction_chip_border    */ RGB(0x3A, 0x3A, 0x3A),
        /* reaction_chip_border_me */ RGB(0x60, 0xCD, 0xFF),
        /* reaction_chip_text      */ RGB(0xE0, 0xE0, 0xE0),
        /* reaction_chip_text_me   */ RGB(0xCC, 0xE7, 0xFF),
        /* unread_badge_bg         */ RGB(0x60, 0xCD, 0xFF),
        /* unread_badge_text       */ RGB(0x00, 0x1A, 0x2E),
    };
    return p;
}

Mode read_system_mode()
{
    HKEY key = nullptr;
    LSTATUS rc = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
        KEY_READ, &key);
    if (rc != ERROR_SUCCESS)
    {
        return Mode::Light;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                     reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return value == 0 ? Mode::Dark : Mode::Light;
}

void clear_brush_cache()
{
    for (auto& kv : g_brush_cache)
    {
        if (kv.second)
        {
            DeleteObject(kv.second);
        }
    }
    g_brush_cache.clear();
}

bool font_family_exists(const wchar_t* name)
{
    HDC hdc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, name, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(
        hdc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int
        {
            *reinterpret_cast<bool*>(lp) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, hdc);
    return found;
}

const wchar_t* preferred_family(FontRole role)
{
    // Segoe UI Variable shipped with Win11. Pick the optical-size axis that
    // matches the role; fall back to plain Segoe UI on Win10 and earlier.
    static bool checked = false;
    static bool has_var = false;
    if (!checked)
    {
        has_var = font_family_exists(L"Segoe UI Variable Display") &&
                  font_family_exists(L"Segoe UI Variable Text");
        checked = true;
    }
    if (!has_var)
    {
        return L"Segoe UI";
    }
    return (role == FontRole::Title) ? L"Segoe UI Variable Display"
                                     : L"Segoe UI Variable Text";
}

HFONT make_font(FontRole role)
{
    // Point size → device units relative to a 96-DPI logical inch (the OS
    // already accounts for DPI on per-monitor-aware processes).
    // This GDI theme has its own 5-value FontRole mapped to tk::FontRole
    // equivalents so font_role_pt() applies the same system-base offsets.
    const int base = tk::d2d::win32_system_base_pt();
    int pt = tk::font_role_pt(tk::FontRole::SenderName, base);
    LONG weight = FW_NORMAL;
    switch (role)
    {
    case FontRole::Small:
        pt = tk::font_role_pt(tk::FontRole::Timestamp, base);
        weight = FW_NORMAL;
        break;
    case FontRole::Body:
        pt = tk::font_role_pt(tk::FontRole::SidebarPreview, base);
        weight = FW_NORMAL;
        break;
    case FontRole::Ui:
        pt = tk::font_role_pt(tk::FontRole::SenderName, base);
        weight = FW_NORMAL;
        break;
    case FontRole::UiSemibold:
        pt = tk::font_role_pt(tk::FontRole::SenderName, base);
        weight = FW_SEMIBOLD;
        break;
    case FontRole::Title:
        pt = tk::font_role_pt(tk::FontRole::Title, base);
        weight = FW_SEMIBOLD;
        break;
    }
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);
    LONG height = -MulDiv(pt, dpi, 72);

    LOGFONTW lf{};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    wcsncpy_s(lf.lfFaceName, preferred_family(role), _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

} // namespace

// ---------------------------------------------------------------------------

Mode current_mode()
{
    if (!g_mode_initialised)
    {
        g_mode = read_system_mode();
        g_mode_initialised = true;
    }
    return g_mode;
}

const Palette& palette()
{
    return current_mode() == Mode::Dark ? dark_palette() : light_palette();
}

bool refresh_from_system()
{
    Mode m = read_system_mode();
    g_mode_initialised = true;
    if (m == g_mode)
    {
        return false;
    }
    g_mode = m;
    clear_brush_cache();
    return true;
}

COLORREF accent_colorref()
{
    // AccentColorMenu is 0xAABBGGRR (alpha + COLORREF byte order).
    // Masking out the alpha gives a ready-to-use COLORREF.
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
            0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        DWORD value = 0, size = sizeof(value), type = REG_DWORD;
        LSTATUS rc = RegQueryValueExW(key, L"AccentColorMenu", nullptr, &type,
                                      reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && type == REG_DWORD)
            return value & 0x00FFFFFF;
    }
    // Fallback: DwmGetColorizationColor is documented as 0xAARRGGBB.
    DWORD col = 0;
    BOOL  opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque)))
        return RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
    return RGB(0x00, 0x78, 0xD4); // Win11 default blue
}

void set_mode(Mode m)
{
    bool was_initialised = g_mode_initialised;
    g_mode_initialised = true;
    if (was_initialised && m == g_mode)
    {
        return;
    }
    g_mode = m;
    clear_brush_cache();
    apply_menu_theme(m);
}

void register_main_window(HWND hwnd)
{
    g_main_hwnd = hwnd;
}

void apply_window_attributes(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    BOOL dark = (current_mode() == Mode::Dark) ? TRUE : FALSE;

    // Dark caption. Try the post-20H1 attribute first; fall back to the
    // 19H1 placeholder value used by early Insider builds.
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmaUseImmersiveDarkMode, &dark,
                                     sizeof(dark))))
    {
        DwmSetWindowAttribute(hwnd, kDwmaUseImmersiveDarkMode_Pre20H1, &dark,
                              sizeof(dark));
    }

    // Title bar colours (Win11 22H2+). Silently ignored on older builds.
    // CAPTION_COLOR overrides the Mica translucency in the caption area in
    // favour of a solid chrome that matches the rest of the window.
    const auto& pal = palette();
    COLORREF caption = pal.chrome_bg;
    COLORREF caption_text = pal.text_primary;
    COLORREF border_col = pal.border;
    DwmSetWindowAttribute(hwnd, kDwmaCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, kDwmaTextColor, &caption_text,
                          sizeof(caption_text));
    DwmSetWindowAttribute(hwnd, kDwmaBorderColor, &border_col,
                          sizeof(border_col));

    // Mica backdrop (Win11 22H2+). Ignored on older builds.
    int backdrop = kDwmsbtMainWindow;
    DwmSetWindowAttribute(hwnd, kDwmaSystemBackdropType, &backdrop,
                          sizeof(backdrop));

    // Win11 rounded corners.
    int corner = kDwmwcpRound;
    DwmSetWindowAttribute(hwnd, kDwmaWindowCornerPreference, &corner,
                          sizeof(corner));
}

void apply_control_theme(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    const wchar_t* sub =
        (current_mode() == Mode::Dark) ? L"DarkMode_Explorer" : L"Explorer";
    SetWindowTheme(hwnd, sub, nullptr);
}

HFONT font(FontRole role)
{
    int idx = static_cast<int>(role);
    if (!g_fonts[idx])
    {
        g_fonts[idx] = make_font(role);
    }
    return g_fonts[idx];
}

HBRUSH brush(COLORREF c)
{
    auto it = g_brush_cache.find(c);
    if (it != g_brush_cache.end())
    {
        return it->second;
    }
    HBRUSH b = CreateSolidBrush(c);
    g_brush_cache.emplace(c, b);
    return b;
}

void on_dpi_changed()
{
    for (HFONT& f : g_fonts)
    {
        if (f)
        {
            DeleteObject(f);
            f = nullptr;
        }
    }
}

void shutdown()
{
    clear_brush_cache();
    for (HFONT& f : g_fonts)
    {
        if (f)
        {
            DeleteObject(f);
            f = nullptr;
        }
    }
    g_main_hwnd = nullptr;
}

// ── Fluent buttons ─────────────────────────────────────────────────────────

namespace
{

enum class BtnState : uint8_t
{
    Normal,
    Hover,
    Pressed,
    Disabled
};
struct BtnInfo
{
    ButtonStyle style;
    BtnState state;
};

std::unordered_map<HWND, BtnInfo>& button_registry()
{
    static std::unordered_map<HWND, BtnInfo> map;
    return map;
}

void fill_rounded(Gdiplus::Graphics& g, Gdiplus::Brush& brush, float x, float y,
                  float w, float h, float r)
{
    r = std::min(r, std::min(w, h) / 2.0f);
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, r * 2, r * 2, 180, 90);
    path.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

LRESULT CALLBACK button_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR /*id*/,
                                      DWORD_PTR /*ref*/)
{
    auto& reg = button_registry();
    auto it = reg.find(hwnd);
    auto invalidate = [&]
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    };
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (it != reg.end() && it->second.state == BtnState::Normal)
        {
            it->second.state = BtnState::Hover;
            invalidate();
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (it != reg.end())
        {
            it->second.state =
                IsWindowEnabled(hwnd) ? BtnState::Normal : BtnState::Disabled;
            invalidate();
        }
        break;
    case WM_LBUTTONDOWN:
        if (it != reg.end())
        {
            it->second.state = BtnState::Pressed;
            invalidate();
        }
        break;
    case WM_LBUTTONUP:
        if (it != reg.end())
        {
            it->second.state = BtnState::Hover;
            invalidate();
        }
        break;
    case WM_ENABLE:
        if (it != reg.end())
        {
            it->second.state = wParam ? BtnState::Normal : BtnState::Disabled;
            invalidate();
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, button_subclass_proc, 0);
        reg.erase(hwnd);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

void register_button(HWND h, ButtonStyle style)
{
    if (!h)
    {
        return;
    }
    button_registry()[h] = {style, BtnState::Normal};
    SetWindowSubclass(h, button_subclass_proc, 0, 0);
}

void draw_button(DRAWITEMSTRUCT* dis)
{
    auto& reg = button_registry();
    auto it = reg.find(dis->hwndItem);
    if (it == reg.end())
    {
        return;
    }
    BtnInfo info = it->second;
    const auto& pal = palette();

    bool pressed =
        (dis->itemState & ODS_SELECTED) != 0 || info.state == BtnState::Pressed;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0 ||
                    info.state == BtnState::Disabled;
    bool hover = info.state == BtnState::Hover && !pressed && !disabled;

    COLORREF fill;
    COLORREF text;
    switch (info.style)
    {
    case ButtonStyle::Primary:
        if (disabled)
        {
            fill = pal.subtle_pressed;
            text = pal.text_muted;
        }
        else if (pressed)
        {
            fill = pal.accent_pressed;
            text = pal.text_on_accent;
        }
        else if (hover)
        {
            fill = pal.accent_hover;
            text = pal.text_on_accent;
        }
        else
        {
            fill = pal.accent;
            text = pal.text_on_accent;
        }
        break;
    case ButtonStyle::Subtle:
        text = disabled ? pal.text_muted : pal.text_primary;
        if (disabled)
        {
            fill = pal.compose_card_bg;
        }
        else if (pressed)
        {
            fill = pal.subtle_pressed;
        }
        else if (hover)
        {
            fill = pal.subtle_hover;
        }
        else
        {
            fill = pal.compose_card_bg;
        }
        break;
    case ButtonStyle::Icon:
    default:
        text = disabled ? pal.text_muted : pal.text_primary;
        if (pressed)
        {
            fill = pal.subtle_pressed;
        }
        else if (hover)
        {
            fill = pal.subtle_hover;
        }
        else
        {
            fill = pal.compose_card_bg;
        }
        break;
    }

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    Gdiplus::SolidBrush erase(gpc(pal.compose_card_bg));
    g.FillRectangle(&erase, (INT)dis->rcItem.left, (INT)dis->rcItem.top,
                    (INT)(dis->rcItem.right - dis->rcItem.left),
                    (INT)(dis->rcItem.bottom - dis->rcItem.top));

    Gdiplus::SolidBrush fillBrush(gpc(fill));
    float fx = (float)dis->rcItem.left + 1.0f;
    float fy = (float)dis->rcItem.top + 1.0f;
    float fw = (float)(dis->rcItem.right - dis->rcItem.left) - 2.0f;
    float fh = (float)(dis->rcItem.bottom - dis->rcItem.top) - 2.0f;
    float radius = (info.style == ButtonStyle::Icon)
                       ? std::min(fw, fh) / 2.0f - 2.0f
                       : 4.0f;
    fill_rounded(g, fillBrush, fx, fy, fw, fh, radius);

    wchar_t label[128] = {};
    GetWindowTextW(dis->hwndItem, label, 127);
    if (label[0])
    {
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, text);
        HFONT old = (HFONT)SelectObject(dis->hDC,
                                        font(info.style == ButtonStyle::Primary
                                                 ? FontRole::UiSemibold
                                                 : FontRole::Ui));
        DrawTextW(dis->hDC, label, -1, &dis->rcItem,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    if (dis->itemState & ODS_FOCUS)
    {
        RECT fr = dis->rcItem;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }
}

} // namespace win32::theme
