#include "MainWindow.h"
#include <shlobj.h> // SHBrowseForFolderW — native folder picker for history export
#include "RoomWindow.h"
#include "AuxWindow.h"
#include "CallWindow.h"
#include "views/BrandView.h"
#include "views/media_viewer_items.h"
#include "views/shortcut_registry.h"
#include "Win32Notifier.h"
#include "Win32Autostart.h"
#include "Win32PowerMonitor.h"
#include "Win32ScreenLock.h"
#include "Win32TrayIcon.h"
#include "Win32Taskbar.h"
#include "Theme.h"
#include "resource.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif
#include "tk/audio_playback.h"
#include "tk/i18n.h"
#include "app/jxl_decode.h"
#include "tk/image_sniff.h"
#include "tk/status_icons.h"
#include "tk/svg.h" // Gdiplus comes in via Theme.h above (NOMINMAX-safe)
#include "platform/video_decode.h"

#include <tesseract/account_session.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

#include <commdlg.h>
#include <cmath>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <string>

namespace
{

std::wstring utf8_to_wstr(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0)
    {
        return {};
    }
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string wstr_to_utf8(const wchar_t* w)
{
    if (!w || !*w)
    {
        return {};
    }
    int n =
        WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
    {
        return {};
    }
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Builds an in-memory DLGTEMPLATE and shows a modal password-entry dialog.
// Returns the entered passphrase, or an empty string if cancelled.
struct PassphraseCtx_ { HWND edit = nullptr; std::wstring result; };

INT_PTR CALLBACK PassphraseDlgProc_(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* ctx = reinterpret_cast<PassphraseCtx_*>(
        GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        ctx = reinterpret_cast<PassphraseCtx_*>(lp);
        ctx->edit = GetDlgItem(dlg, 100);
        SetFocus(ctx->edit);
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && ctx)
        {
            wchar_t buf[512]{};
            GetWindowTextW(ctx->edit, buf, 512);
            ctx->result = buf;
            SecureZeroMemory(buf, sizeof(buf));
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::wstring prompt_passphrase_w32(HWND parent, const wchar_t* title)
{
    alignas(DWORD) uint8_t tmpl[1024]{};
    uint8_t* p = tmpl;

    auto w16  = [&](WORD v)         { memcpy(p, &v, 2); p += 2; };
    auto wstr = [&](const wchar_t* s)
    {
        size_t n = (wcslen(s) + 1) * 2;
        memcpy(p, s, n);
        p += n;
    };
    auto align4 = [&]()
    {
        auto off = static_cast<uintptr_t>(p - tmpl);
        if (off & 2) { w16(0); }
    };

    // DLGTEMPLATE header (18 bytes).
    auto* dt = reinterpret_cast<DLGTEMPLATE*>(p);
    dt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    dt->dwExtendedStyle = 0;
    dt->cdit = 3;
    dt->x = 0; dt->y = 0; dt->cx = 200; dt->cy = 58;
    p += sizeof(DLGTEMPLATE);

    w16(0);          // no menu
    w16(0);          // predefined window class
    wstr(title);     // dialog title
    w16(9);          // DS_SETFONT: 9pt
    wstr(L"Segoe UI");
    align4();

    auto item = [&](DWORD style, WORD id, short x, short y, short cx, short cy,
                    WORD cls_atom, const wchar_t* text)
    {
        auto* di = reinterpret_cast<DLGITEMTEMPLATE*>(p);
        di->style = style;
        di->dwExtendedStyle = 0;
        di->id = id;
        di->x = x; di->y = y; di->cx = cx; di->cy = cy;
        p += sizeof(DLGITEMTEMPLATE);
        w16(0xFFFF); w16(cls_atom);
        wstr(text);
        w16(0);      // no creation data
        align4();
    };

    item(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
         100, 4, 4, 192, 14, 0x0081, L"");
    item(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, IDOK, 96, 40, 46, 14, 0x0080, utf8_to_wstr(tk::tr("OK")).c_str());
    item(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, IDCANCEL, 146, 40, 50, 14, 0x0080, utf8_to_wstr(tk::tr("Cancel")).c_str());

    PassphraseCtx_ ctx;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                            reinterpret_cast<LPCDLGTEMPLATEW>(tmpl),
                            parent, PassphraseDlgProc_,
                            reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

} // namespace

namespace win32
{

// ---------------------------------------------------------------------------
// Posted-payload types — shared between worker threads and the UI wnd_proc.
// Lives near the top of the TU so wnd_proc's WM_* cases can reinterpret_cast
// from LPARAM without forward declarations.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Status-bar WndProc — flat custom-painted strip replacing the comctl32
// STATUSCLASSNAMEW (which carries a 9x-style size grip and chunky borders).
// Stores the latest text via WM_SETTEXT / SB_SETTEXTW so existing callers
// using either message continue to work.
// SB_SET_INFLIGHT_COLOR (WM_USER+2): wParam = COLORREF for the dot indicator.
// ---------------------------------------------------------------------------

static constexpr UINT SB_SET_INFLIGHT_COLOR = WM_USER + 2;

namespace
{

// Hyperlink state for the status bar. Status text may carry markdown-style
// "[label](url)" spans (see app/status_links.h); the wnd proc parses them on
// WM_SETTEXT and paints link runs in accent + underline. `hits` holds the
// painted link rects (client coords), rebuilt on every WM_PAINT, so cursor
// and click hit-testing always match what is on screen.
struct StatusBarLinkHit
{
    RECT rc;
    std::string url;
};
struct StatusBarLinkState
{
    std::vector<tesseract::StatusSegment> segs;
    std::vector<std::wstring> wide; // per-segment text, pre-widened
    bool has_links = false;
    std::vector<StatusBarLinkHit> hits;
};

// Draw the low-power (Lucide battery-low) glyph, tinted to `color`, centred in
// `box`. Caches the rasterized GDI+ bitmap keyed by size+colour so it is not
// re-rasterized on every WM_PAINT.
void draw_low_power_glyph(HDC hdc, RECT box, COLORREF color)
{
    const int px = std::min<LONG>(box.right - box.left, box.bottom - box.top);
    if (px <= 0)
        return;

    struct Slot
    {
        std::vector<std::uint8_t> pixels;
        std::unique_ptr<Gdiplus::Bitmap> bmp;
        int px = -1;
        COLORREF color = CLR_INVALID;
    };
    static Slot slot;

    if (!slot.bmp || slot.px != px || slot.color != color)
    {
        const tk::Color tint{GetRValue(color), GetGValue(color),
                             GetBValue(color), 255};
        std::vector<std::uint8_t> rgba =
            tk::rasterize_svg_rgba(tk::low_power_icon_svg(), px, tint);
        if (rgba.empty())
            return;
        // nanosvg gives straight-alpha R,G,B,A; GDI+ PixelFormat32bppARGB wants
        // straight alpha in B,G,R,A order.
        for (std::size_t i = 0; i + 2 < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
        slot.pixels = std::move(rgba);
        slot.bmp = std::make_unique<Gdiplus::Bitmap>(
            px, px, px * 4, PixelFormat32bppARGB, slot.pixels.data());
        slot.px = px;
        slot.color = color;
    }
    if (!slot.bmp)
        return;
    Gdiplus::Graphics gfx(hdc);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    const int x = box.left + (box.right - box.left - px) / 2;
    const int y = box.top + (box.bottom - box.top - px) / 2;
    gfx.DrawImage(slot.bmp.get(), x, y, px, px);
}

void status_bar_update_links(HWND hwnd, const std::wstring& text)
{
    auto* st =
        static_cast<StatusBarLinkState*>(GetPropW(hwnd, L"TesseractStatusLinks"));
    if (!st)
    {
        st = new StatusBarLinkState;
        SetPropW(hwnd, L"TesseractStatusLinks", st);
    }
    // The owning window decides whether this message may carry links: only a
    // message that opted in does (ShellBase::show_status_message_ allow_links),
    // so server or error text can never become a clickable link. Without an
    // owner the text stays plain.
    const std::string utf8 = wstr_to_utf8(text.c_str());
    if (auto* owner = static_cast<MainWindow*>(GetPropW(hwnd, L"TesseractStatusOwner")))
        st->segs = owner->status_segments(utf8);
    else
        st->segs = {{utf8, std::string{}}};
    st->has_links = tesseract::status_has_links(st->segs);
    st->wide.clear();
    st->hits.clear();
    if (st->has_links)
    {
        st->wide.reserve(st->segs.size());
        for (const auto& s : st->segs)
        {
            st->wide.push_back(utf8_to_wstr(s.text));
        }
    }
}

} // namespace

LRESULT CALLBACK status_bar_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCREATE:
        SetPropW(hwnd, L"TesseractStatusText", nullptr);
        SetPropW(hwnd, L"TesseractStatusDot",
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(RGB(0x40, 0xBF, 0x4D))));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NCDESTROY:
    {
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        delete p;
        auto* st = static_cast<StatusBarLinkState*>(
            GetPropW(hwnd, L"TesseractStatusLinks"));
        delete st;
        RemovePropW(hwnd, L"TesseractStatusText");
        RemovePropW(hwnd, L"TesseractStatusLinks");
        RemovePropW(hwnd, L"TesseractStatusDot");
        RemovePropW(hwnd, L"StatusTip");
        RemovePropW(hwnd, L"TesseractStatusOwner");
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    {
        if (msg == WM_LBUTTONUP)
        {
            // Hyperlink click — consume before the tooltip relay.
            auto* st = static_cast<StatusBarLinkState*>(
                GetPropW(hwnd, L"TesseractStatusLinks"));
            if (st && st->has_links)
            {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                for (const auto& h : st->hits)
                {
                    if (PtInRect(&h.rc, pt))
                    {
                        tesseract::Client::open_in_browser(h.url);
                        return 0;
                    }
                }
            }
            // Not a link click — a plain click anywhere else on the bar
            // triggers the current persistent status's action, if any
            // (today: jumping back to an in-progress history export).
            // No-op when nothing has claimed the persistent-status slot.
            if (auto* owner = static_cast<MainWindow*>(GetPropW(hwnd, L"TesseractStatusOwner")))
                owner->trigger_persistent_status_click_();
        }
        if (HWND tip = static_cast<HWND>(GetPropW(hwnd, L"StatusTip")))
        {
            MSG relay{hwnd, msg, wParam, lParam};
            SendMessageW(tip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay));
        }
        break;
    }
    case WM_SETCURSOR:
    {
        auto* st = static_cast<StatusBarLinkState*>(
            GetPropW(hwnd, L"TesseractStatusLinks"));
        if (st && st->has_links)
        {
            const DWORD mp = GetMessagePos();
            POINT pt{GET_X_LPARAM(mp), GET_Y_LPARAM(mp)};
            ScreenToClient(hwnd, &pt);
            for (const auto& h : st->hits)
            {
                if (PtInRect(&h.rc, pt))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;
    }
    case SB_SET_INFLIGHT_COLOR:
    {
        SetPropW(hwnd, L"TesseractStatusDot",
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
                     static_cast<COLORREF>(wParam))));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTEXT:
    case SB_SETTEXTW:
    {
        const wchar_t* txt = reinterpret_cast<const wchar_t*>(lParam);
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (!p)
        {
            p = new std::wstring;
            SetPropW(hwnd, L"TesseractStatusText", p);
        }
        p->assign(txt ? txt : L"");
        status_bar_update_links(hwnd, *p);
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;
    }
    case SB_SETTEXTA:
    {
        const char* txt = reinterpret_cast<const char*>(lParam);
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (!p)
        {
            p = new std::wstring;
            SetPropW(hwnd, L"TesseractStatusText", p);
        }
        if (txt && *txt)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, txt, -1, nullptr, 0);
            p->resize(n > 0 ? static_cast<std::size_t>(n - 1) : 0u, L'\0');
            if (n > 0)
            {
                MultiByteToWideChar(CP_ACP, 0, txt, -1, p->data(), n);
            }
        }
        else
        {
            p->clear();
        }
        status_bar_update_links(hwnd, *p);
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;
    }
    case WM_SIZE:
        // The inflight dot/spinner and the status text are both
        // right-anchored (see the WM_PAINT case below). Without
        // CS_HREDRAW, a width change during a live resize drag only
        // auto-invalidates the newly-exposed sliver — the old dot/text
        // position, now stranded mid-bar, is left un-repainted and ghosts
        // until the drag ends. Same fix as CustomTitleBar's buttons.
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        const auto& pal = theme::palette();
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Solid background + 1px top separator.
        HBRUSH bg = theme::brush(pal.chrome_bg);
        FillRect(hdc, &rc, bg);
        RECT top = {rc.left, rc.top, rc.right, rc.top + 1};
        FillRect(hdc, &top, theme::brush(pal.separator));

        const UINT sb_dpi = GetDpiForWindow(hwnd);
        const bool low_power_glyph_on =
            GetPropW(hwnd, L"TesseractLowPower") != nullptr;
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (p && !p->empty())
        {
            SetBkMode(hdc, TRANSPARENT);
            HFONT small_font = theme::font(theme::FontRole::Small);
            // Reserve space on the right for the inflight dot (DPI-scaled), plus
            // a little more for the low-power glyph when it is showing.
            RECT text_rc = {
                rc.left + MulDiv(10, sb_dpi, 96), rc.top,
                rc.right - MulDiv(low_power_glyph_on ? 40 : 24, sb_dpi, 96),
                rc.bottom};
            auto* st = static_cast<StatusBarLinkState*>(
                GetPropW(hwnd, L"TesseractStatusLinks"));
            if (st && st->has_links)
            {
                // Segmented paint: plain runs in text_secondary, link runs in
                // accent + underline. Hit rects are rebuilt to match exactly
                // what is painted (clamped to the visible text area).
                st->hits.clear();
                LOGFONTW lf{};
                GetObjectW(small_font, sizeof(lf), &lf);
                lf.lfUnderline = TRUE;
                HFONT link_font = CreateFontIndirectW(&lf);
                HFONT old = (HFONT)SelectObject(hdc, small_font);
                int x = text_rc.left;
                for (std::size_t i = 0;
                     i < st->segs.size() && x < text_rc.right; ++i)
                {
                    const std::wstring& run = st->wide[i];
                    if (run.empty())
                    {
                        continue;
                    }
                    const bool is_link = !st->segs[i].url.empty();
                    SelectObject(hdc, is_link && link_font ? link_font
                                                           : small_font);
                    SetTextColor(hdc,
                                 is_link ? pal.accent : pal.text_secondary);
                    SIZE sz{};
                    GetTextExtentPoint32W(hdc, run.c_str(),
                                          static_cast<int>(run.size()), &sz);
                    const bool overflow = x + sz.cx > text_rc.right;
                    RECT run_rc = {x, text_rc.top, text_rc.right,
                                   text_rc.bottom};
                    DrawTextW(hdc, run.c_str(), static_cast<int>(run.size()),
                              &run_rc,
                              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                                  (overflow ? DT_END_ELLIPSIS : 0));
                    if (is_link)
                    {
                        RECT hit = {x, text_rc.top,
                                    std::min<LONG>(x + sz.cx, text_rc.right),
                                    text_rc.bottom};
                        st->hits.push_back({hit, st->segs[i].url});
                    }
                    if (overflow)
                    {
                        break;
                    }
                    x += sz.cx;
                }
                SelectObject(hdc, old);
                if (link_font)
                {
                    DeleteObject(link_font);
                }
            }
            else
            {
                SetTextColor(hdc, pal.text_secondary);
                HFONT old = (HFONT)SelectObject(hdc, small_font);
                DrawTextW(hdc, p->c_str(), -1, &text_rc,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                              DT_NOPREFIX);
                SelectObject(hdc, old);
            }
        }
        // Draw the inflight dot and optional spinning ring.
        {
            const COLORREF dot_cr = static_cast<COLORREF>(reinterpret_cast<ULONG_PTR>(
                GetPropW(hwnd, L"TesseractStatusDot")));
            const int   DOT_R = static_cast<int>(std::lround(tk::kInflightDotR * sb_dpi / 96.0f));
            const int   cx    = rc.right - static_cast<int>((tk::kInflightOrbitR + tk::kInflightRingDotR + 4.0f) * sb_dpi / 96.0f);
            const int   cy    = (rc.top + rc.bottom) / 2;
            const float orbit = tk::kInflightOrbitR * sb_dpi / 96.0f;
            const float rdot  = tk::kInflightRingDotR * sb_dpi / 96.0f;

            // Spinning ring — drawn first so the center dot paints on top.
            const auto phase_enc = static_cast<uint32_t>(reinterpret_cast<ULONG_PTR>(
                GetPropW(hwnd, L"TesseractStatusPhase")));
            if (phase_enc > 0u)
            {
                const float phase = static_cast<float>(phase_enc - 1u) / 65535.0f;
                constexpr int kN = 8;
                HPEN null_pen = CreatePen(PS_NULL, 0, 0);
                HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, null_pen));
                for (int i = 0; i < kN; ++i)
                {
                    const float angle =
                        (i / static_cast<float>(kN) + phase) * 2.0f * 3.14159265f;
                    const float dx = std::cos(angle) * orbit;
                    const float dy = std::sin(angle) * orbit;
                    const float t  = static_cast<float>(i) / kN;
                    const BYTE  a  = static_cast<BYTE>(40 + 215 * t);
                    // Ring color 0xA0A0A6 blended with per-dot alpha.
                    const BYTE r = static_cast<BYTE>(0xA0u * a / 255u);
                    const BYTE g = static_cast<BYTE>(0xA0u * a / 255u);
                    const BYTE b = static_cast<BYTE>(0xA6u * a / 255u);
                    HBRUSH rb = CreateSolidBrush(RGB(r, g, b));
                    HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, rb));
                    const int rx = static_cast<int>(cx + dx - rdot);
                    const int ry = static_cast<int>(cy + dy - rdot);
                    const int rr = static_cast<int>(rdot * 2.0f);
                    Ellipse(hdc, rx, ry, rx + rr, ry + rr);
                    SelectObject(hdc, ob);
                    DeleteObject(rb);
                }
                SelectObject(hdc, old_pen);
                DeleteObject(null_pen);
            }

            // Center dot — on top of the ring.
            HBRUSH dot_br  = CreateSolidBrush(dot_cr);
            HPEN   null_pen = CreatePen(PS_NULL, 0, 0);
            HBRUSH old_br  = static_cast<HBRUSH>(SelectObject(hdc, dot_br));
            HPEN   old_pen = static_cast<HPEN>(SelectObject(hdc, null_pen));
            Ellipse(hdc, cx - DOT_R, cy - DOT_R, cx + DOT_R, cy + DOT_R);
            SelectObject(hdc, old_br);
            SelectObject(hdc, old_pen);
            DeleteObject(dot_br);
            DeleteObject(null_pen);
        }

        // Low-power glyph — the Lucide battery-low icon just left of the dot.
        if (low_power_glyph_on)
        {
            const int gpx = MulDiv(14, sb_dpi, 96);
            const int cy = (rc.top + rc.bottom) / 2;
            RECT glyph_rc = {rc.right - MulDiv(38, sb_dpi, 96), cy - gpx / 2,
                             rc.right - MulDiv(38, sb_dpi, 96) + gpx,
                             cy - gpx / 2 + gpx};
            draw_low_power_glyph(hdc, glyph_rc, pal.text_secondary);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void register_status_bar_class(HINSTANCE hInst)
{
    static bool registered = false;
    if (registered)
    {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = status_bar_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TesseractStatusBar";
    RegisterClassExW(&wc);
    registered = true;
}

void MainWindow::apply_default_font(HWND h)
{
    if (h)
    {
        SendMessageW(h, WM_SETFONT,
                     reinterpret_cast<WPARAM>(theme::font(theme::FontRole::Ui)),
                     TRUE);
    }
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
    win32::theme::refresh_from_system();
    return win32::theme::current_mode() == win32::theme::Mode::Dark
               ? tk::ThemeMode::Dark
               : tk::ThemeMode::Light;
}

namespace
{
// COLORREF (0x00BBGGRR) -> tk::Color::rgb(0xRRGGBB).
constexpr tk::Color cr_to_tk(COLORREF c)
{
    return tk::Color::rgba(GetRValue(c), GetGValue(c), GetBValue(c), 255);
}
} // namespace

std::optional<tk::Color> MainWindow::os_accent_color_() const
{
    return cr_to_tk(win32::theme::accent_colorref());
}

void MainWindow::apply_fonts_ui_()
{
    // Drop the GDI fonts built from the old config, then hand the rebuilt ones
    // to the native controls that hold an HFONT.
    win32::theme::on_fonts_changed();
    apply_default_font(hStatus_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (hStatus_)
        InvalidateRect(hStatus_, nullptr, TRUE);
}

void MainWindow::apply_native_theme_(const tk::Theme& t)
{
    win32::theme::set_mode(t.mode == tk::ThemeMode::Dark
                               ? win32::theme::Mode::Dark
                               : win32::theme::Mode::Light);
    auto cref = [](const tk::Color& c)
    { return RGB(c.r, c.g, c.b); };
    win32::theme::set_accent({cref(t.palette.accent),
                              cref(t.palette.accent_hover),
                              cref(t.palette.accent_pressed),
                              cref(t.palette.text_on_accent)});
    win32::theme::apply_window_attributes(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (hStatus_)
    {
        InvalidateRect(hStatus_, nullptr, TRUE);
    }
}

void MainWindow::for_each_shell_surface_(
    const std::function<void(tk::SurfaceBase&)>& fn)
{
    if (branding_surface_)
        fn(*branding_surface_);
    if (main_app_surface_)
        fn(*main_app_surface_);
}

void MainWindow::on_system_theme_changed()
{
    if (!theme::refresh_from_system())
    {
        return;
    }
    if (tesseract::Settings::instance().theme_pref ==
        tesseract::Settings::ThemePreference::System)
    {
        apply_current_theme_();
    }
}

void MainWindow::paint_main_background(HDC hdc, const RECT& rc)
{
    const auto& pal = theme::palette();
    FillRect(hdc, &rc, theme::brush(pal.window_bg));
}

// ---------------------------------------------------------------------------
// EventHandlerBase UI-thread hook overrides (Win32)
// ---------------------------------------------------------------------------
// All marshalling is now handled by EventHandlerBase::post_to_ui_ which posts
// WM_TESSERACT_POST_TO_UI with a heap-allocated std::function<void()>. These
// methods run directly on the UI thread.

// handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
// are inherited from ShellBase (which drives the same room_view_ and dispatches
// to secondary windows). See MainWindow.h for the rationale.

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                       std::string description,
                                       bool soft_logout)
{
    // Agnostic state machine lives in ShellBase; this shell only supplies the
    // native restart timer (post_to_ui_after_), status bar, user strip
    // (refresh_user_strip_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status_();
    on_inflight_ui_();
}

bool MainWindow::tray_available_() const
{
    return tray_ && tray_->is_available();
}

void MainWindow::on_inflight_ui_()
{
    if (!hStatus_)
        return;
    const auto   c  = inflight_dot_color_();
    SendMessageW(hStatus_, SB_SET_INFLIGHT_COLOR,
                 static_cast<WPARAM>(RGB(c.r, c.g, c.b)), 0);
    // Update ring phase property; 0 = no ring, 1..65536 = ring + encoded phase.
    const uint32_t phase_enc = inflight_needs_anim_()
        ? static_cast<uint32_t>(inflight_spin_phase_() * 65535.0f) + 1u
        : 0u;
    SetPropW(hStatus_, L"TesseractStatusPhase",
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(phase_enc)));
    if (hStatusTip_)
    {
        inflight_tip_text_ = utf8_to_wstr(inflight_tooltip_text_());
        if (low_power_active_win_)
        {
            inflight_tip_text_.insert(
                0, utf8_to_wstr(tk::tr(
                       "Low power mode — background sync paused")) +
                       L"\n");
        }
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        ti.hwnd = hStatus_;
        ti.uId = reinterpret_cast<UINT_PTR>(hStatus_);
        ti.lpszText = inflight_tip_text_.data();
        SendMessageW(hStatusTip_, TTM_UPDATETIPTEXTW, 0,
                     reinterpret_cast<LPARAM>(&ti));
    }
}

void MainWindow::on_low_power_mode_ui_(bool active)
{
    low_power_active_win_ = active;
    if (hStatus_)
    {
        SetPropW(hStatus_, L"TesseractLowPower",
                 active ? reinterpret_cast<HANDLE>(1) : nullptr);
        InvalidateRect(hStatus_, nullptr, TRUE);
    }
    // Refresh the shared status-bar tooltip text (prepends / drops the note).
    on_inflight_ui_();
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    if (hStatus_)
    {
        SetWindowTextW(hStatus_, utf8_to_wstr(msg).c_str());
        UpdateWindow(hStatus_);
    }
}

void MainWindow::on_restore_status_ui_()
{
    refresh_sync_status_();
}

void MainWindow::on_startup_restore_progress_ui_(const std::string& status)
{
    if (branding_view_)
    {
        branding_view_->set_status(status);
    }
}

// ---------------------------------------------------------------------------
// wnd_proc
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    if (msg == WM_GETMINMAXINFO)
    {
        // Enforce kMinWindowWidth (derived from the compose bar's own
        // button footprint — see visual.h) regardless of self, since this
        // can fire before WM_NCCREATE sets GWLP_USERDATA.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.f : 1.f;
        mmi->ptMinTrackSize.x = static_cast<LONG>(
            std::round(tesseract::visual::kMinWindowWidth * scale));
        mmi->ptMinTrackSize.y = static_cast<LONG>(
            std::round(tesseract::visual::kMinWindowHeight * scale));
        return 0;
    }

    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == self->taskbar_.taskbar_button_created_message())
    {
        self->taskbar_.on_taskbar_button_created(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        self->on_create(hwnd);
        return 0;

    // ── Custom extended title bar (see CustomTitleBar.h) ───────────────────
    case WM_NCCALCSIZE:
        if (wParam == TRUE && !self->fs_active_)
        {
            self->title_bar_.adjust_nccalcsize(hwnd, wParam, lParam);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCHITTEST:
    {
        if (auto dwm = win32::CustomTitleBar::dwm_hittest_hook(hwnd, msg,
                                                                wParam, lParam))
            return *dwm;
        const LRESULT ht = self->title_bar_.handle_nchittest(hwnd, lParam);
        if (ht != HTNOWHERE)
            return ht;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCMOUSEMOVE:
        self->title_bar_.handle_ncmousemove(hwnd, wParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCMOUSELEAVE:
        self->title_bar_.handle_ncmouseleave(hwnd);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCLBUTTONDOWN:
    {
        if (auto dwm = win32::CustomTitleBar::dwm_hittest_hook(hwnd, msg,
                                                                wParam, lParam))
            return *dwm;
        if (self->title_bar_.handle_nclbuttondown(hwnd, wParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCLBUTTONUP:
    {
        if (auto dwm = win32::CustomTitleBar::dwm_hittest_hook(hwnd, msg,
                                                                wParam, lParam))
            return *dwm;
        if (self->title_bar_.handle_nclbuttonup(hwnd, wParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCRBUTTONDOWN:
        if (auto dwm = win32::CustomTitleBar::dwm_hittest_hook(hwnd, msg,
                                                                wParam, lParam))
            return *dwm;
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCRBUTTONUP:
    {
        if (auto dwm = win32::CustomTitleBar::dwm_hittest_hook(hwnd, msg,
                                                                wParam, lParam))
            return *dwm;
        if (self->title_bar_.show_system_menu(hwnd, wParam, lParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_CLOSE:
        // Only the window that owns the app-wide tray icon consults
        // CloseAction — spawned per-account windows have no tray_ and always
        // close for real below. HideToTray/Minimize are intercepted here;
        // Quit falls through to DefWindowProc so the window is destroyed.
        // Hiding is only reachable when the tray icon actually exists, so
        // CloseAction::HideToTray can never strand the app unreachable.
        if (self->tray_ && self->tray_->is_available() && !self->quitting_)
        {
            switch (tesseract::Settings::instance().close_action)
            {
            case tesseract::Settings::CloseAction::HideToTray:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case tesseract::Settings::CloseAction::Minimize:
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            case tesseract::Settings::CloseAction::Quit:
                break;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam); // → WM_DESTROY

    case WM_DESTROY:
        self->on_destroy();
        // Hand this window's account bridge back to the primary, release its
        // dedicated mapping and tray ownership (multi-window), then unregister.
        self->on_window_closing_();
        self->account_manager_.unregister_window(self);
        if (self->account_manager_.window_count() == 0)
            PostQuitMessage(0);
        return 0;

    case WM_ACTIVATE:
    {
        const bool active = LOWORD(wParam) != WA_INACTIVE;
        if (active)
        {
            FLASHWINFO fwi{sizeof(fwi), hwnd, FLASHW_STOP, 0, 0};
            FlashWindowEx(&fwi);
            self->tray_owner_considered_active_ = true;
        }
        else if (!MainWindow::is_explorer_foreground_(GetForegroundWindow()))
        {
            self->tray_owner_considered_active_ = false;
        }
        if (self->is_active_ != active)
        {
            self->is_active_ = active;
            RECT title_rc{};
            GetClientRect(hwnd, &title_rc);
            title_rc.bottom = self->title_bar_.height_px();
            InvalidateRect(hwnd, &title_rc, TRUE);
        }
        theme::apply_border_color(hwnd, active);
        // Clicking the app's own popup (the account picker) activates it; the
        // window is not being left, and closing the popup now would drop the
        // click before it lands.
        if (active || !tk::win32::is_popup_surface_hwnd(reinterpret_cast<HWND>(lParam)))
            self->notify_window_active_(active);
        DefWindowProcW(hwnd, msg, wParam, lParam);
        // DefWindowProc restores focus to whatever GetFocus() returned at
        // WM_ACTIVATE(WA_INACTIVE) time. If no focused child was recorded
        // (e.g. first activation before the compose bar had ever been shown)
        // DefWindowProc silently leaves focus on the D2D surface HWND or
        // the main window rather than the compose bar. Detect this case and
        // redirect explicitly: any HWND that is not a native input overlay
        // (text field / area) means the restore fell through to a canvas.
        if (active && self->room_text_area_ &&
            self->room_text_area_->visible() &&
            !self->current_room_id_.empty())
        {
            HWND focused = GetFocus();
            HWND surface = self->main_app_surface_
                               ? self->main_app_surface_->hwnd()
                               : nullptr;
            if (focused == nullptr || focused == hwnd || focused == surface)
                self->room_text_area_->set_focused(true);
        }
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            self->on_size(LOWORD(lParam), HIWORD(lParam));
            RECT wrc{};
            GetWindowRect(hwnd, &wrc);
            auto& g = tesseract::Settings::instance().main_window_geometry;
            g.x = wrc.left; g.y = wrc.top;
            g.w = wrc.right - wrc.left; g.h = wrc.bottom - wrc.top;
            g.dpi = static_cast<int>(GetDpiForWindow(hwnd));
            g.valid = true;
            self->save_settings_debounced_();
            self->start_anim_tick_();   // restart after restore from minimized
        }
        self->update_video_playback_suspension_();
        return 0;

    case WM_SHOWWINDOW:
        if (wParam)   // window being shown (e.g. restored from tray)
            self->start_anim_tick_();
        self->update_video_playback_suspension_();
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_MOVE:
    {
        if (!IsIconic(hwnd))
        {
            RECT wrc{};
            GetWindowRect(hwnd, &wrc);
            auto& g = tesseract::Settings::instance().main_window_geometry;
            g.x = wrc.left; g.y = wrc.top;
            g.w = wrc.right - wrc.left; g.h = wrc.bottom - wrc.top;
            g.dpi = static_cast<int>(GetDpiForWindow(hwnd));
            g.valid = true;
            self->save_settings_debounced_();
        }
        return 0;
    }

    case WM_COMMAND:
        if (self->taskbar_.handle_thumbnail_command(hwnd, wParam))
            return 0;
        // Compose bar Send / Emoji clicks go through the shared widgets'
        // callbacks now — no WM_COMMAND wiring. The emoji-picker search
        // field is a NativeTextField overlay handled by its set_on_changed
        // lambda. User context menu items invoke callbacks directly in
        // show_user_context_menu_ (no PostMessageW).
        // Registry shortcuts (see the accelerator table built in on_create):
        // forward the chord's KeyEvent; MainAppWidget decides what it does.
        if (const int idx = LOWORD(wParam) - IDC_SHORTCUT_BASE;
            idx >= 0 && idx < static_cast<int>(self->accel_events_.size()))
        {
            if (self->main_app_)
                self->main_app_->dispatch_key_down(
                    self->accel_events_[static_cast<std::size_t>(idx)]);
        }
        return 0;

    case WM_ERASEBKGND:
    {
        // The parent paints behind any pixel a child doesn't cover. Use the
        // themed window background — also keeps Mica fade-in clean during
        // resize, since the OS lerps from our colour to the new layout.
        //
        // paint_main_background() and title_bar_.paint() are two separate
        // GDI draws — DWM's compositor runs on its own asynchronous
        // schedule and can (confirmed live, via debugger) sample this
        // window's redirection surface *between* them, showing the plain
        // background for one composited frame before the title bar content
        // lands, even though no message is dispatched between the two
        // calls. Double-buffer: paint both into an off-screen DC first and
        // blit the finished frame across in one atomic call, so the real
        // window DC only ever holds a complete image.
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp =
            CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);
        self->paint_main_background(mem_dc, rc);
        if (!self->fs_active_)
            self->title_bar_.paint(mem_dc, hwnd, rc, self->is_active_);
        BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
              mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
        return 1;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    {
        // Theme any flat children that don't owner-draw themselves: status
        // colours on the EDIT controls.
        const auto& pal = theme::palette();
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        // The compose bar is now a tk::win32::Surface — it paints its own
        // background; no WM_CTLCOLOR tinting needed.
        // EDIT controls → compose-card bg.
        if (msg == WM_CTLCOLOREDIT)
        {
            SetTextColor(dc, pal.text_primary);
            SetBkColor(dc, pal.compose_card_bg);
            return reinterpret_cast<LRESULT>(theme::brush(pal.compose_card_bg));
        }
        SetTextColor(dc, pal.text_primary);
        SetBkColor(dc, pal.window_bg);
        return reinterpret_cast<LRESULT>(theme::brush(pal.window_bg));
    }

    case WM_SYSCOLORCHANGE:
        // High Contrast scheme edits change the system colors the title bar
        // paints with.
        theme::apply_window_attributes(hwnd);
        self->title_bar_.invalidate_strip(hwnd);
        return 0;

    case WM_SETTINGCHANGE:
    {
        if (wParam == SPI_SETHIGHCONTRAST)
        {
            theme::refresh_high_contrast();
            theme::apply_window_attributes(hwnd);
            self->title_bar_.invalidate_strip(hwnd);
            return 0;
        }
        if (wParam == SPI_SETNONCLIENTMETRICS)
        {
            self->title_bar_.on_system_metrics_changed();
            self->title_bar_.invalidate_strip(hwnd);
            return 0;
        }
        // Watch for OS dark/light flip. lParam carries the changed area name
        // as a wide string when it originated in Personalize. The accent
        // color picker in the same Personalize page also broadcasts
        // "ImmersiveColorSet" (on_system_theme_changed() alone swallows an
        // accent-only change, since refresh_from_system() only reports a
        // change when the light/dark mode itself flipped) so check the
        // accent too.
        if (lParam)
        {
            auto* name = reinterpret_cast<const wchar_t*>(lParam);
            if (name && wcscmp(name, L"ImmersiveColorSet") == 0)
            {
                const bool bars_changed =
                    theme::refresh_accent_on_title_bars();
                self->on_system_theme_changed();
                self->on_system_accent_changed_();
                if (bars_changed)
                {
                    self->apply_theme_ui_(self->current_theme_);
                }
            }
        }
        return 0;
    }

    case WM_DWMCOLORIZATIONCOLORCHANGED:
        // Fires when the user changes their Windows accent color in
        // Settings > Personalization > Colors. Ignore wParam (the new
        // colorization color) and re-read AccentColorMenu via
        // accent_colorref(), the source it prefers.
        {
            const bool bars_changed = theme::refresh_accent_on_title_bars();
            self->on_system_accent_changed_();
            if (bars_changed)
            {
                self->apply_theme_ui_(self->current_theme_);
            }
        }
        return 0;

    case WM_DPICHANGED:
    {
        theme::on_dpi_changed();
        self->title_bar_.on_dpi_changed(LOWORD(wParam));
        // Move+resize to the rect Windows calculated for the new DPI.
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        // Push the new scale into every surface anchored to *this* window
        // (branding/main-app/settings/login — all child HWNDs of
        // hwnd_, so they share its monitor/DPI) so native-control image
        // captures (tk::NativeTextField/NativeTextArea) don't stay
        // stale/blurry. Deliberately NOT propagated to call_window_ or
        // owned_secondary_windows_ (RoomWindow popouts) — unlike theme,
        // which is a single global setting broadcast everywhere, scale is
        // per-monitor: those are independent top-level windows the user can
        // drag to a different-DPI monitor on their own, and each already
        // gets its own WM_DPICHANGED for that.
        const float scale = static_cast<float>(LOWORD(wParam)) / 96.0f;
        if (self->branding_surface_)
            self->branding_surface_->apply_scale_change(scale);
        if (self->main_app_surface_)
            self->main_app_surface_->apply_scale_change(scale);
        if (self->settings_surface_)
            self->settings_surface_->apply_scale_change(scale);
        self->apply_scale_to_login_surface_(scale);
        return 0;
    }

    case WM_TESSERACT_NOTIFY_CLICK:
    {
        auto* payload = reinterpret_cast<win32::NotifyClickPayload*>(lParam);
        if (!payload->reply_text.empty())
        {
            // Reply submit. Unpackaged Win32 toasts only support foreground
            // activation, so the app is brought forward regardless — there
            // is no way to send a reply without also foregrounding here,
            // unlike macOS/KDE.
            self->send_notification_reply_(payload->user_id, payload->room_id,
                                           payload->event_id,
                                           payload->reply_text);
        }
        // Routes to the window that owns the account (may not be `self`),
        // which restores/raises itself via activate_for_notification_.
        self->on_notification_clicked_(payload->user_id, payload->room_id,
                                       payload->event_id, {});
        delete payload;
        return 0;
    }
    case WM_TESSERACT_POST_TO_UI:
    {
        // ShellBase::post_to_ui_ posts a heap-allocated std::function here.
        // Execute it on the UI thread and free it.
        auto* fn = reinterpret_cast<std::function<void()>*>(lParam);
        (*fn)();
        delete fn;
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && self->handle_escape_key_())
            return 0;
        if (wParam == 'K' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Character;
                event.text = "k";
                event.ctrl = true;
                self->main_app_->dispatch_key_down(event);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_TIMER:
        if (wParam == kAnimTimerId)
        {
            self->on_anim_tick();
            return 0;
        }
        if (wParam == kInflightTimerId)
        {
            self->inflight_tick_();
            return 0;
        }
        if (wParam == kPresenceTickTimerId)
        {
            // Periodic (30 s); auto-reschedules — do not kill.
            // Skip the sweep while the window is not visible (e.g. minimized).
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                self->notify_presence_tick_();
            return 0;
        }
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        if (wParam == kScreenshotStepTimerId)
        {
            KillTimer(hwnd, kScreenshotStepTimerId);
            if (auto step = std::exchange(self->screenshot_step_, nullptr))
                step();
            return 0;
        }
#endif
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (cds && cds->dwData == 1 /* matrix URI */ && cds->cbData > 0)
        {
            std::string uri(static_cast<const char*>(cds->lpData),
                            cds->cbData - 1);
            self->open_matrix_link(uri);
        }
        else if (cds && cds->dwData == 2 &&
                 cds->cbData == sizeof(tesseract::LaunchAction) && cds->lpData)
        {
            const auto action = *static_cast<const tesseract::LaunchAction*>(cds->lpData);
            // Room needs its ID and arrives as dwData == 3 instead.
            if (action > tesseract::LaunchAction::None &&
                action < tesseract::LaunchAction::Room)
                self->dispatch_launch_action_(action);
        }
        else if (cds && cds->dwData == 3 && cds->cbData > 1 && cds->lpData)
        {
            self->dispatch_launch_action_(
                tesseract::LaunchAction::Room,
                std::string(static_cast<const char*>(cds->lpData),
                            cds->cbData - 1));
        }
        return TRUE;
    }

    case WM_NCDESTROY:
    {
        // Last message this window receives. A window spawned by
        // spawn_main_window_ owns itself; the primary lives on main()'s stack.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        const LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        // The last window has no heir to take its account bridges, and SDK
        // callbacks would then dereference a freed window: keep it until the
        // process exits (the window count is 0 and the loop is quitting).
        if (self->delete_on_destroy_ && !self->is_account_bridge_target_())
        {
            delete self;
        }
        return r;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Enter-to-send is handled by the NativeTextArea overlay inside the
// shared ComposeBar — the legacy `input_subclass_proc` is no longer
// needed.

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

const wchar_t* MainWindow::class_name()
{
    static const std::wstring name = []
    {
        std::wstring n = L"TesseractMainWnd";
        for (char c : tesseract::profile_suffix())
            n.push_back(static_cast<wchar_t>(c));
        return n;
    }();
    return name.c_str();
}

bool MainWindow::register_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // No class brush: WM_ERASEBKGND handler fills with the themed window_bg.
    // This lets DWM Mica show through during the brief moments where a child
    // control hasn't yet repainted (resize) and avoids a stale-white flash on
    // dark-mode startup.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = class_name();
    // Big icon: Alt+Tab, taskbar. Small icon: titlebar, system menu.
    // LoadImage picks the best-matching frame from the multi-resolution .ico.
    wc.hIcon = static_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                   LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                   LR_DEFAULTCOLOR | LR_SHARED));
    if (!wc.hIcon)
    {
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    if (!wc.hIconSm)
    {
        wc.hIconSm = wc.hIcon;
    }
    return RegisterClassExW(&wc) != 0;
}

MainWindow::MainWindow(tesseract::AccountManager& account_manager, HINSTANCE hInst,
                       Win32Taskbar& taskbar, bool start_hidden,
                       tesseract::LaunchAction launch_action,
                       std::string launch_room_id
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                       , std::filesystem::path screenshot_dir
#endif
                       )
    : ShellBase(account_manager)
    , hInst_(hInst)
    , taskbar_(taskbar)
    , start_hidden_(start_hidden)
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    , screenshot_dir_(std::move(screenshot_dir))
#endif
{
    set_screen_lock_(std::make_unique<win32::Win32ScreenLock>(hInst));
    set_power_monitor_(std::make_unique<win32::Win32PowerMonitor>(hInst));
    set_autostart_(std::make_unique<win32::Win32Autostart>());

    // Held until show_main_content_() calls mark_main_content_ready_().
    dispatch_launch_action_(launch_action, std::move(launch_room_id));

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

MainWindow::~MainWindow()
{
    release_settings_surface_();
    // unregister_window is called in WM_DESTROY (before PostQuitMessage) so
    // it is not repeated here.  broadcast_rebuild_tray_ is still needed to
    // refresh any remaining windows after this C++ shell is freed.
    broadcast_rebuild_tray_();

    // Popup windows anchor to main_app_surface_: they go first.
    release_popups_();

    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the survivors.
    if (!is_pinned_window_)
    {
        for (auto& s : account_manager_.accounts())
        {
            if (s && s->client)
            {
                s->client->stop_sync();
            }
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->stop_sync();
    }
    // Release the login page while the client pointers it is bound to are
    // still alive.
    release_login_surface_();
    destroy_accelerators_();
    // Theme fonts and the icon cache are process-wide: a closing secondary
    // window must leave them to the windows that stay.
    if (!is_pinned_window_)
    {
        theme::shutdown();
    }
    if (gdiplus_token_)
    {
        if (!is_pinned_window_)
        {
            CustomTitleBar::shutdown_icon_cache();
        }
        Gdiplus::GdiplusShutdown(gdiplus_token_);
    }
}

bool MainWindow::create(int nCmdShow)
{
    hwnd_ = CreateWindowExW(0, class_name(), L"Tesseract",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr,
                            nullptr, hInst_, this);
    if (!hwnd_)
    {
        return false;
    }
    // WM_CREATE fires synchronously inside CreateWindowExW, which calls
    // on_create() → load_from_disk(), so saved geometry is available here.
    {
        auto g = tesseract::Settings::instance().main_window_geometry;
        // If the save DPI is known, rescale w/h to the target monitor's DPI
        // before clamping so the window opens at the right logical size even
        // when moved to a different-DPI screen or a Remote Desktop session.
        if (g.valid && g.dpi > 0)
        {
            HMONITOR hm = MonitorFromPoint({g.x, g.y}, MONITOR_DEFAULTTONEAREST);
            UINT targetDpi = 96, dummy = 0;
            GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &targetDpi, &dummy);
            if (static_cast<int>(targetDpi) != g.dpi)
            {
                const double s = double(targetDpi) / double(g.dpi);
                g.w = static_cast<int>(std::lround(g.w * s));
                g.h = static_cast<int>(std::lround(g.h * s));
            }
        }
        const auto geom = clamp_to_screens_(g, 1024, 768, get_screen_work_areas_());
        if (geom.valid)
        {
            SetWindowPos(hwnd_, nullptr, geom.x, geom.y, geom.w, geom.h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            SetWindowPos(hwnd_, nullptr, 0, 0, dip_to_phys(1024.f),
                         dip_to_phys(768.f),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // Force a WM_NCCALCSIZE recompute even if the geometry restore above
    // happened to be a no-op (e.g. no saved geometry and the fallback size
    // exactly matches the size already passed to CreateWindowExW, which
    // Windows treats as an unchanged size/position and never re-fires
    // WM_NCCALCSIZE for). Without this, the custom-titlebar-hiding logic in
    // CustomTitleBar::adjust_nccalcsize never runs before ShowWindow, and
    // the native caption stays visible alongside the custom one until the
    // window is resized.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::on_create(HWND hwnd)
{
    taskbar_.register_main_window(hwnd, [this] { navigate_tray_unread_(); });
    // Global-shortcut accelerator tables (see rebuild_accelerators_).
    rebuild_accelerators_();

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus_token_, &gsi, nullptr);
    HDC dpi_dc = GetDC(hwnd);
    UINT dpi = dpi_dc ? GetDeviceCaps(dpi_dc, LOGPIXELSY) : 96;
    if (dpi_dc)
    {
        ReleaseDC(hwnd, dpi_dc);
    }

    // Initialise theme + DWM attributes for the caption + Mica backdrop
    // before any child controls are created so the first paint already
    // reflects dark / light mode.
    if (!is_pinned_window_)
    {
        theme::register_main_window(hwnd);
    }
    theme::apply_window_attributes(hwnd);
    title_bar_.on_dpi_changed(dpi);
    title_bar_.attach(hwnd);

    // Single surface hosting the full MainAppWidget tree (sidebar + chat +
    // banners + lightbox overlays). The first Surface creation initialises the
    // D2D backend singleton which builds the Twemoji-first font fallback.
    {
        main_app_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hwnd, tk::Theme::light());

        main_app_surface_->set_root(create_main_app_());

        // Stay hidden until show_main_content_() reveals it (after startup
        // restore completes) — mirrors settings_surface_'s identical hide
        // right after its own creation, below. Without this, main_app_surface_
        // sits fully WS_VISIBLE (its default at HWND creation) for the entire
        // restore window, relying purely on branding_surface_ happening to
        // stack on top of it in z-order — fragile compared to Qt/GTK/macOS's
        // QStackedWidget-equivalent, which makes the two mutually exclusive
        // structurally rather than by convention.
        if (main_app_surface_->hwnd())
        {
            ShowWindow(main_app_surface_->hwnd(), SW_HIDE);
        }

        wire_main_surface_(static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);

        // 30 s periodic tick — paired with WM_TIMER below.
        SetTimer(hwnd, kPresenceTickTimerId, 30000, nullptr);

        tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

        create_main_room_pane_();

        // Provider wiring (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // UserInfo callbacks.
        main_app_->user_info()->on_primary = [this](tk::Point /*p*/)
        {
            open_account_picker_();
        };
        main_app_->user_info()->on_secondary = [this](tk::Point p)
        {
            POINT sp{dip_to_phys(p.x), dip_to_phys(p.y)};
            if (main_app_surface_ && main_app_surface_->hwnd())
            {
                ClientToScreen(main_app_surface_->hwnd(), &sp);
            }
            show_user_context_menu_(sp.x, sp.y);
        };

        // Mention-pill avatar provider + on_fetch_room_members (which
        // populates the cache it reads) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), using
        // RoomPane's own cached_room_members_/cached_members_room_ instead
        // of this window's now-removed copy of those fields.
        wire_room_view_services_();

        // on_send / on_send_reply / on_send_edit / on_send_image /
        // on_send_video / on_send_audio / on_send_file / on_send_gallery
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_), a
        // verbatim port of this window's old on_send body including the
        // composer mention-draft-to-markdown conversion. on_send_reply/
        // on_send_edit now report failures via an async status message
        // (RoomPane::send_reply_/send_edit_) — this window previously
        // discarded the SDK Result entirely for those two, so this is a
        // strict improvement, not a behavior loss. on_send's own local
        // error dialog never actually fired: ShellBase::dispatch_room_send_'s
        // outcome always reports success for the non-slash-command path
        // (the actual send happens asynchronously in a background task with
        // the result discarded).
        // on_send_image / on_send_video / on_send_audio / on_send_file
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_). Deliberate behavior change as part of
        // this refactor: the main window's sync client_->send_image()/etc.
        // (blocked the UI thread, showed an inline status message on
        // failure) is replaced by RoomPane's async
        // client_->send_image_async()/etc. (matches what pop-outs always
        // did). Error reporting is NOT lost — ShellBase::handle_upload_complete_ui_
        // already shows a generic "Upload failed: ..." status message on
        // IEventHandler::on_upload_complete for every async send regardless
        // of which window/pane started it, routed through the same
        // on_show_status_message_ui_ -> hStatus_ this block used to update
        // inline.
        // on_delete_requested / on_copy_event_source_requested /
        // on_reaction_toggled already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_) — equivalent bodies, so the
        // duplicate assignments that used to be here are gone. See
        // RoomPane.cpp's delete_event_/copy_event_source_to_clipboard_/
        // toggle_reaction_ for the shared implementation (toggle_reaction_
        // includes the same MSC4027 shortcode lookup this block used to do
        // inline).
        // on_receipt_needed / on_member_pronoun_needed already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_).
        setup_link_clicked_(room_view_);
        room_view_->on_show_copy_menu = [this]()
        {
            if (!room_view_)
                return;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, utf8_to_wstr(room_view_->copy_menu_label()).c_str());
            POINT pt{};
            GetCursorPos(&pt);
            int cmd = static_cast<int>(TrackPopupMenuEx(
                menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                pt.x, pt.y, hwnd_, nullptr));
            DestroyMenu(menu);
            if (cmd == 1)
                room_view_->copy_context_menu_target();
        };
        // Switch the surface cursor to the link/pointer shape while the
        // mouse is over a URL / map tile / file card. Empty URL clears.
        room_view_->on_link_hovered = [this](const std::string& url)
        {
            if (!main_app_surface_) return;
            using HC = tesseract::views::MessageListView::HoverCursor;
            const HC hc = tesseract::views::MessageListView::hover_cursor_for(url);
            main_app_surface_->set_cursor(
                hc == HC::Text      ? tk::win32::Cursor::IBeam
                : hc == HC::Pointer ? tk::win32::Cursor::Pointer
                                    : tk::win32::Cursor::Default);
        };
        main_app_->space_root()->on_link_hovered = [this](const std::string& url)
        {
            if (!main_app_surface_) return;
            main_app_surface_->set_cursor(url.empty() ? tk::win32::Cursor::Default
                                                      : tk::win32::Cursor::Pointer);
        };
        // Cursor over the draggable room-list separator: ↔ on the band, hand
        // on the collapse grip.
        main_app_->on_sidebar_cursor =
            [this](tesseract::views::MainAppWidget::SidebarCursor c)
        {
            if (!main_app_surface_) return;
            using SC = tesseract::views::MainAppWidget::SidebarCursor;
            main_app_surface_->set_cursor(
                c == SC::Resize   ? tk::win32::Cursor::SizeWE
                : c == SC::Toggle ? tk::win32::Cursor::Pointer
                                  : tk::win32::Cursor::Default);
        };
        // on_near_top / on_near_bottom / on_return_to_live /
        // on_scroll_to_original already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_ + RoomPane::request_pagination_back_,
        // which now also calls set_paginating(true)/reset_near_top_latch() to
        // match this window's old request_more_history spinner behavior —
        // see RoomPane.cpp).
        // on_date_jump already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_) — this call was fully redundant with
        // it (and silently overrode it, since it ran after attach()).
        // on_threads_button_clicked / on_thread_open_requested /
        // on_thread_close_requested / on_thread_send / on_thread_send_reply
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_) — equivalent bodies, including the
        // mention/emoticon draft rebuild on send. See RoomPane.cpp's
        // apply_thread_transition_/on_thread_send/on_thread_send_reply for
        // the shared implementation.
        // on_pin_requested / on_unpin_requested already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ ->
        // pin_event_/unpin_event_, equivalent to ShellBase::on_pin_requested/
        // on_unpin_requested's bodies).
        // wire_room_view_picker_ (set_client/emoji+sticker picker image
        // providers/on_sticker_picked) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_'s
        // picker tail) — this call was fully redundant with it.
        // on_fetch_room_members / on_save_topic / on_leave_room /
        // on_room_settings_opened / on_room_settings_avatar_upload_requested
        // / room_settings_view()->on_accept already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ +
        // Deps.on_left_room, constructed above). on_save_topic now reports
        // failures via an async status message (RoomPane::wire_room_view_'s
        // on_save_topic), which this window never had before. on_leave_room's
        // old body cleared current_room_id_/messages unconditionally on any
        // successful leave, even for a room other than the one being viewed
        // — Deps.on_left_room fixes that pre-existing bug by checking
        // current_room_id_ == room_id first, matching Qt6/GTK4.

        // on_ignore_user already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_).

        // ── Sticker right-click on main surface ─────────────────────────────
        main_app_surface_->set_on_right_click(
            [this](tk::Point p)
            {
                if (main_app_ && main_app_->user_info())
                {
                    const auto& r = main_app_->user_info()->bounds();
                    if (p.x >= r.x && p.x < r.right() && p.y >= r.y &&
                        p.y < r.bottom())
                    {
                        if (main_app_->user_info()->on_secondary)
                        {
                            main_app_->user_info()->on_secondary(p);
                        }
                        return;
                    }
                }
                if (!room_view_ || !client_ || room_view_->is_overlay_open())
                {
                    return;
                }
                auto hit = room_view_->message_list()->sticker_hit_at(p);
                if (!hit)
                {
                    return;
                }
                const std::string mxc = hit->source ? hit->source->mxc_url() : std::string{};
                const std::string body = hit->body;
                const std::string info = hit->info_json;
                const bool already_saved =
                    client_->user_pack_has_sticker(mxc, info);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING | (already_saved ? MF_GRAYED : 0),
                            1,
                            utf8_to_wstr(already_saved
                                             ? tk::tr("Already in Saved Stickers")
                                             : tk::tr("Add to Saved Stickers"))
                                .c_str());
                POINT sp{dip_to_phys(p.x), dip_to_phys(p.y)};
                ClientToScreen(main_app_surface_->hwnd(), &sp);
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         sp.x, sp.y, 0, hwnd_, nullptr);
                DestroyMenu(menu);
                if (cmd == 1)
                {
                    auto res = client_->save_sticker_to_user_pack(body, body,
                                                                  mxc, info);
                    if (!res.ok)
                    {
                        show_status_message_(res.message, 6000);
                    }
                }
            });

        // ── Native overlays ──────────────────────────────────────────────────
        // The room-list search field is wired in
        // ShellBase::wire_main_app_widget_() (shared across all four shells).

        room_text_area_ = main_app_->room_view()->compose_bar()->text_area();
        // The composer's popups (@mention, /command, :shortcode:, /gif) and the
        // text area's change/submit/navigation wiring are the shared
        // ComposerPopups.
        create_compose_popups_(main_app_surface_->host());

        // The topic field, the new-pack-name/shortcode/rename fields, and the
        // paste-catcher are all self-owned by each RoomSettingsView instance
        // (room_view_'s and the space-root's, independently) — see
        // RoomSettingsView::name_field()/topic_field() and
        // ImagePackEditorView::new_pack_name_field()/shortcode_field()/
        // pack_name_field()/paste_catcher() — so no shell-side wiring is
        // needed for any of them.

        // on_image_clicked / on_avatar_clicked already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), which
        // uses this window's own Deps.grab_surface_focus (SetFocus(hwnd_))
        // in place of the direct call this window used to make.

        // on_video_clicked (both room_view_'s and room_media_view_'s
        // gallery-reuse alias) already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_, which aliases
        // room_media_view()->on_image_clicked/on_video_clicked to the same
        // handlers it installs on room_view()).
    }

    // Size the main app surface to fill the content area initially.
    if (HWND ms = main_app_surface_->hwnd())
    {
        SetWindowPos(ms, nullptr, 0, 0, 1024, 744,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Emoji picker creation is deferred until first toggle so the cold-
    // start path stays cheap. Recents live in account-data
    // (io.element.recent_emoji), read on demand by the shared picker.

    // Custom flat status strip. Replaces STATUSCLASSNAMEW which carries a 9x
    // size-grip and chunky inset borders.
    register_status_bar_class(hInst_);
    hStatus_ = CreateWindowExW(0, L"TesseractStatusBar", L"",
                               WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr,
                               hInst_, nullptr);
    // Lets status_bar_wnd_proc (a free function, no `this`) reach back into
    // this shell for the persistent-status click hook — see the
    // WM_LBUTTONUP "not a link" branch below.
    SetPropW(hStatus_, L"TesseractStatusOwner", reinterpret_cast<HANDLE>(this));
    // Owner must be a top-level window — using hStatus_ (WS_CHILD) as owner
    // causes the tooltip to be invisible on some Windows 11 builds.
    hStatusTip_ = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd_, nullptr, hInst_, nullptr);
    {
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        // TTF_IDISHWND without TTF_SUBCLASS: we relay mouse messages manually
        // from status_bar_wnd_proc via TTM_RELAYEVENT, which is more reliable
        // on custom window classes than automatic subclassing.
        ti.uFlags = TTF_IDISHWND;
        ti.hwnd = hStatus_;
        ti.uId = reinterpret_cast<UINT_PTR>(hStatus_);
        ti.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(hStatusTip_, TTM_ADDTOOLW, 0,
                     reinterpret_cast<LPARAM>(&ti));
    }
    SendMessageW(hStatusTip_, TTM_SETMAXTIPWIDTH, 0, 500);
    // Store the tooltip HWND as a property so status_bar_wnd_proc can relay.
    if (hStatus_ && hStatusTip_)
        SetPropW(hStatus_, L"StatusTip", hStatusTip_);
    init_pool_callbacks_();
    on_inflight_ui_();
    refresh_sync_status_(); // "Not logged in" until a session restores

    // The login and settings views are constructed lazily by ShellBase
    // (ensure_login_view_()/ensure_settings_view_()).

    branding_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    // create_root_widget (not plain make_unique) so BrandView::host() is
    // valid from construction — Surface::set_root()/RootWidget::add_child()
    // adopt an already-built widget without ever fixing up its host_, so a
    // plain make_unique here leaves host_ permanently null and BrandView's
    // self-scheduling post_delayed() wireframe animation silently never
    // arms itself (confirmed on Qt: it depends entirely on incidental
    // repaints from elsewhere, freezing solid whenever nothing else
    // invalidates it — e.g. throughout the whole account-restore window).
    auto branding_owner =
        tk::create_root_widget<tesseract::views::BrandView>(&branding_surface_->host());
    branding_view_ = branding_owner.get();
    branding_surface_->set_root(std::move(branding_owner));

    apply_current_theme_();

    // Defer login (or the isolated screenshot fixture) to the message loop.
    // on_create() runs synchronously inside
    // CreateWindowExW (i.e. inside create()). A spawned window is created after
    // set_initial_account() and the account hand-off, but posting start_login()
    // still defers it past the rest of spawn_main_window_(), so a secondary
    // window takes the bind path in start_login() instead of re-restoring every
    // account from disk.
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (!screenshot_dir_.empty())
        post_to_ui_([this] { start_screenshot_mode_(); });
    else
#endif
        post_to_ui_([this] { start_login(); });
}

void MainWindow::on_destroy()
{
    taskbar_.unregister_window(hwnd_);
    if (anim_timer_running_ && hwnd_)
    {
        KillTimer(hwnd_, kAnimTimerId);
        anim_timer_running_ = false;
    }
    if (inflight_timer_running_ && hwnd_)
    {
        KillTimer(hwnd_, kInflightTimerId);
        inflight_timer_running_ = false;
    }
    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    // Send this window's deferred "New messages" moves before the clients
    // stop.
    flush_pending_fully_read_now_();
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the survivors;
    // it still drains its own per-window pools below.
    if (!is_pinned_window_)
    {
        // Hang up an active call while the SDK can still send the leave. Once the
        // sync stops, the call would only end with the session, and others would
        // still see us in it.
        end_call();
        for (auto& s : account_manager_.accounts())
        {
            if (s && s->client)
                s->client->stop_sync();
        }
    }
    if (pending_login_client_)
        pending_login_client_->stop_sync();

    // Stop queued work and join all pool threads. decode_image now creates a
    // per-call WIC factory in the worker's own MTA apartment, so there is no
    // longer any COM marshaling back to the STA message queue — a plain join()
    // is sufficient. WorkerPool::~WorkerPool also calls drain(), but by then
    // every thread is already joined (joinable() returns false), so it is a
    // no-op.
    auto com_drain = [](WorkerPool& wp)
    {
        {
            std::lock_guard<std::mutex> lk(wp.mu_);
            wp.stop_      = true;
            wp.on_change_ = nullptr;
            wp.queue_.clear();
            wp.pending_.store(0, std::memory_order_relaxed);
        }
        wp.cv_.notify_all();
        for (auto& t : wp.threads_)
        {
            if (t.joinable())
                t.join();
        }
    };
    com_drain(pool_);
    com_drain(mut_pool_);
    com_drain(media_prefetch_pool_);
}

// run_async_ is implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h)
{
    // The button rects are right-anchored, so a live resize drag only
    // auto-invalidates the newly-exposed sliver by default — force a full
    // repaint of the strip on every size change so the old button position
    // (now stranded mid-strip) doesn't ghost until the drag ends.
    title_bar_.invalidate_strip(hwnd_);

    // A popup's screen position is captured once when it opens and never
    // recomputed — leaving one open across a resize would strand it away
    // from whatever control anchored it. Close everything instead.
    if (main_app_surface_) main_app_surface_->host().dismiss_active_popup();
    if (settings_surface_) settings_surface_->host().dismiss_active_popup();
    if (room_view_) room_view_->dismiss_popups();
    hide_compose_popups_();
    close_account_picker_();

    // In media-viewer full-screen the custom caption strip and the status bar
    // are both suppressed, so the child surfaces fill the whole client area.
    const int STATUS_H = fs_active_ ? 0 : dip_to_phys(24.f);
    const int TITLEBAR_H = fs_active_ ? 0 : title_bar_.height_px();

    if (hStatus_)
    {
        ShowWindow(hStatus_, fs_active_ ? SW_HIDE : SW_SHOW);
        SetWindowPos(hStatus_, nullptr, 0, h - STATUS_H, w, STATUS_H,
                     SWP_NOZORDER);
        SendMessageW(hStatus_, WM_SIZE, 0, 0);
    }

    // main_app_surface_ (and every other tk::win32::Surface) now flushes
    // its own repaint synchronously on resize (see Host::on_resize()'s
    // UpdateWindow() call) so its DirectComposition-presented content is
    // never a stale frame behind. The title bar/background (painted
    // directly on hwnd_) and the status bar (a separate HWND) are still on
    // the classic invalidate-and-wait-for-the-next-WM_PAINT path, which
    // left them lagging a frame or two behind the now-faster surface —
    // visible as a flicker/seam right as a resize drag ends. Flush both
    // synchronously too, right before every return below, so all three
    // regions land in the same frame.
    auto flush_chrome = [&]
    {
        UpdateWindow(hwnd_);
        if (hStatus_)
        {
            UpdateWindow(hStatus_);
        }
    };

    // SetWindowPos() below synchronously dispatches WM_SIZE to the child
    // surface whenever its size actually changes, and that WM_SIZE already
    // triggers a full relayout + synchronous repaint (Host::on_resize()).
    // So on every tick of a live resize drag, an unconditional follow-up
    // relayout()/layout() call here would redo that same measure+arrange+
    // paint pass a second time. Only call it ourselves when the target size
    // matches what the surface already has — meaning SetWindowPos won't
    // fire WM_SIZE at all (e.g. a view switch where the overall client
    // size didn't change), so nobody else will relayout it.
    auto reposition_needs_relayout = [](HWND hwnd, int target_w,
                                        int target_h)
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        return rc.right == target_w && rc.bottom == target_h;
    };

    int content_h = h - STATUS_H - TITLEBAR_H;

    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        const bool needs_relayout = reposition_needs_relayout(
            branding_surface_->hwnd(), w, content_h);
        SetWindowPos(branding_surface_->hwnd(), nullptr, 0, TITLEBAR_H, w,
                     content_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (needs_relayout) branding_surface_->relayout();
        flush_chrome();
        return;
    }

    if (login_visible_ && login_view_ && login_native_()->hwnd())
    {
        const bool needs_relayout =
            reposition_needs_relayout(login_native_()->hwnd(), w, content_h);
        SetWindowPos(login_native_()->hwnd(), nullptr, 0, TITLEBAR_H, w,
                     content_h, SWP_NOZORDER);
        if (needs_relayout) login_surface_->relayout();
        flush_chrome();
        return;
    }

    if (app_settings_open_ && settings_surface_ && settings_native_()->hwnd())
    {
        const bool needs_relayout = reposition_needs_relayout(
            settings_native_()->hwnd(), w, content_h);
        SetWindowPos(settings_native_()->hwnd(), nullptr, 0, TITLEBAR_H, w,
                     content_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (needs_relayout) settings_surface_->relayout();
        flush_chrome();
        return;
    }

    // The MainAppWidget tree owns all sidebar/chat/banner layout internally.
    // Just resize the single surface to fill the content area and relayout.
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        const bool needs_relayout = reposition_needs_relayout(
            main_app_surface_->hwnd(), w, content_h);
        SetWindowPos(main_app_surface_->hwnd(), nullptr, 0, TITLEBAR_H, w,
                     content_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (needs_relayout) main_app_surface_->relayout();
    }
    flush_chrome();
}

// ---------------------------------------------------------------------------
// Login / reconnect
// ---------------------------------------------------------------------------

void MainWindow::start_login()
{
    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (is_secondary_window_startup_())
    {
        finish_login_ui_(active_account_->user_id);
        return;
    }

    // Pre-flight OS-level connectivity check — see tk::Host::
    // is_network_available()'s doc comment. Computed here, on the UI
    // thread, and threaded through so the worker-thread restore loop below
    // never touches Host.
    const bool network_available = branding_surface_->host().is_network_available();

    // Migrate + restore every stored account (shared loop in ShellBase), off
    // the UI thread so the window stays responsive. The native per-account
    // notifier construction runs through install_account_notifier_ below;
    // Win32 has no UnifiedPush connector.
    restore_all_accounts_async_(
        [this](RestoreResult restore)
        {
            if (!restore.any_accounts)
            {
                // Nothing to silently restore, so an autostart launch can't
                // stay hidden — the user needs to log in.
                if (start_hidden_)
                {
                    start_hidden_ = false;
                    ShowWindow(hwnd_, SW_SHOW);
                }
                show_login_after_restore_(restore, [this] { start_login(); });
                return;
            }

            finish_login_ui_(restore.active_uid);
        },
        network_available);
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
class MainWindow::Win32ScreenshotHost final
    : public tesseract::screenshot::ScreenshotHost
{
public:
    explicit Win32ScreenshotHost(MainWindow& window) : window_(window) {}

    void apply_theme(tesseract::screenshot::ScreenshotTheme theme) override
    {
        window_.apply_theme_ui_(
            theme == tesseract::screenshot::ScreenshotTheme::Dark
                ? tk::Theme::dark()
                : tk::Theme::light());
    }
    void refresh() override
    {
        // Relayout the surfaces directly: on_size() would also dismiss open
        // popups (the emoji-picker scene's picker is one).
        if (window_.main_app_surface_)
            window_.main_app_surface_->relayout();
        if (window_.settings_surface_)
            window_.settings_surface_->relayout();
        RedrawWindow(window_.hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);
    }
    bool save_png(const std::string& filename) override
    {
        if (window_.save_screenshot_(utf8_to_wstr(filename).c_str()))
            return true;
        OutputDebugStringW(L"Tesseract screenshot: failed to save image\n");
        return false;
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        window_.screenshot_step_ = std::move(fn);
        SetTimer(window_.hwnd_, kScreenshotStepTimerId,
                 static_cast<UINT>(ms), nullptr);
    }
    void finish(bool ok) override
    {
        // The message loop's exit code isn't reachable from here; a failed
        // capture must still fail the CI step.
        if (!ok)
            std::exit(EXIT_FAILURE);
        DestroyWindow(window_.hwnd_);
    }

private:
    MainWindow& window_;
};

void MainWindow::start_screenshot_mode_()
{
    if (!seed_screenshot_fixture_(main_app_surface_->factory()))
    {
        OutputDebugStringW(L"Tesseract screenshot: could not load assets\n");
        std::exit(EXIT_FAILURE);
    }
    refresh_user_strip_();
    show_main_content_();

    // A fixed physical window size makes output stable on the 96-DPI hosted
    // runner while remaining large enough to exercise the desktop layout.
    SetWindowPos(hwnd_, nullptr, 0, 0, 1100, 768,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    std::error_code ec;
    std::filesystem::create_directories(screenshot_dir_, ec);
    if (ec)
    {
        OutputDebugStringW(L"Tesseract screenshot: could not create output directory\n");
        std::exit(EXIT_FAILURE);
    }

    screenshot_host_ = std::make_unique<Win32ScreenshotHost>(*this);
    start_screenshot_director_(*screenshot_host_, "win", 500);
}

bool MainWindow::save_screenshot_(const wchar_t* filename)
{
    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect))
        return false;
    const int width  = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return false;

    HDC window_dc = GetWindowDC(hwnd_);
    HDC memory_dc = window_dc ? CreateCompatibleDC(window_dc) : nullptr;
    HBITMAP bitmap = memory_dc
        ? CreateCompatibleBitmap(window_dc, width, height)
        : nullptr;
    if (!window_dc || !memory_dc || !bitmap)
    {
        if (bitmap) DeleteObject(bitmap);
        if (memory_dc) DeleteDC(memory_dc);
        if (window_dc) ReleaseDC(hwnd_, window_dc);
        return false;
    }

    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    BOOL painted = PrintWindow(hwnd_, memory_dc, PW_RENDERFULLCONTENT);
    if (!painted)
        painted = BitBlt(memory_dc, 0, 0, width, height, window_dc, 0, 0,
                         SRCCOPY | CAPTUREBLT);
    SelectObject(memory_dc, previous);

    bool saved = false;
    if (painted)
    {
        UINT count = 0;
        UINT bytes = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &bytes) == Gdiplus::Ok &&
            count > 0 && bytes > 0)
        {
            std::vector<BYTE> storage(bytes);
            auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(
                storage.data());
            if (Gdiplus::GetImageEncoders(count, bytes, codecs) == Gdiplus::Ok)
            {
                for (UINT i = 0; i < count; ++i)
                {
                    if (wcscmp(codecs[i].MimeType, L"image/png") != 0)
                        continue;
                    Gdiplus::Bitmap image(bitmap, nullptr);
                    const auto path = screenshot_dir_ / filename;
                    saved = image.Save(path.c_str(), &codecs[i].Clsid,
                                       nullptr) == Gdiplus::Ok;
                    break;
                }
            }
        }
    }

    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(hwnd_, window_dc);
    return saved;
}
#endif

std::unique_ptr<tesseract::IEventHandler>
MainWindow::make_account_bridge_(const std::string& uid)
{
    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(uid);
    return bridge;
}

void MainWindow::install_account_notifier_(tesseract::AccountSession& session)
{
    // Per-account notifier: the Win32Notifier routes balloon clicks back
    // through the message pump, switching account + navigating to the room.
    session.notifier =
        std::make_unique<win32::Win32Notifier>(hwnd_, session.user_id);
}

std::unique_ptr<tk::AudioPlayback> MainWindow::make_call_audio_output_()
{
    return tk::make_audio_playback_win32();
}

tesseract::CallWindowBase* MainWindow::create_call_window_()
{
    return new win32::CallWindow(this);
}

tesseract::AuxWindowBase* MainWindow::create_aux_window_(std::string title,
                                                         std::unique_ptr<tk::Widget> root,
                                                         int width, int height)
{
    return new win32::AuxWindow(this, title, std::move(root), width, height, current_theme_);
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_login_surface_ui_()
{
    auto surface = std::make_unique<tk::win32::Surface>(hInst_, hwnd_,
                                                        tk::Theme::light());
    if (surface->hwnd())
        ShowWindow(surface->hwnd(), SW_HIDE);
    return surface;
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_settings_surface_ui_()
{
    auto surface = std::make_unique<tk::win32::Surface>(hInst_, hwnd_,
                                                        tk::Theme::light());
    if (surface->hwnd())
        ShowWindow(surface->hwnd(), SW_HIDE);
    return surface;
}

void MainWindow::on_pre_paint_()
{
    // WM_TIMER is only delivered when no input or WM_PAINT is pending, so
    // continuous repaints (pointer motion, scrolling) can starve the animation
    // clock. If a tick is overdue, queue one: posted messages are dispatched
    // before WM_PAINT and WM_TIMER. Never tick inside the paint itself, as it
    // invalidates and would sustain a paint loop.
    if (anim_timer_running_ && !anim_catchup_posted_ &&
        monotonic_ms_() - last_anim_tick_ms_ >= static_cast<std::int64_t>(kAnimTimerMs))
    {
        anim_catchup_posted_ = true;
        post_to_ui_(
            [this]
            {
                anim_catchup_posted_ = false;
                if (anim_timer_running_)
                    on_anim_tick();
            });
    }
}

void MainWindow::grab_main_surface_focus_()
{
    if (hwnd_)
        SetFocus(hwnd_);
}

tk::SurfaceBase* MainWindow::main_surface_()
{
    return main_app_surface_.get();
}

void MainWindow::open_settings_()
{
    if (!prepare_settings_view_())
        return;
    if (settings_from_login_())
    {
        // Settings replaces the login view for now; close_settings_()
        // brings it back.
        login_visible_ = false;
        if (login_view_ && login_native_()->hwnd())
            ShowWindow(login_native_()->hwnd(), SW_HIDE);
    }

    set_app_settings_open_(true);
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_HIDE);
    }
    if (settings_surface_ && settings_native_()->hwnd())
    {
        ShowWindow(settings_native_()->hwnd(), SW_SHOW);
    }
    start_search_index_stats_poll_();

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::rebind_login_client_ui_()
{
    if (!login_view_)
        return;
    login_view_->set_client(reset_pending_login_client_());
    // Swap only. Settings is open on top of the login view, and reset() would
    // refocus its homeserver field mid-typing; the homeserver check is re-run
    // by leaving Settings (take_login_recheck_()).
}

void MainWindow::close_settings_()
{
    // Opened from the login screen: go back to it, not the main content.
    const bool to_login = take_settings_from_login_() && login_view_;
    set_app_settings_open_(false);
    stop_search_index_stats_poll_();
    if (settings_surface_ && settings_native_()->hwnd())
    {
        ShowWindow(settings_native_()->hwnd(), SW_HIDE);
    }
    if (!to_login && main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_SHOW);
    }
    teardown_settings_view_();

    if (to_login)
    {
        show_login_page_ui_();
        // A proxy change made in Settings replaced the client: re-run the
        // homeserver check against it.
        if (take_login_recheck_() && login_view_)
            login_view_->reset();
        return;
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::show_login_page_ui_()
{
    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        branding_visible_ = false;
        ShowWindow(branding_surface_->hwnd(), SW_HIDE);
    }
    login_visible_ = true;
    if (login_view_ && login_native_()->hwnd())
    {
        ShowWindow(login_native_()->hwnd(), SW_SHOW);
    }
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_HIDE);
    }
    if (settings_surface_ && settings_native_()->hwnd())
    {
        ShowWindow(settings_native_()->hwnd(), SW_HIDE);
    }
    set_app_settings_open_(false);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::show_main_content_()
{
    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        branding_visible_ = false;
        ShowWindow(branding_surface_->hwnd(), SW_HIDE);
    }
    login_visible_ = false;
    set_app_settings_open_(false);
    if (login_view_ && login_native_()->hwnd())
    {
        ShowWindow(login_native_()->hwnd(), SW_HIDE);
    }
    teardown_login_view_();
    if (settings_surface_ && settings_native_()->hwnd())
    {
        ShowWindow(settings_native_()->hwnd(), SW_HIDE);
    }
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_SHOW);
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);

    mark_main_content_ready_();
}

void MainWindow::bring_main_window_forward_for_overlay_()
{
    if (IsIconic(hwnd_))
    {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
}

void MainWindow::restore_focus_after_overlay_()
{
    // Prefer the compose bar when a room is open, otherwise fall back to the
    // main window so Esc / nav keys reach wnd_proc.
    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
    else
    {
        SetFocus(hwnd_);
    }
}

void MainWindow::raise_main_window_ui_()
{
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    else ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

bool MainWindow::is_main_window_focused_() const
{
    return hwnd_ && GetForegroundWindow() == hwnd_;
}

void MainWindow::request_notification_attention_()
{
    // Only a window that is on screen has a taskbar button worth flashing; a
    // minimised or hidden one gets the notification itself.
    if (!is_main_window_visible_())
    {
        return;
    }
    FLASHWINFO fwi{};
    fwi.cbSize = sizeof(fwi);
    fwi.hwnd = hwnd_;
    fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    fwi.uCount = 3;
    FlashWindowEx(&fwi);
}

bool MainWindow::accel_from_chord_(const tk::KeyChord& chord, ACCEL& out)
{
    BYTE virt = FVIRTKEY;
    if (chord.mods & (tk::ModPrimary | tk::ModCtrl))
        virt |= FCONTROL;
    if (chord.mods & tk::ModShift)
        virt |= FSHIFT;
    if (chord.mods & tk::ModAlt)
        virt |= FALT;
    if (chord.mods & tk::ModMeta)
        return false; // the Windows key isn't an accelerator modifier
    WORD key = 0;
    switch (chord.key)
    {
    case tk::Key::Character:
    {
        if (chord.text.size() != 1)
            return false;
        const char c = chord.text.front();
        if (c >= 'a' && c <= 'z')
        {
            key = static_cast<WORD>(c - 'a' + 'A');
            break;
        }
        // Punctuation: ask the active layout which key types it (`/` is
        // VK_OEM_2 on US, Shift+7 on German).
        const SHORT scan = VkKeyScanW(static_cast<WCHAR>(c));
        if (scan == -1)
            return false;
        const BYTE layout_shift = HIBYTE(scan);
        if (layout_shift & (2 | 4))
            return false; // needs Ctrl/Alt (AltGr) — no clean accelerator
        if (layout_shift & 1)
        {
            if (!((chord.mods | chord.optional) & tk::ModShift))
                return false;
            virt |= FSHIFT;
        }
        key = LOBYTE(scan);
        break;
    }
    case tk::Key::Tab: key = VK_TAB; break;
    case tk::Key::Left: key = VK_LEFT; break;
    case tk::Key::Right: key = VK_RIGHT; break;
    case tk::Key::F1: key = VK_F1; break;
    default: return false;
    }
    out.fVirt = virt;
    out.key = key;
    return true;
}

void MainWindow::destroy_accelerators_()
{
    for (HACCEL* table : {&accel_, &accel_app_})
    {
        if (*table)
        {
            DestroyAcceleratorTable(*table);
            *table = nullptr;
        }
    }
    accel_events_.clear();
}

// Accelerator tables built from the shared shortcut registry (Ctrl+K,
// Ctrl+F, Alt+Left, Ctrl+/, F1, ...): accelerators fire even when a native
// edit control (compose / search) holds keyboard focus — those controls eat
// WM_KEYDOWN before it reaches this window's wnd_proc, so a plain key
// handler only fires when the canvas has focus. TranslateAccelerator
// re-fires WM_COMMAND for every OS key-repeat while held, which is what lets
// repeated Tab presses advance the Ctrl+Tab cycle; committing on
// Ctrl-release is handled by the tk host's keyboard hook — accelerators only
// fire on key-down.
//
// accel_ holds every global shortcut, for keys pressed in this window;
// accel_app_ only the Application-scoped ones, for keys pressed in a pop-out
// room/call window (Window-scoped shortcuts act on this window's room). Both
// map to the same WM_COMMAND ids. Punctuation keys depend on the keyboard
// layout, so the tables are rebuilt whenever it changes.
void MainWindow::rebuild_accelerators_()
{
    destroy_accelerators_();
    accel_layout_ = GetKeyboardLayout(0);
    std::vector<ACCEL> all;
    std::vector<ACCEL> app;
    for (const auto& def : tesseract::views::shortcuts())
    {
        if (def.scope == tesseract::views::ShortcutScope::Contextual)
            continue;
        for (const auto& chord : def.chords)
        {
            ACCEL a{};
            if (!accel_from_chord_(chord, a) || accel_events_.size() >= kMaxShortcuts)
                continue;
            a.cmd = static_cast<WORD>(IDC_SHORTCUT_BASE + accel_events_.size());
            all.push_back(a);
            if (def.scope == tesseract::views::ShortcutScope::Application)
                app.push_back(a);
            accel_events_.push_back(tk::to_key_event(chord));
        }
    }
    accel_ = CreateAcceleratorTableW(all.data(), static_cast<int>(all.size()));
    if (!app.empty())
        accel_app_ = CreateAcceleratorTableW(app.data(), static_cast<int>(app.size()));
}

bool MainWindow::pre_translate_message(MSG* msg)
{
    if (!hwnd_ || msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST)
        return false;
    if (GetKeyboardLayout(0) != accel_layout_)
        rebuild_accelerators_();
    const bool in_main_window = msg->hwnd && GetAncestor(msg->hwnd, GA_ROOT) == hwnd_;
    HACCEL table = in_main_window ? accel_ : accel_app_;
    return table && TranslateAcceleratorW(hwnd_, table, msg);
}

void MainWindow::focus_forward_picker_field_()
{
    if (!main_app_ || !main_app_->forward_picker())
        return;
    if (auto* f = main_app_->forward_picker()->search_field())
    {
        f->set_text("");
        f->set_focused(true);
    }
}

void MainWindow::hide_forward_picker_field_()
{
    if (main_app_ && main_app_->forward_picker())
        if (auto* f = main_app_->forward_picker()->search_field())
            f->set_visible(false);
    SetFocus(hwnd_);
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (main_app_)
    {
        main_app_->room_list_view()->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
}

// ---------------------------------------------------------------------------
// Window title
// ---------------------------------------------------------------------------

void MainWindow::apply_window_title_ui_(const std::string& title)
{
    if (!hwnd_)
        return;
    SetWindowTextW(hwnd_, utf8_to_wstr(title).c_str());
    title_bar_.invalidate_strip(hwnd_);
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

void MainWindow::after_rooms_updated_ui_()
{
    taskbar_.set_next_unread_available(hwnd_, best_unread_room_() != nullptr);
}

void MainWindow::on_recent_room_visited_(const tesseract::RoomInfo& room)
{
    const std::string title = !room.name.empty()
        ? room.name
        : (!room.canonical_alias.empty() ? room.canonical_alias : room.id);
    taskbar_.record_recent_room(utf8_to_wstr(room.id), utf8_to_wstr(title));

    const std::string avatar = room.effective_avatar_url();
    if (avatar.empty()) return;
    recent_taskbar_avatar_rooms_[avatar] = room.id;
    ensure_room_avatar_(room);
    const auto disk_key = tk::CacheKey::thumbnail(
        avatar, tesseract::visual::kAvatarCacheSize, tesseract::visual::kAvatarCacheSize);
    run_async_([this, room_id = room.id, disk_key]
    {
        auto bytes = account_manager_.media_disk_cache().load(disk_key);
        if (bytes.empty()) return;
        post_to_ui_([this, room_id, bytes = std::move(bytes)]
        {
            taskbar_.record_recent_room_avatar(utf8_to_wstr(room_id), bytes);
        });
    });
}

void MainWindow::on_space_children_cache_ready_ui_()
{
    refresh_room_list_();
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refresh_room_list_();
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
    taskbar_.set_unread(has_unread, has_highlight);
    taskbar_.set_next_unread_available(hwnd_, best_unread_room_() != nullptr);
}

void MainWindow::on_upload_progress_ui_(std::uint64_t request_id,
                                         std::uint64_t current,
                                         std::uint64_t total)
{
    taskbar_.upload_progress(request_id, current, total);
}

void MainWindow::on_upload_finished_ui_(std::uint64_t request_id, bool ok)
{
    taskbar_.upload_finished(request_id, ok);
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

// ensure_room_avatar_, ensure_user_avatar_, and ensure_media_image_ are
// implemented in tesseract::ShellBase. ensure_reply_details_ now lives on
// RoomPane (main_room_pane_).

// ---------------------------------------------------------------------------
// Animated media — multi-frame WIC decode + 60 Hz WM_TIMER tick
// ---------------------------------------------------------------------------

void MainWindow::on_anim_tick()
{
    last_anim_tick_ms_ = monotonic_ms_();
    tick_anim_();
}

void MainWindow::stop_anim_tick_()
{
    if (anim_timer_running_ && hwnd_)
    {
        KillTimer(hwnd_, kAnimTimerId);
        anim_timer_running_ = false;
    }
}

void MainWindow::update_main_anim_regions_()
{
    if (main_app_surface_)
    {
        main_app_surface_->update_anim_regions();
    }
}

// ---------------------------------------------------------------------------
// ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::request_relayout_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::request_repaint_()
{
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        InvalidateRect(main_app_surface_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::post_to_ui_(std::function<void()> fn)
{
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(hwnd_, WM_TESSERACT_POST_TO_UI, 0,
                      reinterpret_cast<LPARAM>(p)))
    {
        delete p; // window already gone; drop the closure
    }
}

void MainWindow::post_to_ui_after_(int ms, std::function<void()> fn)
{
    // Delegate to Host::post_delayed: a detached thread sleeps `ms` ms then
    // marshals the closure back via PostMessageW to the surface HWND.
    // SetTimer on hwnd_ was tried but WM_TIMER never arrived reliably at
    // MainWindow::wnd_proc (same failure as the original kSearchDebounceTimer=3
    // approach fixed in e09105d).
    if (main_app_surface_)
    {
        main_app_surface_->host().post_delayed(ms, std::move(fn));
        return;
    }
    // Nowhere to post: the post is dropped, so no debounce slot may stay armed
    // waiting for it.
    for (auto& [key, st] : debounce_state_)
        st.armed = false;
}

bool MainWindow::accept_media_bytes_(const tk::CacheKey& cache_key,
                                     MediaKind kind,
                                     const std::vector<std::uint8_t>& bytes)
{
    if (kind == MediaKind::RoomAvatar)
    {
        if (const auto it = recent_taskbar_avatar_rooms_.find(cache_key.id);
            it != recent_taskbar_avatar_rooms_.end())
        {
            taskbar_.record_recent_room_avatar(utf8_to_wstr(it->second), bytes);
        }
    }
    return main_app_surface_ != nullptr;
}

void MainWindow::ingest_tile_media_(const tk::CacheKey& cache_key,
                                    std::vector<std::uint8_t> bytes)
{
    if (auto img = main_app_surface_->factory().decode_image(bytes))
    {
        account_manager_.image_cache().store(cache_key, std::move(img));
        // A map tile fills a fixed-size map card and isn't a tracked row media
        // source, so it needs only a repaint (LocationMapPanner::paint re-reads
        // the tile from the cache).
    }
    notify_secondary_media_ready_(cache_key.id, MediaKind::Tile);
    if (HWND h = main_app_surface_->hwnd())
    {
        InvalidateRect(h, nullptr, FALSE);
    }
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    wchar_t buf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    const std::wstring filter =
        file_filter({{tk::tr("Images"), L"*.png;*.jpg;*.jpeg;*.gif;*.webp"}});
    ofn.lpstrFilter = filter.c_str();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    HANDLE hf = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER sz{};
    GetFileSizeEx(hf, &sz);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz.QuadPart));
    DWORD read_bytes = 0;
    ReadFile(hf, bytes.data(), static_cast<DWORD>(bytes.size()),
             &read_bytes, nullptr);
    CloseHandle(hf);

    std::wstring wp(buf);
    std::string mime = "image/jpeg";
    if (wp.ends_with(L".png"))       mime = "image/png";
    else if (wp.ends_with(L".gif"))  mime = "image/gif";
    else if (wp.ends_with(L".webp")) mime = "image/webp";

    post_to_ui_([cb = std::move(cb), bytes = std::move(bytes), mime]() mutable
    {
        cb(std::move(bytes), mime);
    });
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                          int max_h)
{
    if (tesseract::is_jxl_data(bytes))
    {
        auto& jxl_backend = tk::win32::backend_singleton();
        return tesseract::jxl::decode_image(bytes, max_w, max_h,
                                  [&jxl_backend](const std::uint8_t* px, int w, int h)
                                  { return tk::d2d::make_image_from_rgba(jxl_backend, px, w, h); });
    }
    // Untrusted bytes: refuse over-budget images before any codec allocates (tk/image_sniff.h).
    if (!tk::decode_size_allowed(bytes, tk::bytes_may_be_animated(bytes)))
        return DecodedImage{};
    DecodedImage d;
    if (bytes.empty())
    {
        return d;
    }
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    auto frames = tk::d2d::decode_animation(backend, span, max_w, max_h);
    if (frames.size() >= 2)
    {
        d.frames.reserve(frames.size());
        d.delays_ms.reserve(frames.size());
        for (auto& af : frames)
        {
            d.frames.push_back(std::move(af.image));
            d.delays_ms.push_back(af.delay_ms);
        }
        return d;
    }
    d.still = tk::d2d::decode_image(backend, span, max_w, max_h);
    return d;
}

bool MainWindow::decode_image_streamed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
    {
        auto& jxl_backend = tk::win32::backend_singleton();
        return tesseract::jxl::decode_streamed(bytes, max_w, max_h,
                                  [&jxl_backend](const std::uint8_t* px, int w, int h)
                                  { return tk::d2d::make_image_from_rgba(jxl_backend, px, w, h); },
                                  on_first_frame, on_frame);
    }
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    if (bytes.empty())
    {
        return false;
    }
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    bool got_first = false;
    std::function<void(std::unique_ptr<tk::Image>, int)> first_cb =
        [&](std::unique_ptr<tk::Image> img, int delay_ms)
    {
        got_first = true;
        on_first_frame(std::move(img), delay_ms);
    };
    tk::d2d::decode_animation(backend, span, max_w, max_h, &first_cb,
                              &on_frame);
    return got_first;
}

bool MainWindow::decode_image_streamed_windowed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
    {
        auto& jxl_backend = tk::win32::backend_singleton();
        return tesseract::jxl::decode_streamed_windowed(bytes, max_w, max_h,
                                  [&jxl_backend](const std::uint8_t* px, int w, int h)
                                  { return tk::d2d::make_image_from_rgba(jxl_backend, px, w, h); },
                                  on_first_frame, on_frame);
    }
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    if (bytes.empty())
    {
        return false;
    }
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    // Animations at or under tk::kAnimDecodeWindowThresholdFrames decode
    // whole (unwindowed) — matches AnimImageCache's own tuning for "not
    // worth the complexity". See anim_image_cache.h's
    // kLookaheadFrames/kTopupBatchFrames/kKeepBehindFrames for the
    // resident-window size once windowing kicks in above this.
    return tk::d2d::decode_animation_windowed(
        backend, span, max_w, max_h, tk::kAnimDecodeWindowThresholdFrames,
        on_first_frame, on_frame);
}

std::int64_t MainWindow::monotonic_ms_()
{
    return static_cast<std::int64_t>(GetTickCount64());
}

void MainWindow::start_anim_tick_()
{
    if (!anim_timer_running_ && hwnd_)
    {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerMs, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::start_inflight_tick_()
{
    if (!inflight_timer_running_ && hwnd_)
    {
        SetTimer(hwnd_, kInflightTimerId, kAnimTimerMs, nullptr);
        inflight_timer_running_ = true;
    }
}

void MainWindow::stop_inflight_tick_()
{
    if (inflight_timer_running_ && hwnd_)
    {
        KillTimer(hwnd_, kInflightTimerId);
        inflight_timer_running_ = false;
    }
}

void MainWindow::repaint_inflight_spinner_()
{
    if (hStatus_)
    {
        const uint32_t phase_enc =
            static_cast<uint32_t>(inflight_spin_phase_() * 65535.0f) + 1u;
        SetPropW(hStatus_, L"TesseractStatusPhase",
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(phase_enc)));
        InvalidateRect(hStatus_, nullptr, FALSE);
    }
}

void MainWindow::probe_drop_video_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
        // COM must be initialized on each background thread.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IStream* com_stream = SHCreateMemStream(
            reinterpret_cast<const BYTE*>(bytes.data()),
            static_cast<UINT>(bytes.size()));
        if (com_stream)
        {
            IMFByteStream* mf_stream = nullptr;
            if (SUCCEEDED(MFCreateMFByteStreamOnStream(com_stream, &mf_stream)))
            {
                IMFSourceReader* reader = nullptr;
                if (SUCCEEDED(MFCreateSourceReaderFromByteStream(
                        mf_stream, nullptr, &reader)))
                {
                    // Duration (in 100-nanosecond units)
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(reader->GetPresentationAttribute(
                            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                            MF_PD_DURATION, &pv)) &&
                        pv.vt == VT_UI8)
                    {
                        info.duration_ms = pv.uhVal.QuadPart / 10000ULL;
                    }
                    PropVariantClear(&pv);

                    // Request RGB32 output for the first video frame
                    IMFMediaType* out_type = nullptr;
                    if (SUCCEEDED(MFCreateMediaType(&out_type)))
                    {
                        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                        reader->SetCurrentMediaType(
                            static_cast<DWORD>(
                                MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                            nullptr, out_type);
                        out_type->Release();
                    }

                    DWORD stream_index = 0, flags = 0;
                    LONGLONG timestamp = 0;
                    IMFSample* sample = nullptr;
                    reader->ReadSample(
                        static_cast<DWORD>(
                            MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                        0, &stream_index, &flags, &timestamp, &sample);

                    if (sample)
                    {
                        IMFMediaBuffer* buf = nullptr;
                        sample->ConvertToContiguousBuffer(&buf);
                        if (buf)
                        {
                            IMFMediaType* cur_type = nullptr;
                            UINT32 vw = 0, vh = 0;
                            if (SUCCEEDED(reader->GetCurrentMediaType(
                                    static_cast<DWORD>(
                                        MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                    &cur_type)))
                            {
                                MFGetAttributeSize(cur_type,
                                                   MF_MT_FRAME_SIZE, &vw, &vh);
                            }
                            info.video_w = vw;
                            info.video_h = vh;

                            BYTE* data = nullptr;
                            DWORD len = 0;
                            buf->Lock(&data, nullptr, &len);

                            if (data && len && vw > 0 && vh > 0)
                            {
                                // MF may produce bottom-up (negative stride) RGB32;
                                // flip rows to top-down before handing to WIC.
                                LONG stride_val =
                                    static_cast<LONG>(vw * 4);
                                if (cur_type)
                                    cur_type->GetUINT32(
                                        MF_MT_DEFAULT_STRIDE,
                                        reinterpret_cast<UINT32*>(&stride_val));
                                UINT abs_stride = stride_val < 0
                                    ? static_cast<UINT>(-stride_val)
                                    : static_cast<UINT>(stride_val);
                                std::vector<BYTE> flipped;
                                BYTE* pixel_data = data;
                                if (stride_val < 0)
                                {
                                    flipped.resize(vh * abs_stride);
                                    for (UINT row = 0; row < vh; ++row)
                                        memcpy(flipped.data() + row * abs_stride,
                                               data + (vh - 1 - row) * abs_stride,
                                               abs_stride);
                                    pixel_data = flipped.data();
                                }

                                IWICImagingFactory* wic = nullptr;
                                if (SUCCEEDED(CoCreateInstance(
                                        CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&wic))))
                                {
                                    IStream* out = nullptr;
                                    CreateStreamOnHGlobal(nullptr, TRUE, &out);
                                    IWICBitmapEncoder* enc = nullptr;
                                    if (SUCCEEDED(wic->CreateEncoder(
                                            GUID_ContainerFormatJpeg, nullptr,
                                            &enc)) && out)
                                    {
                                        enc->Initialize(out,
                                                        WICBitmapEncoderNoCache);
                                        IWICBitmapFrameEncode* frame = nullptr;
                                        if (SUCCEEDED(
                                                enc->CreateNewFrame(&frame, nullptr)))
                                        {
                                            frame->Initialize(nullptr);
                                            frame->SetSize(vw, vh);
                                            WICPixelFormatGUID fmt =
                                                GUID_WICPixelFormat32bppBGR;
                                            frame->SetPixelFormat(&fmt);
                                            frame->WritePixels(
                                                vh, abs_stride,
                                                vh * abs_stride, pixel_data);
                                            frame->Commit();
                                            frame->Release();
                                        }
                                        enc->Commit();
                                        enc->Release();
                                        // Read JPEG bytes from stream
                                        LARGE_INTEGER seek{};
                                        out->Seek(seek, STREAM_SEEK_SET, nullptr);
                                        STATSTG stat{};
                                        out->Stat(&stat, STATFLAG_NONAME);
                                        std::vector<std::uint8_t> jpeg(
                                            static_cast<std::size_t>(
                                                stat.cbSize.QuadPart));
                                        ULONG nread = 0;
                                        out->Read(jpeg.data(),
                                                  static_cast<ULONG>(jpeg.size()),
                                                  &nread);
                                        info.thumb_bytes = std::move(jpeg);
                                        info.thumb_w = vw;
                                        info.thumb_h = vh;
                                    }
                                    if (out) out->Release();
                                    wic->Release();
                                }
                            }
                            if (cur_type) cur_type->Release();
                            buf->Unlock();
                            buf->Release();
                        }
                        sample->Release();
                    }
                    reader->Release();
                }
                mf_stream->Release();
            }
            com_stream->Release();
        }

        CoUninitialize();

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::probe_drop_audio_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
        // COM must be initialized on each background thread.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IStream* com_stream = SHCreateMemStream(
            reinterpret_cast<const BYTE*>(bytes.data()),
            static_cast<UINT>(bytes.size()));
        if (com_stream)
        {
            IMFByteStream* mf_stream = nullptr;
            if (SUCCEEDED(MFCreateMFByteStreamOnStream(com_stream, &mf_stream)))
            {
                IMFSourceReader* reader = nullptr;
                if (SUCCEEDED(MFCreateSourceReaderFromByteStream(
                        mf_stream, nullptr, &reader)))
                {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(reader->GetPresentationAttribute(
                            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                            MF_PD_DURATION, &pv)) &&
                        pv.vt == VT_UI8)
                    {
                        info.duration_ms = pv.uhVal.QuadPart / 10000ULL;
                    }
                    PropVariantClear(&pv);
                    reader->Release();
                }
                mf_stream->Release();
            }
            com_stream->Release();
        }

        CoUninitialize();

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::decode_video_first_frame_(
    std::vector<std::uint8_t> bytes,
    std::function<void(std::vector<std::uint8_t>)> done)
{
    // Runs tk::decode_video_frames + a WIC JPEG-encode against `bytes`
    // off-thread and invokes `done(jpeg)` from that worker (empty jpeg on any
    // failure, including a truncated prefix with no decodable frame).
    run_async_(
        [done = std::move(done), bytes = std::move(bytes)]() mutable
        {
            tk::DecodedVideoFrames dvf = tk::decode_video_frames(
                bytes.data(), bytes.size(),
                tesseract::visual::kMaxInlineImageWidth,
                tesseract::visual::kMaxInlineImageHeight,
                /*max_frames=*/1);
            if (dvf.frames.empty())
            {
                done({});
                return;
            }
            const tk::VideoFrame& frame = dvf.frames.front();
            // BGRA → JPEG bytes via WIC (same recipe used by the
            // selfie-capture path). This runs on a worker thread, so —
            // as with tk::d2d::decode_image — create a per-call COM
            // apartment here rather than assuming the UI thread's.
            const HRESULT coinit_hr =
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct ComGuard
            {
                bool owned;
                ~ComGuard() { if (owned) CoUninitialize(); }
            } com_guard{coinit_hr == S_OK};
            std::vector<std::uint8_t> jpeg;
            IWICImagingFactory* wic = nullptr;
            if (SUCCEEDED(CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            {
                IStream* out = nullptr;
                CreateStreamOnHGlobal(nullptr, TRUE, &out);
                IWICBitmapEncoder* enc = nullptr;
                if (SUCCEEDED(wic->CreateEncoder(
                        GUID_ContainerFormatJpeg, nullptr, &enc)) &&
                    out)
                {
                    enc->Initialize(out, WICBitmapEncoderNoCache);
                    IWICBitmapFrameEncode* wf = nullptr;
                    if (SUCCEEDED(enc->CreateNewFrame(&wf, nullptr)))
                    {
                        wf->Initialize(nullptr);
                        wf->SetSize(static_cast<UINT>(frame.w),
                                   static_cast<UINT>(frame.h));
                        IWICBitmap* src_bmp = nullptr;
                        wic->CreateBitmapFromMemory(
                            static_cast<UINT>(frame.w),
                            static_cast<UINT>(frame.h),
                            GUID_WICPixelFormat32bppBGRA,
                            static_cast<UINT>(frame.w) * 4u,
                            static_cast<UINT>(frame.bgra.size()),
                            const_cast<std::uint8_t*>(
                                frame.bgra.data()),
                            &src_bmp);
                        if (src_bmp)
                        {
                            IWICFormatConverter* conv = nullptr;
                            if (SUCCEEDED(wic->CreateFormatConverter(
                                    &conv)))
                            {
                                conv->Initialize(
                                    src_bmp,
                                    GUID_WICPixelFormat24bppBGR,
                                    WICBitmapDitherTypeNone, nullptr,
                                    0.0, WICBitmapPaletteTypeCustom);
                                WICPixelFormatGUID fmt =
                                    GUID_WICPixelFormat24bppBGR;
                                wf->SetPixelFormat(&fmt);
                                wf->WriteSource(conv, nullptr);
                                conv->Release();
                            }
                            src_bmp->Release();
                        }
                        wf->Commit();
                        wf->Release();
                    }
                    enc->Commit();
                    enc->Release();
                    LARGE_INTEGER seek{};
                    out->Seek(seek, STREAM_SEEK_SET, nullptr);
                    STATSTG stat{};
                    out->Stat(&stat, STATFLAG_NONAME);
                    jpeg.resize(
                        static_cast<std::size_t>(stat.cbSize.QuadPart));
                    ULONG nread = 0;
                    out->Read(jpeg.data(),
                             static_cast<ULONG>(jpeg.size()), &nread);
                }
                if (out) out->Release();
                wic->Release();
            }
            done(std::move(jpeg));
        });
}

void MainWindow::cache_rgba_image_(const tk::CacheKey& key, int w, int h,
                                   std::vector<uint8_t> rgba)
{
    if (account_manager_.image_cache().contains(key) || !main_app_surface_)
    {
        return;
    }
    auto img =
        main_app_surface_->factory().create_image_rgba(rgba.data(), w, h);
    if (!img)
    {
        return;
    }
    account_manager_.image_cache().store(key, std::move(img));
    if (HWND ms = main_app_surface_->hwnd())
    {
        InvalidateRect(ms, nullptr, FALSE);
    }
}

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

void MainWindow::show_sync_status_text_(const std::string& text)
{
    if (hStatus_)
    {
        SetWindowTextW(hStatus_, utf8_to_wstr(text).c_str());
    }
}

// ---------------------------------------------------------------------------
// Avatar decode
// ---------------------------------------------------------------------------

std::unique_ptr<tk::Image>
MainWindow::decode_avatar_(const std::vector<std::uint8_t>& bytes)
{
    // The first frame only: the generic decode_image_ would decode (and then
    // discard) every frame of an animated avatar.
    if (tesseract::is_jxl_data(bytes) ||
        !tk::decode_size_allowed(bytes, tk::bytes_may_be_animated(bytes)))
        return ShellBase::decode_avatar_(bytes);
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    return tk::d2d::decode_image(backend, span,
                                 tesseract::visual::kAvatarCacheSize,
                                 tesseract::visual::kAvatarCacheSize);
}

std::vector<std::uint8_t> MainWindow::encode_selfie_jpeg_(
    const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> jpeg;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return jpeg;
    IStream* out = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &out);
    IWICBitmapEncoder* enc = nullptr;
    if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
        out)
    {
        enc->Initialize(out, WICBitmapEncoderNoCache);
        IWICBitmapFrameEncode* frame = nullptr;
        if (SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)))
        {
            frame->Initialize(nullptr);
            frame->SetSize(w, h);
            // Wrap BGRA data in a WIC bitmap and let IWICFormatConverter strip
            // the alpha channel to 24bppBGR — no manual pixel loop needed.
            IWICBitmap* src_bmp = nullptr;
            wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA,
                                        w * 4u, w * h * 4u,
                                        const_cast<BYTE*>(bgra), &src_bmp);
            if (src_bmp)
            {
                IWICFormatConverter* conv = nullptr;
                if (SUCCEEDED(wic->CreateFormatConverter(&conv)))
                {
                    conv->Initialize(src_bmp, GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&fmt);
                    frame->WriteSource(conv, nullptr);
                    conv->Release();
                }
                src_bmp->Release();
            }
            frame->Commit();
            frame->Release();
        }
        enc->Commit();
        enc->Release();
        LARGE_INTEGER seek{};
        out->Seek(seek, STREAM_SEEK_SET, nullptr);
        STATSTG stat{};
        out->Stat(&stat, STATFLAG_NONAME);
        jpeg.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        ULONG nread = 0;
        out->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &nread);
    }
    if (out)
        out->Release();
    wic->Release();
    return jpeg;
}

bool MainWindow::main_window_foreground_for_tray_() const
{
    return IsWindowVisible(hwnd_) && is_tray_owner_effectively_active_();
}

void MainWindow::hide_main_window_for_tray_()
{
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::show_main_window_for_tray_(const std::string& /*token*/)
{
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

bool MainWindow::is_tray_owner_effectively_active_() const
{
    return GetForegroundWindow() == hwnd_ || tray_owner_considered_active_;
}

bool MainWindow::is_explorer_foreground_(HWND fg)
{
    if (!fg)
    {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid)
    {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
    {
        return false;
    }
    wchar_t path[MAX_PATH];
    DWORD len = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    bool is_explorer = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &len))
    {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        is_explorer = _wcsicmp(base, L"explorer.exe") == 0;
    }
    CloseHandle(proc);
    return is_explorer;
}

void MainWindow::start_platform_services_ui_()
{
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<Win32TrayIcon>(
            hInst_, [this] { tray_activate_(); }, [this] { tray_toggle_(); },
            [this]
            {
                quitting_ = true;
                DestroyWindow(hwnd_);
            });
        if (tray_ && tray_->is_available())
        {
            // Seed the new tray with the current aggregate so an already-
            // unread state shows immediately rather than waiting for the
            // next sync tick to flip on_tray_unread_changed_.
            tray_->set_unread(last_tray_unread_, last_tray_highlight_);
            // tray_ didn't exist yet at construction time (when
            // broadcast_rebuild_tray_() last ran), so its per-account
            // submenu was never populated until now.
            broadcast_rebuild_tray_();
        }
    }
}

void MainWindow::show_user_context_menu_(int screen_x, int screen_y)
{
    const auto items = build_user_menu_items_(
        [this] { open_settings_(); },
        [this] { begin_add_account(); },
        [this] { start_qr_grant_overlay(); },
        [this] { logout_active_account_ui_(); },
        [this] { quitting_ = true; DestroyWindow(hwnd_); },
        verify_session_menu_callback_());

    HMENU menu = CreatePopupMenu();
    UINT id = 1;
    for (const auto& item : items)
    {
        if (item.label.empty())
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else
            AppendMenuW(menu, MF_STRING, id++, utf8_to_wstr(item.label).c_str());
    }
    UINT pick = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                               screen_x, screen_y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (!pick)
        return;
    UINT non_sep = 0;
    for (const auto& item : items)
    {
        if (item.label.empty())
            continue;
        if (++non_sep == pick && item.callback)
        {
            item.callback();
            break;
        }
    }
}

std::unique_ptr<tk::Image> MainWindow::make_image_from_bgra_(
    int w, int h, const std::uint8_t* bgra)
{
    return tk::d2d::make_image_from_bgra(tk::win32::backend_singleton(), bgra, w,
                                         h);
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    ShellBase::on_tab_state_changed_ui_();

    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

bool MainWindow::has_save_file_dialog_() const
{
    return true;
}

void MainWindow::prompt_passphrase_(std::string title,
                                    std::function<void(std::string)> cb)
{
    std::wstring pass =
        prompt_passphrase_w32(hwnd_, utf8_to_wstr(title).c_str());
    if (!pass.empty())
        cb(wstr_to_utf8(pass.c_str()));
}

void MainWindow::pick_save_file_(std::string /*title*/,
                                 std::string suggested_name,
                                 std::function<void(std::string)> cb)
{
    std::wstring path = show_save_dialog_(
        utf8_to_wstr(suggested_name),
        file_filter({{tk::tr("Key files"), L"*.txt"},
                     {tk::tr("All files"), L"*.*"}}).c_str());
    if (!path.empty())
        cb(wstr_to_utf8(path.c_str()));
}

void MainWindow::pick_media_save_path_(const tesseract::views::MediaSaveSpec& spec,
                                       std::function<void(std::string)> cb)
{
    const std::wstring filter = save_filter_for(spec);
    std::wstring path = show_save_dialog_(utf8_to_wstr(spec.suggested_name), filter.c_str());
    if (!path.empty())
        cb(wstr_to_utf8(path.c_str()));
}

void MainWindow::pick_open_file_(std::string /*title*/,
                                 std::function<void(std::string)> cb)
{
    wchar_t buf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    const std::wstring filter = file_filter(
        {{tk::tr("Key files"), L"*.txt"}, {tk::tr("All files"), L"*.*"}});
    ofn.lpstrFilter = filter.c_str();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        cb(wstr_to_utf8(buf));
}

void MainWindow::pick_folder_(std::string title,
                              std::function<void(std::string)> cb)
{
    wchar_t path_buf[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.pszDisplayName = path_buf;
    const std::wstring wtitle = utf8_to_wstr(title);
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return;
    wchar_t folder_buf[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, folder_buf))
        cb(wstr_to_utf8(folder_buf));
    CoTaskMemFree(pidl);
}

void MainWindow::show_message_dialog_(std::string title, std::string text,
                                      bool is_error)
{
    MessageBoxW(hwnd_, utf8_to_wstr(text).c_str(), utf8_to_wstr(title).c_str(),
                MB_OK | (is_error ? MB_ICONWARNING : MB_ICONINFORMATION));
}

// ─────────────────────────────────────────────────────────────────────────────

std::wstring MainWindow::file_filter(
    std::initializer_list<std::pair<std::string, const wchar_t*>> entries)
{
    std::wstring f;
    for (const auto& [label, pattern] : entries)
    {
        f += utf8_to_wstr(label);
        f.push_back(L'\0');
        f += pattern;
        f.push_back(L'\0');
    }
    f.push_back(L'\0');
    return f;
}

std::wstring MainWindow::save_filter_for(const tesseract::views::MediaSaveSpec& spec)
{
    if (spec.filter_name.empty() || spec.patterns.empty())
        return file_filter({{tk::tr("All files"), L"*.*"}});
    std::wstring patterns;
    for (const auto& pat : spec.patterns)
    {
        if (!patterns.empty())
            patterns += L';';
        patterns += utf8_to_wstr(pat);
    }
    return file_filter({{spec.filter_name, patterns.c_str()},
                        {tk::tr("All files"), L"*.*"}});
}

std::wstring MainWindow::show_save_dialog_(const std::wstring& suggested,
                                           const wchar_t* filter)
{
    wchar_t buf[MAX_PATH]{};
    if (!suggested.empty())
        wcsncpy_s(buf, suggested.c_str(), MAX_PATH - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn))
        return buf;
    return {};
}

std::vector<tk::Rect> MainWindow::get_screen_work_areas_() const
{
    std::vector<tk::Rect> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* r = reinterpret_cast<std::vector<tk::Rect>*>(data);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi))
            {
                r->push_back(tk::Rect{
                    static_cast<float>(mi.rcWork.left),
                    static_cast<float>(mi.rcWork.top),
                    static_cast<float>(mi.rcWork.right  - mi.rcWork.left),
                    static_cast<float>(mi.rcWork.bottom - mi.rcWork.top)});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

void MainWindow::raise_and_activate_()
{
    if (hwnd_)
        SetForegroundWindow(hwnd_);
}

void MainWindow::activate_for_notification_(const std::string&)
{
    if (!hwnd_)
        return;
    if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void MainWindow::set_window_fullscreen_(bool on)
{
    if (!hwnd_ || on == fs_active_)
        return;
    fs_active_ = on;

    const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (on)
    {
        fs_saved_placement_.length = sizeof(fs_saved_placement_);
        GetWindowPlacement(hwnd_, &fs_saved_placement_);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    else
    {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &fs_saved_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
    }
    title_bar_.invalidate_strip(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    // Re-lay-out so the status bar (hidden in full-screen) and the content
    // surface pick up the new client-area extents even if SetWindowPos didn't
    // synthesise a WM_SIZE (e.g. the SWP_NOSIZE exit path).
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    on_size(rc.right - rc.left, rc.bottom - rc.top);
}

bool MainWindow::spawn_relaunch_(const std::vector<std::string>& args)
{
    wchar_t exe_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0)
        return false;

    // relaunch_args() are ASCII without spaces (profile names are
    // [A-Za-z0-9_-]), so only the exe path needs quoting.
    std::wstring cmd = std::wstring(L"\"") + exe_path + L"\"";
    for (const auto& arg : args)
    {
        cmd += L" ";
        for (char c : arg)
            cmd.push_back(static_cast<wchar_t>(c));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_path, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
    {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void MainWindow::quit_app_()
{
    // The "Quit" menu item only destroys its own window; the app exits when
    // the last one goes (WM_DESTROY). Restarting must close them all.
    // Close the others first, re-reading the registry each time (WM_DESTROY
    // unregisters a window, and closing one may tear down others), and this
    // window last, since nothing may touch `this` after it is destroyed.
    for (;;)
    {
        MainWindow* other = nullptr;
        for (ShellBase* w : account_manager_.all_windows())
        {
            if (w != this)
            {
                other = dynamic_cast<MainWindow*>(w);
                if (other)
                    break;
            }
        }
        if (!other)
            break;
        other->quitting_ = true;
        if (!DestroyWindow(other->hwnd_))
            break;
    }
    quitting_ = true;
    DestroyWindow(hwnd_);
}

void MainWindow::rebuild_tray_()
{
    if (!tray_ || !tray_->is_available())
        return;

    auto items = build_tray_items_();
    tray_->rebuild_menu(std::move(items));
}

bool MainWindow::is_ctrl_held_() const
{
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

void MainWindow::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    auto* win = new win32::MainWindow(account_manager_, hInst_, taskbar_);
    win->set_initial_account(account);
    // Shared hand-off: re-point bridge at the new window, seed caches, pin, and
    // register dedicated — before the new window's deferred doLogin().
    hand_account_to_spawned_window_(win, account);
    if (!win->create(SW_SHOW))
    {
        // No HWND: give the account back to this window and drop the object.
        rebind_account_bridge_(*account, this);
        account_manager_.clear_dedicated(account->user_id);
        account_manager_.unregister_window(win);
        delete win;
        return;
    }
    win->delete_on_destroy_ = true;
    win->raise_and_activate_();
}

} // namespace win32
