#include "MainWindow.h"
#include "RoomWindow.h"
#include "CallWindow.h"
#include "views/BrandView.h"
#include "views/media_drop.h"
#include "Win32Notifier.h"
#include "Win32ScreenLock.h"
#include "Win32TrayIcon.h"
#include "LoginView.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"
#include "app/SlashCommands.h"
#include "app/status_links.h"
#include "tk/audio_playback.h"
#include "tk/i18n.h"
#include "tk/video_decode.h"

#include <thread>

#include <tesseract/account_session.h>
#include <tesseract/emoji.h>
#include <tesseract/mentions.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

#include "views/AccountPicker.h"
#include "views/text_util.h"

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

#if defined(__MINGW32__)
// mingw-w64's <mfapi.h> omits this prototype even though mfplat.dll exports it.
extern "C" HRESULT STDAPICALLTYPE MFCreateMFByteStreamOnStream(
    IStream* pStream, IMFByteStream** ppByteStream);
#endif

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
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
    item(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, IDOK, 96, 40, 46, 14, 0x0080, L"OK");
    item(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, IDCANCEL, 146, 40, 50, 14, 0x0080, L"Cancel");

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

namespace
{
// MediaBytesPayload is no longer posted by this shell — media fetches now flow
// through ShellBase::ensure_*_() → post_to_ui_ → on_media_bytes_ready_().
// The struct is kept as a forward stub so any stale queued messages can be
// safely drained in the WM_TESSERACT_MEDIA_BYTES handler.
struct MediaBytesPayload
{
    int kind; // unused in drain handler
    std::string cache_key;
    std::vector<std::uint8_t> bytes;
};
} // namespace

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

void status_bar_update_links(HWND hwnd, const std::wstring& text)
{
    auto* st =
        static_cast<StatusBarLinkState*>(GetPropW(hwnd, L"TesseractStatusLinks"));
    if (!st)
    {
        st = new StatusBarLinkState;
        SetPropW(hwnd, L"TesseractStatusLinks", st);
    }
    st->segs = tesseract::parse_status_links(wstr_to_utf8(text.c_str()));
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
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (p && !p->empty())
        {
            SetBkMode(hdc, TRANSPARENT);
            HFONT small_font = theme::font(theme::FontRole::Small);
            // Reserve space on the right for the inflight dot (DPI-scaled).
            RECT text_rc = {rc.left + MulDiv(10, sb_dpi, 96), rc.top,
                            rc.right - MulDiv(24, sb_dpi, 96), rc.bottom};
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
            const int   DOT_R = static_cast<int>(tk::kInflightDotR * sb_dpi / 96.0f);
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

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    // Sync the Win32-native palette and title bar to match the chosen mode.
    win32::theme::set_mode(t.mode == tk::ThemeMode::Dark
                               ? win32::theme::Mode::Dark
                               : win32::theme::Mode::Light);
    win32::theme::apply_window_attributes(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (hStatus_)
    {
        InvalidateRect(hStatus_, nullptr, TRUE);
    }

    // Build a tk::Theme that mirrors the Windows system accent color so D2D
    // surfaces (buttons, badges, chips, selection) use the user's chosen color.
    const COLORREF accent_cr = win32::theme::accent_colorref();

    // Derive light/dark variants from the raw system accent.
    COLORREF a_cr, ah_cr, ap_cr;
    if (t.mode == tk::ThemeMode::Dark)
    {
        // Lighten for legibility on dark backgrounds.
        a_cr  = RGB(min(255, (int)GetRValue(accent_cr) + 0x30),
                    min(255, (int)GetGValue(accent_cr) + 0x30),
                    min(255, (int)GetBValue(accent_cr) + 0x30));
        ah_cr = RGB(min(255, (int)GetRValue(a_cr) + 0x18),
                    min(255, (int)GetGValue(a_cr) + 0x18),
                    min(255, (int)GetBValue(a_cr) + 0x18));
        ap_cr = RGB(max(0, (int)GetRValue(a_cr) - 0x18),
                    max(0, (int)GetGValue(a_cr) - 0x18),
                    max(0, (int)GetBValue(a_cr) - 0x18));
    }
    else
    {
        a_cr  = accent_cr;
        ah_cr = RGB(max(0, (int)GetRValue(a_cr) - 0x1A),
                    max(0, (int)GetGValue(a_cr) - 0x1A),
                    max(0, (int)GetBValue(a_cr) - 0x1A));
        ap_cr = RGB(max(0, (int)GetRValue(a_cr) - 0x30),
                    max(0, (int)GetGValue(a_cr) - 0x30),
                    max(0, (int)GetBValue(a_cr) - 0x30));
    }

    // Black or white depending on perceived luminance.
    const float lum = (0.2126f * GetRValue(a_cr) + 0.7152f * GetGValue(a_cr)
                       + 0.0722f * GetBValue(a_cr)) / 255.f;
    const COLORREF ton_cr = (lum > 0.45f) ? RGB(0x1B, 0x1B, 0x1B)
                                           : RGB(0xFF, 0xFF, 0xFF);

    // COLORREF (0x00BBGGRR) → tk::Color::rgb(0xRRGGBB).
    auto cr2tk = [](COLORREF c) -> tk::Color {
        return tk::Color::rgb((static_cast<uint32_t>(GetRValue(c)) << 16) |
                              (static_cast<uint32_t>(GetGValue(c)) <<  8) |
                               static_cast<uint32_t>(GetBValue(c)));
    };

    // Patch accent into current_theme_ (the stable ShellBase member whose address
    // surfaces hold via set_theme's theme_ = &t pointer store).
    current_theme_                        = t;
    current_theme_.palette.accent         = cr2tk(a_cr);
    current_theme_.palette.accent_hover   = cr2tk(ah_cr);
    current_theme_.palette.accent_pressed = cr2tk(ap_cr);
    current_theme_.palette.text_on_accent = cr2tk(ton_cr);
    current_theme_.palette.unread_bg      = cr2tk(a_cr);
    current_theme_.palette.unread_text    = cr2tk(ton_cr);
    current_theme_.palette.selection      = tk::Color::rgba(
        GetRValue(a_cr), GetGValue(a_cr), GetBValue(a_cr), 0x50);

    // Update all tk surfaces with current_theme_ so they hold &current_theme_,
    // a pointer that stays valid for the shell's lifetime.
    if (branding_surface_)
    {
        branding_surface_->set_theme(current_theme_);
        branding_surface_->root()->apply_theme(current_theme_);
    }
    if (main_app_surface_)
    {
        main_app_surface_->set_theme(current_theme_);
        main_app_surface_->root()->apply_theme(current_theme_);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(current_theme_);
        emoji_picker_surface_->root()->apply_theme(current_theme_);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(current_theme_);
        sticker_picker_surface_->root()->apply_theme(current_theme_);
    }
    if (slash_popup_surface_)
    {
        slash_popup_surface_->set_theme(current_theme_);
        slash_popup_surface_->root()->apply_theme(current_theme_);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(current_theme_);
        shortcode_popup_surface_->root()->apply_theme(current_theme_);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(current_theme_);
        mention_popup_surface_->root()->apply_theme(current_theme_);
    }
    if (join_room_surface_)
    {
        join_room_surface_->set_theme(current_theme_);
        join_room_surface_->root()->apply_theme(current_theme_);
    }
    if (account_picker_surface_)
    {
        account_picker_surface_->set_theme(current_theme_);
        account_picker_surface_->root()->apply_theme(current_theme_);
    }
    if (settings_surface_)
    {
        settings_surface_->set_theme(current_theme_);
        settings_surface_->root()->apply_theme(current_theme_);
    }
    if (login_view_)
    {
        login_view_->set_theme(current_theme_);
    }

    // Pop-out room windows track the theme too.
    apply_theme_to_secondary_windows_(current_theme_);
    if (call_window_)
        call_window_->apply_theme(current_theme_);
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
    // (refresh_user_strip_) and relogin (request_relogin_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::refresh_user_strip_()
{
    populate_user_strip();
}

void MainWindow::request_relogin_(const std::string& user_id)
{
    const bool is_active =
        active_account_ && active_account_->user_id == user_id;
    if (is_active)
    {
        // ShellBase already showed "Session expired…" and cleared/stopped the
        // account; drop to the login flow.
        logout_active_account();
        return;
    }
    // A non-active account expired: forget it and drop it from the persisted
    // index so it doesn't reappear on next launch. (ShellBase already cleared
    // its stored session and stopped its sync.)
    account_manager_.remove_account(user_id);
    auto index = tesseract::SessionStore::load_index();
    index.user_ids.erase(
        std::remove(index.user_ids.begin(), index.user_ids.end(), user_id),
        index.user_ids.end());
    if (index.active_user_id == user_id && active_account_)
    {
        index.active_user_id = active_account_->user_id;
    }
    tesseract::SessionStore::save_index(index);
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    on_backup_progress(&progress);
}

void MainWindow::refresh_pickers_packs_()
{
    refresh_sticker_picker();
    refresh_emoji_picker();
}

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!main_app_)
    {
        return;
    }
    // Only prompt when there is actually an identity to verify against. On a
    // fresh/only device our own login-time bootstrap holds the cross-signing
    // keys, so "verify this device" is a dead end — check_encryption_setup_
    // drives the Fresh setup overlay instead.
    if (!is_verified && !verification_banner_dismissed_
        && foreign_cross_signing_identity_())
    {
        if (!verif_banner_visible_)
        {
            if (verif_shared_)
            {
                verif_shared_->set_state(
                    tesseract::views::VerificationBanner::State::Prompt);
            }
            main_app_->show_verif_banner(true);
            verif_banner_visible_ = true;
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
    }
    else
    {
        if (verif_banner_visible_)
        {
            main_app_->show_verif_banner(false);
            verif_banner_visible_ = false;
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
    }
}

void MainWindow::handle_verification_request_ui_(std::string flow_id,
                                                 std::string /*user_id*/,
                                                 std::string /*device_id*/,
                                                 bool incoming)
{
    active_verification_flow_id_ = std::move(flow_id);
    if (!verif_shared_)
    {
        return;
    }
    if (incoming)
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    }
    else
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_)
        {
            client_->start_sas(active_verification_flow_id_);
        }
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!verif_shared_)
    {
        return;
    }
    verif_shared_->set_emojis(emojis);
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    dismiss_encryption_setup_after_verification_();
    if (!verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Done);
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    if (hwnd_)
    {
        SetTimer(hwnd_, kVerifDoneTimerId, 1500, nullptr);
    }
}

void MainWindow::handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                   std::string reason)
{
    if (!verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    verif_shared_->set_cancel_reason(std::move(reason));
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id, std::string room_name,
    std::string sender, std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }

    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);
    NotificationPayload p{std::move(room_id),
                          std::move(room_name),
                          std::move(sender),
                          std::move(body),
                          std::move(user_id),
                          is_mention,
                          std::move(avatar_bytes),
                          std::move(image_bytes)};
    on_tesseract_notify(&p);
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
    on_inflight_ui_();
}

void MainWindow::on_inflight_ui_()
{
    if (!hStatus_)
        return;
    const auto   c  = inflight_dot_color_();
    const auto   n  = inflight_total_();
    const size_t fp = pool_pending_count_();
    const size_t sp = mut_pool_pending_count_();
    const size_t mp = pending_media_count_();
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
        wchar_t buf[192];
        if (n == 1u)
            std::swprintf(buf, 192,
                          L"1 request in flight\nmedia: %zu loading \xB7 fetch: %zu queued \xB7 send: %zu queued",
                          mp, fp, sp);
        else
            std::swprintf(buf, 192,
                          L"%u requests in flight\nmedia: %zu loading \xB7 fetch: %zu queued \xB7 send: %zu queued",
                          n, mp, fp, sp);
        inflight_tip_text_ = buf;
#ifndef NDEBUG
        if (!last_inflight_urls_.empty()) {
            inflight_tip_text_ += L"\n── requests ──\n";
            inflight_tip_text_ += utf8_to_wstr(last_inflight_urls_);
        }
#endif
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        ti.hwnd = hStatus_;
        ti.uId = reinterpret_cast<UINT_PTR>(hStatus_);
        ti.lpszText = inflight_tip_text_.data();
        SendMessageW(hStatusTip_, TTM_UPDATETIPTEXTW, 0,
                     reinterpret_cast<LPARAM>(&ti));
    }
}

void MainWindow::on_server_info_ready_ui_()
{
    if (settings_view_)
        settings_view_->set_server_info(server_info_);
    if (main_app_ && main_app_->room_view())
        main_app_->room_view()->header()->set_jump_to_date_enabled(
            server_info_.supports_msc3030);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::on_own_extended_profile_ready_ui_()
{
    if (settings_view_)
        settings_view_->set_extended_profile(own_extended_profile_);
    if (settings_surface_)
        settings_surface_->relayout();
}

void MainWindow::on_profile_field_result_ui_(const std::string& key,
                                              bool ok,
                                              const std::string& error)
{
    if (!settings_view_) return;
    settings_view_->set_profile_field_busy(key, false);
    if (!ok)
        settings_view_->set_profile_field_error(key, error);
    if (settings_surface_)
        settings_surface_->relayout();
}

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
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
    refresh_sync_status();
}

// ---------------------------------------------------------------------------
// wnd_proc
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
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

    switch (msg)
    {
    case WM_CREATE:
        self->on_create(hwnd);
        return 0;

    case WM_CLOSE:
        if (self->tray_ && self->tray_->is_available() && !self->quitting_)
        {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
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
        }
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

    case WM_MOVING:
    {
        const auto* nr = reinterpret_cast<const RECT*>(lParam);
        int dx = nr->left - self->picker_track_pos_.x;
        int dy = nr->top  - self->picker_track_pos_.y;
        self->picker_track_pos_ = {nr->left, nr->top};
        self->reposition_visible_pickers_(dx, dy);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_MOVE:
    {
        RECT wrc{};
        GetWindowRect(hwnd, &wrc);
        int dx = wrc.left - self->picker_track_pos_.x;
        int dy = wrc.top  - self->picker_track_pos_.y;
        self->picker_track_pos_ = {wrc.left, wrc.top};
        self->reposition_visible_pickers_(dx, dy);
        auto& g = tesseract::Settings::instance().main_window_geometry;
        g.x = wrc.left; g.y = wrc.top;
        g.w = wrc.right - wrc.left; g.h = wrc.bottom - wrc.top;
        g.dpi = static_cast<int>(GetDpiForWindow(hwnd));
        g.valid = true;
        self->save_settings_debounced_();
        return 0;
    }

    case WM_COMMAND:
        // Compose bar Send / Emoji clicks go through the shared widgets'
        // callbacks now — no WM_COMMAND wiring. The emoji-picker search
        // field is a NativeTextField overlay handled by its set_on_changed
        // lambda. User context menu items invoke callbacks directly in
        // show_user_context_menu_ (no PostMessageW).
        if (LOWORD(wParam) == IDC_QUICK_SWITCH)
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Character;
                event.text = "k";
                event.ctrl = true;
                self->main_app_->dispatch_key_down(event);
            }
        }
        if (LOWORD(wParam) == IDC_MESSAGE_SEARCH)
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Character;
                event.text = "f";
                event.ctrl = true;
                event.shift = true;
                self->main_app_->dispatch_key_down(event);
            }
        }
        if (LOWORD(wParam) == IDC_FIND_IN_ROOM)
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Character;
                event.text = "f";
                event.ctrl = true;
                self->main_app_->dispatch_key_down(event);
            }
        }
        if (LOWORD(wParam) == IDC_NAV_BACK)
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Left;
                event.alt = true;
                self->main_app_->dispatch_key_down(event);
            }
        }
        if (LOWORD(wParam) == IDC_NAV_FWD)
        {
            if (self->main_app_)
            {
                tk::KeyEvent event{};
                event.key = tk::Key::Right;
                event.alt = true;
                self->main_app_->dispatch_key_down(event);
            }
        }
        return 0;

    case WM_ERASEBKGND:
    {
        // The parent paints behind any pixel a child doesn't cover. Use the
        // themed window background — also keeps Mica fade-in clean during
        // resize, since the OS lerps from our colour to the new layout.
        RECT rc;
        GetClientRect(hwnd, &rc);
        self->paint_main_background(reinterpret_cast<HDC>(wParam), rc);
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

    case WM_SETTINGCHANGE:
    {
        // Watch for OS dark/light flip. lParam carries the changed area name
        // as a wide string when it originated in Personalize.
        if (lParam)
        {
            auto* name = reinterpret_cast<const wchar_t*>(lParam);
            if (name && wcscmp(name, L"ImmersiveColorSet") == 0)
            {
                self->on_system_theme_changed();
            }
        }
        return 0;
    }

    case WM_DPICHANGED:
    {
        win32::text::on_dpi_changed(LOWORD(wParam));
        theme::on_dpi_changed();
        // Move+resize to the rect Windows calculated for the new DPI.
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_TESSERACT_PAGINATE_DONE:
    {
        auto* p = reinterpret_cast<std::string*>(lParam);
        self->on_tesseract_paginate_done(p, wParam != 0);
        delete p;
        return 0;
    }
    case WM_TESSERACT_NOTIFY_CLICK:
    {
        auto* payload = reinterpret_cast<win32::NotifyClickPayload*>(lParam);
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd))
        {
            ShowWindow(hwnd, SW_RESTORE);
        }
        SetForegroundWindow(hwnd);
        // Switch to the account that owns this notification before navigating.
        self->switch_active_account(payload->user_id);
        self->navigate_to_room(payload->room_id);
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
    case WM_TESSERACT_MEDIA_BYTES:
    {
        // No longer posted by this shell — media now flows through
        // ShellBase::ensure_*_() → post_to_ui_ → on_media_bytes_ready_().
        // Keep the case to safely drain any stale messages during shutdown.
        auto* p = reinterpret_cast<MediaBytesPayload*>(lParam);
        delete p;
        return 0;
    }
    case WM_TESSERACT_JOIN_ROOM_LOOKUP_DONE:
    {
        // lParam owns a heap-allocated RoomSummary (or empty room_id = error).
        // wParam carries the generation counter; discard stale callbacks.
        auto* s = reinterpret_cast<tesseract::RoomSummary*>(lParam);
        if (static_cast<uint32_t>(wParam) == self->join_room_gen_ &&
            self->join_room_shared_)
        {
            if (s->ok())
            {
                self->join_room_shared_->set_preview(*s);
            }
            else
            {
                self->join_room_shared_->set_error("Room not found.");
            }
            if (self->join_room_surface_)
            {
                self->join_room_surface_->relayout();
            }
        }
        delete s;
        return 0;
    }
    case WM_TESSERACT_JOIN_ROOM_DONE:
    {
        // lParam owns a heap-allocated canonical room ID (empty = failure).
        // wParam carries the generation counter; discard stale callbacks.
        auto* room_id = reinterpret_cast<std::string*>(lParam);
        if (static_cast<uint32_t>(wParam) == self->join_room_gen_ &&
            self->join_room_shared_)
        {
            if (!room_id->empty())
            {
                if (self->hJoinRoom_)
                {
                    ShowWindow(self->hJoinRoom_, SW_HIDE);
                }
                self->navigate_to_room(*room_id);
            }
            else
            {
                self->join_room_shared_->set_error("Join failed.");
                if (self->join_room_surface_)
                {
                    self->join_room_surface_->relayout();
                }
            }
        }
        delete room_id;
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            const bool had_quick_switch = self->main_app_ &&
                self->main_app_->quick_switcher() &&
                self->main_app_->quick_switcher()->is_open();
            const bool had_message_search = self->main_app_ &&
                self->main_app_->message_search() &&
                self->main_app_->message_search()->is_open();
            const bool had_room_search = self->main_app_ &&
                self->main_app_->room_view() &&
                self->main_app_->room_view()->room_search_open();
            if (self->main_app_ &&
                self->main_app_->dispatch_key_down({tk::Key::Escape}))
            {
                if (had_quick_switch)
                    self->close_quick_switch_();
                else if (had_message_search)
                    self->close_message_search_();
                else if (had_room_search)
                    self->close_find_in_room_();
                else if (self->main_app_surface_)
                    self->main_app_surface_->relayout();
                return 0;
            }
        }
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
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (self->room_view_ && self->room_view_->message_list()->has_selection())
            {
                self->room_view_->message_list()->copy_selection();
                return 0;
            }
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
        if (wParam == kScrollDebounceTimerId)
        {
            KillTimer(hwnd, kScrollDebounceTimerId);
            if (self->room_list_view_ && self->active_account_)
            {
                auto ids  = self->room_list_view_->visible_room_ids();
                auto sess = self->active_account_;
                self->run_async_mut_([sess, ids = std::move(ids)]() mutable
                {
                    if (sess && sess->client)
                    {
                        sess->client->stop_background_backfill();
                        sess->client->start_background_backfill(ids);
                    }
                });
            }
            return 0;
        }
        if (wParam == kVerifDoneTimerId)
        {
            KillTimer(hwnd, kVerifDoneTimerId);
            if (self->verif_shared_ && self->verif_shared_->on_done)
            {
                self->verif_shared_->on_done();
            }
            return 0;
        }
        if (wParam == kMarkReadTimerId)
        {
            KillTimer(hwnd, kMarkReadTimerId);
            self->mark_room_read_(self->current_room_id_);
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
        if (wParam == kSyncStatusDebounceTimerId)
        {
            KillTimer(hwnd, kSyncStatusDebounceTimerId);
            self->sync_status_debounce_timer_id_ = 0;
            using RLS = tesseract::RoomListState;
            if (self->hStatus_ &&
                (self->last_room_list_state_ == RLS::Init ||
                 self->last_room_list_state_ == RLS::SettingUp))
            {
                self->sync_progress_shown_ = true;
                SetWindowTextW(self->hStatus_, L"Syncing rooms…");
            }
            return 0;
        }
        if (wParam == kStatusClearTimerId)
        {
            KillTimer(hwnd, kStatusClearTimerId);
            if (self->hStatus_)
            {
                SetWindowTextW(self->hStatus_, L"");
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (cds && cds->dwData == 1 /* matrix URI */ && cds->cbData > 0)
        {
            std::string uri(static_cast<const char*>(cds->lpData),
                            cds->cbData - 1);
            self->open_matrix_link(uri);
        }
        return TRUE;
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
    wc.lpszClassName = CLASS_NAME;
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

MainWindow::MainWindow(tesseract::AccountManager& account_manager, HINSTANCE hInst)
    : ShellBase(account_manager)
    , hInst_(hInst)
{
    set_screen_lock_(std::make_unique<win32::Win32ScreenLock>(hInst));

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

MainWindow::~MainWindow()
{
    // unregister_window is called in WM_DESTROY (before PostQuitMessage) so
    // it is not repeated here.  broadcast_rebuild_tray_ is still needed to
    // refresh any remaining windows after this C++ shell is freed.
    broadcast_rebuild_tray_();

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
    // login_view_ calls cancel_oauth() + joins its worker on destruction.
    // Tear it down while the client pointers are still alive.
    login_view_.reset();
    if (accel_)
    {
        DestroyAcceleratorTable(accel_);
        accel_ = nullptr;
    }
    theme::shutdown();
    win32::text::shutdown();
    if (gdiplus_token_)
    {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
    }
}

bool MainWindow::create(int nCmdShow)
{
    hwnd_ = CreateWindowExW(0, CLASS_NAME, L"Tesseract",
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
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::on_create(HWND hwnd)
{
    // Application accelerator table: Ctrl+K opens the quick switcher even when
    // a native edit control (compose / search) holds keyboard focus — those
    // controls eat WM_KEYDOWN before it reaches this window's wnd_proc, so a
    // plain key handler only fires when the canvas has focus.
    {
        ACCEL accs[5]{};
        accs[0].fVirt = FCONTROL | FVIRTKEY;
        accs[0].key   = 'K';
        accs[0].cmd   = IDC_QUICK_SWITCH;
        accs[1].fVirt = FALT | FVIRTKEY;
        accs[1].key   = VK_LEFT;
        accs[1].cmd   = IDC_NAV_BACK;
        accs[2].fVirt = FALT | FVIRTKEY;
        accs[2].key   = VK_RIGHT;
        accs[2].cmd   = IDC_NAV_FWD;
        accs[3].fVirt = FCONTROL | FSHIFT | FVIRTKEY;
        accs[3].key   = 'F';
        accs[3].cmd   = IDC_MESSAGE_SEARCH;
        accs[4].fVirt = FCONTROL | FVIRTKEY;
        accs[4].key   = 'F';
        accs[4].cmd   = IDC_FIND_IN_ROOM;
        accel_ = CreateAcceleratorTableW(accs, 5);
    }

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus_token_, &gsi, nullptr);
    win32::text::init();
    HDC dpi_dc = GetDC(hwnd);
    UINT dpi = dpi_dc ? GetDeviceCaps(dpi_dc, LOGPIXELSY) : 96;
    if (dpi_dc)
    {
        ReleaseDC(hwnd, dpi_dc);
    }
    win32::text::on_dpi_changed(dpi);

    // Initialise theme + DWM attributes for the caption + Mica backdrop
    // before any child controls are created so the first paint already
    // reflects dark / light mode.
    theme::register_main_window(hwnd);
    theme::apply_window_attributes(hwnd);

    // Single surface hosting the full MainAppWidget tree (sidebar + chat +
    // banners + lightbox overlays). The first Surface creation initialises the
    // D2D backend singleton which builds the Twemoji-first font fallback.
    {
        main_app_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hwnd, tk::Theme::light());

        auto mainAppRoot = tk::create_root_widget<tesseract::views::MainAppWidget>(
            &main_app_surface_->host());
        main_app_ = mainAppRoot.get();

        main_app_surface_->set_root(std::move(mainAppRoot));

        // Feed input into the PresenceTracker.
        main_app_surface_->host().set_on_user_activity(
            [this] { notify_user_activity_(); });

        // 30 s periodic tick — paired with WM_TIMER below.
        SetTimer(hwnd, kPresenceTickTimerId, 30000, nullptr);

        // Share the DWrite font fallback built by the Surface with TextRenderer
        // so the room header draws flag emoji the same way.
        win32::text::set_font_fallback(tk::win32::dwrite_font_fallback());

        // Wire borrowed sub-view pointers.
        room_list_view_ = main_app_->room_list_view();
        room_view_ = main_app_->room_view();
        verif_shared_ = main_app_->verif_banner();
        img_viewer_ = main_app_->image_viewer();
        vid_viewer_ = main_app_->video_viewer();
        room_media_view_ = main_app_->room_media_view();

        // Space nav callback.
        main_app_->on_space_back = [this]
        {
            on_space_back();
        };
        main_app_->on_quick_switch_shortcut = [this] { open_quick_switch_(); };
        main_app_->on_message_search_shortcut =
            [this] { open_message_search_(); };
        main_app_->on_find_in_room_shortcut = [this] { open_find_in_room_(); };
        main_app_->on_history_back_shortcut = [this] { navigate_history_back(); };
        main_app_->on_history_forward_shortcut =
            [this] { navigate_history_forward(); };

        tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

        // TabBar callbacks.
        main_app_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            // Ctrl+click pops the room out into its own window (and closes the
            // tab); a plain click just switches to it.
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                tab_popout_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        main_app_->tab_bar()->on_tab_closed = [this](const std::string& room_id)
        {
            tab_close(room_id);
        };

        // Provider wiring (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // UserInfo callbacks.
        main_app_->user_info()->on_primary = [this](tk::Point /*p*/)
        {
            open_account_picker();
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

        room_list_view_->on_room_selected = [this](const std::string& room_id)
        {
            // A space is not a room: clicking one drills into it rather than
            // opening it as the active room/tab (which would put the space
            // title in the room header).
            for (const auto& r : rooms_)
            {
                if (r.id == room_id && r.is_space)
                {
                    space_nav_frames_.push_back(SpaceNavFrame::capture(room_list_view_));
                    space_stack_.push_back(room_id);
                    refresh_room_list();
                    SpaceNavFrame::enter(room_list_view_);
                    return;
                }
            }
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                tab_open_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        room_list_view_->on_scroll = [this]
        {
            KillTimer(hwnd_, kScrollDebounceTimerId);
            SetTimer(hwnd_, kScrollDebounceTimerId, 300, nullptr);
        };
        room_list_view_->on_search_clear = [this]
        {
            cancel_debounce_(DebounceSlot::RoomSearch);
            pending_search_text_.clear();
            if (auto* sf = room_list_view_->search_field())
            {
                sf->set_text("");
            }
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
        room_list_view_->on_join_room_requested = [this]
        {
            open_join_room_dialog();
        };
        room_list_view_->on_unjoined_room_selected =
            [this](const tesseract::RoomSummary& s)
        {
            if (!s.avatar_url.empty())
                ensure_media_thumbnail_(s.avatar_url, 64, 64, false);
            if (main_app_)
            {
                main_app_->show_room_preview(s, make_avatar_image_provider_());
                request_relayout_();
            }
        };
        if (auto* rp = main_app_->room_preview())
        {
            rp->on_avatar_needed = [this](const std::string& mxc)
            {
                ensure_media_thumbnail_(mxc, 64, 64, false);
            };
            rp->on_join = [this, rp](const std::string& room_id)
            {
                rp->set_state(tesseract::views::RoomPreviewView::State::Joining);
                join_room_command_(room_id);
            };
            rp->on_dismiss = [this]
            {
                if (main_app_)
                    main_app_->hide_room_preview();
            };
        }

        // ── RoomView shortcode wiring (avatar/image/preview via helper) ────
        room_view_->set_shortcode_provider(
            [this](const std::string& mxc) -> std::string
            {
                return shortcode_for_mxc_(mxc);
            });
        // Avatar inside received mention pills: resolve user id -> member
        // avatar mxc -> cached image (kicking a fetch on miss; the row
        // repaints when the bytes arrive).
        room_view_->message_list()->set_mention_avatar_provider(
            [this](const std::string& user_id) -> const tk::Image*
            {
                for (const auto& m : cached_room_members_)
                {
                    if (m.user_id != user_id)
                        continue;
                    if (m.avatar_url.empty())
                        return nullptr;
                    ensure_user_avatar_(
                        m.avatar_url,
                        media_group_for_room_(current_room_id_));
                    return account_manager_.thumbnail_cache().peek(
                        m.avatar_url);
                }
                return nullptr;
            });
        if (auto player = main_app_surface_->host().make_audio_player())
        {
            room_view_->set_audio_player(std::move(player));
        }
        capture_ = main_app_surface_->host().make_audio_capture();
        wire_voice_capture_(
            room_view_,
            [this]()
            {
                if (main_app_surface_ && main_app_surface_->hwnd())
                    InvalidateRect(main_app_surface_->hwnd(), nullptr, FALSE);
            },
            [this]() { return current_room_id_; },
            [this]()
            {
                if (room_text_area_)
                    room_text_area_->set_text("");
                room_view_->set_current_text({});
            });
        room_view_->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t>
            {
                // Non-blocking: warmed bytes or empty + async fetch (repaint on
                // arrival) so playback never freezes the UI thread.
                return voice_bytes_or_fetch_(source_json,
                                             [this] { request_relayout_(); });
            });

        room_view_->on_send = [this](const std::string& body)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            // Build from the composer's mention draft so mentions become
            // matrix.to links + m.mentions; fall back to the plain body.
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->composer_draft()
                                : std::vector<tesseract::MentionSeg>{};
            bool has_mention = false;
            for (const auto& seg : draft)
            {
                if (seg.kind == tesseract::MentionSeg::Kind::Mention)
                    has_mention = true;
            }
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            std::string trimmed = tesseract::text::trim(msg.body);
            if (trimmed.empty() && !has_mention)
            {
                return;
            }
            auto outcome = dispatch_room_send_(current_room_id_, msg.body,
                                               msg.formatted_body);
            if (outcome.handled_as_command || outcome.send_result)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->set_current_text({});
            }
            else
            {
                MessageBoxW(hwnd_,
                            utf8_to_wstr(outcome.send_result.message).c_str(),
                            L"Send failed", MB_ICONWARNING);
            }
        };
        room_view_->on_send_reply =
            [this](const std::string& reply_event_id, const std::string& body)
        {
            if (body.empty() || current_room_id_.empty())
            {
                return;
            }
            client_->send_reply(current_room_id_, reply_event_id, body);
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->set_current_text({});
        };
        room_view_->on_send_edit =
            [this](const std::string& event_id, const std::string& new_body)
        {
            if (new_body.empty() || current_room_id_.empty())
            {
                return;
            }
            client_->send_edit(current_room_id_, event_id, new_body);
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->set_current_text({});
        };
        room_view_->on_send_image =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int src_w,
                   int src_h, bool is_animated, std::string reply_event_id)
        {
            if (current_room_id_.empty() || !main_app_surface_)
            {
                return;
            }
            tesseract::Result res;
            if (is_animated)
            {
                // Animated GIF/WebP: send the original bytes verbatim via
                // the MSC4230 raw path. Re-encoding would flatten the
                // animation to a single frame.
                res = client_->send_image(
                    current_room_id_, bytes, mime, filename, caption,
                    static_cast<std::uint32_t>(src_w < 0 ? 0 : src_w),
                    static_cast<std::uint32_t>(src_h < 0 ? 0 : src_h),
                    /*is_animated=*/true, reply_event_id);
            }
            else
            {
                const bool compress =
                    tesseract::Settings::instance().image_quality ==
                    tesseract::Settings::ImageQuality::Compressed;
                auto enc = main_app_surface_->host().encode_for_send(
                    bytes.data(), bytes.size(), compress);
                if (enc.bytes.empty())
                {
                    return;
                }
                std::string out_name = filename;
                if (enc.mime == "image/jpeg")
                {
                    auto dot = out_name.find_last_of('.');
                    if (dot != std::string::npos)
                    {
                        out_name = out_name.substr(0, dot);
                    }
                    out_name += ".jpg";
                }
                res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
                                          out_name, caption, enc.width,
                                          enc.height, /*is_animated=*/false,
                                          reply_event_id);
            }
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->set_current_text({});
            }
            else if (hStatus_)
            {
                SetWindowTextW(hStatus_, L"Send image failed");
            }
        };
        room_view_->on_send_video =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int w, int h,
                   std::vector<std::uint8_t> thumb_bytes, int thumb_w,
                   int thumb_h, std::uint64_t duration_ms,
                   std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_video(
                current_room_id_, bytes, mime, filename, caption,
                static_cast<std::uint32_t>(w < 0 ? 0 : w),
                static_cast<std::uint32_t>(h < 0 ? 0 : h), thumb_bytes,
                static_cast<std::uint32_t>(thumb_w < 0 ? 0 : thumb_w),
                static_cast<std::uint32_t>(thumb_h < 0 ? 0 : thumb_h),
                duration_ms, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->set_current_text({});
            }
            else if (hStatus_)
            {
                SetWindowTextW(hStatus_, L"Send video failed");
            }
        };
        room_view_->on_send_audio =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::uint64_t duration_ms, std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res =
                client_->send_audio(current_room_id_, bytes, mime, filename,
                                     caption, duration_ms, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->set_current_text({});
            }
            else if (hStatus_)
            {
                SetWindowTextW(hStatus_, L"Send audio failed");
            }
        };
        room_view_->on_send_file =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_file(current_room_id_, bytes, mime,
                                          filename, caption, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->set_current_text({});
            }
            else
            {
                if (hStatus_)
                {
                    SetWindowTextW(hStatus_, L"Send file failed");
                }
            }
        };
        room_view_->on_edit_cancelled = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->set_current_text({});
        };
        room_view_->on_edit_prefill = [this](const std::string& body)
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(body);
            }
        };
        room_view_->on_delete_requested = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->redact_event(current_room_id_, event_id);
        };
        room_view_->on_copy_event_source_requested =
            [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string json = client_->get_event_source(current_room_id_, event_id);
            if (json.empty())
            {
                return;
            }
            if (main_app_surface_)
            {
                main_app_surface_->host().set_clipboard_text(json);
                main_app_surface_->host().show_toast(tk::tr("Copied to clipboard"));
            }
        };
        room_view_->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key,
                   const std::string& source_mxc)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            if (!source_mxc.empty())
            {
                // For MSC4027 chips matrix-sdk aggregates by the mxc:// key
                // (so `key` IS the mxc URI). Look up the shortcode locally
                // so the outgoing event carries `:shortcode:` rather than
                // re-broadcasting the URI as its own shortcode.
                std::string sc = shortcode_for_mxc_(source_mxc);
                std::string shortcode =
                    sc.empty() ? std::string() : ":" + sc + ":";
                client_->send_reaction_custom(current_room_id_, event_id,
                                              source_mxc, shortcode);
                return;
            }
            client_->send_reaction(current_room_id_, event_id, key);
        };
        room_view_->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            pending_reaction_event_id_ = event_id;
            if (main_app_surface_ && main_app_surface_->hwnd())
            {
                popup_emoji_at_rect(main_app_surface_->hwnd(), anchor);
            }
            else
            {
                toggle_emoji_picker();
            }
        };
        room_view_->on_receipt_needed = [this](const std::string& eid)
        {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        room_view_->message_list()->on_tile_needed = [this](int z, int x, int y)
        {
            ensure_tile_async(z, x, y);
        };
        setup_link_clicked_(room_view_);
        room_view_->on_set_clipboard = [this](std::string_view t)
        {
            if (main_app_surface_)
                main_app_surface_->host().set_clipboard_text(t);
        };
        room_view_->message_list()->on_show_copy_menu = [this]()
        {
            if (!room_view_)
                return;
            auto* ml = room_view_->message_list();
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Copy");
            POINT pt{};
            GetCursorPos(&pt);
            int cmd = static_cast<int>(TrackPopupMenuEx(
                menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                pt.x, pt.y, hwnd_, nullptr));
            DestroyMenu(menu);
            if (cmd == 1)
                ml->copy_selection();
        };
        // Switch the surface cursor to the link/pointer shape while the
        // mouse is over a URL / map tile / file card. Empty URL clears.
        room_view_->on_link_hovered = [this](const std::string& url)
        {
            if (!main_app_surface_) return;
            main_app_surface_->set_cursor(url.empty() ? tk::win32::Cursor::Default
                                                      : tk::win32::Cursor::Pointer);
        };
        room_view_->on_near_top = [this]
        {
            if (current_room_id_.empty())
            {
                return;
            }
            request_more_history(current_room_id_);
        };
        room_view_->on_near_bottom = [this]
        {
            if (!current_room_id_.empty())
            {
                request_forward_history_(current_room_id_);
            }
        };
        room_view_->on_return_to_live = [this]
        {
            if (!current_room_id_.empty())
            {
                return_to_live_(current_room_id_);
            }
        };
        room_view_->on_scroll_to_original = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string room = current_room_id_;
            begin_focused_subscription_(room, event_id);
            run_async_mut_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        room_view_->on_date_jump = [this](std::uint64_t ts_ms)
        {
            handle_date_jump_(ts_ms);
        };
        room_view_->on_threads_button_clicked = [this]
        {
            on_threads_button_clicked();
        };
        room_view_->on_pin_requested =
            [this](const std::string& ev) { on_pin_requested(ev); };
        room_view_->on_unpin_requested =
            [this](const std::string& ev) { on_unpin_requested(ev); };
        room_view_->on_thread_open_requested =
            [this](const std::string& root)
        {
            on_thread_open_requested(root);
        };
        room_view_->on_thread_close_requested = [this]
        {
            on_thread_close_requested();
        };
        room_view_->on_thread_send =
            [this](const std::string& body, const std::string& /*formatted*/)
        {
            // RoomView has no access to the native text area's mention/
            // emoticon draft, so it always passes an empty `formatted` here —
            // rebuild it the same way on_send does so thread sends keep
            // mentions and MSC2545 custom emoji instead of plain shortcode
            // text.
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->composer_draft()
                                : std::vector<tesseract::MentionSeg>{};
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            on_thread_send_requested(msg.body, msg.formatted_body);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_thread_send_reply =
            [this](const std::string& reply_id,
                   const std::string& body,
                   const std::string& /*formatted*/)
        {
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->composer_draft()
                                : std::vector<tesseract::MentionSeg>{};
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            on_thread_send_reply_requested(reply_id, msg.body,
                                           msg.formatted_body);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_emoji = [this](tk::Rect btn)
        {
            ensure_emoji_picker_created();
            if (hEmojiPicker_ && IsWindowVisible(hEmojiPicker_))
            {
                ShowWindow(hEmojiPicker_, SW_HIDE);
                if (room_text_area_)
                    room_text_area_->set_focused(true);
            }
            else if (main_app_surface_)
            {
                popup_emoji_at_rect(main_app_surface_->hwnd(), btn);
            }
        };
        room_view_->on_sticker = [this](tk::Rect btn)
        {
            ensure_sticker_picker_created();
            if (hStickerPicker_ && IsWindowVisible(hStickerPicker_))
            {
                ShowWindow(hStickerPicker_, SW_HIDE);
                if (room_text_area_)
                    room_text_area_->set_focused(true);
            }
            else if (main_app_surface_)
            {
                popup_sticker_at_rect(main_app_surface_->hwnd(), btn);
            }
        };
        room_view_->on_fetch_room_members = [this](std::string room_id)
        {
            auto* c = client_;
            run_async_(
                [this, c, room_id = std::move(room_id)]() mutable
                {
                    auto members = c->get_room_members(room_id);
                    // Only names/avatar_urls are cached — no avatar bytes
                    // are fetched until a mention pill or the info panel
                    // actually needs one (set_mention_avatar_provider above).
                    post_to_ui_(
                        [this, room_id,
                         members = std::move(members)]() mutable
                        {
                            if (main_app_)
                            {
                                cached_room_members_ = members;
                                cached_members_room_ = room_id;
                                main_app_->room_view()->set_room_members(
                                    std::move(members));
                            }
                        });
                });
        };
        room_view_->on_save_topic = [this](std::string room_id, std::string topic)
        {
            auto* c = client_;
            run_async_mut_(
                [c, room_id = std::move(room_id), topic = std::move(topic)]()
                {
                    c->set_room_topic(room_id, topic);
                });
        };
        room_view_->on_leave_room = [this](std::string room_id)
        {
            auto* c = client_;
            if (!c) return;
            run_async_mut_(
                [this, c, room_id = std::move(room_id)]() mutable
                {
                    auto result = c->leave_room(room_id);
                    if (result.ok)
                    {
                        post_to_ui_(
                            [this]()
                            {
                                current_room_id_.clear();
                                clear_messages();
                                if (room_list_view_)
                                    room_list_view_->set_selected_room("");
                                if (main_app_surface_)
                                    main_app_surface_->relayout();
                            });
                    }
                });
        };
        room_view_->on_room_settings_opened = [this](std::string room_id)
        {
            auto* v = room_view_->room_settings_view();
            if (!v) return;
            if (!client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                seed_room_media_section_(room_id);
                seed_image_pack_tab_(room_id, v);
                return;
            }
            v->set_field_permissions(client_->can_set_room_name(room_id),
                                     client_->can_set_room_topic(room_id),
                                     client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                client_->can_set_room_encryption(room_id),
                client_->can_set_room_join_rules(room_id),
                client_->can_set_room_guest_access(room_id),
                client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(client_->room_power_levels(room_id));
            v->set_own_power_level(client_->room_own_power_level(room_id));
            seed_room_media_section_(room_id);
            fetch_room_security_state_(room_id);
            seed_image_pack_tab_(room_id, v);
        };
        room_view_->on_room_settings_avatar_upload_requested =
            [this](std::string room_id)
        {
            stage_room_settings_avatar_upload_(room_id, room_view_->room_settings_view());
        };
        room_view_->room_settings_view()->on_accept =
            [this](std::string room_id, tesseract::views::RoomSettingsChanges changes)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_(
                [this, c, room_id = std::move(room_id),
                 changes = std::move(changes)]() mutable
                {
                    auto outcome = ShellBase::apply_room_settings_(c, room_id, changes);
                    post_to_ui_([this, outcome, room_id,
                                 media_override = changes.media_override]() mutable
                    {
                        if (!room_view_) return;
                        if (auto* v = room_view_->room_settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            commit_room_media_preview_override_(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
                });
        };
        room_view_->room_settings_view()->set_image_pack_provider(
            make_static_image_provider_with_fetch_(96, 96));
        room_view_->room_settings_view()->on_image_pack_images_needed =
            [this](std::string pack_id)
        { handle_image_pack_images_needed_(pack_id, room_view_->room_settings_view()); };
        room_view_->room_settings_view()->on_image_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        { handle_image_pack_pending_image_added_(local_id, bytes, mime, room_view_->room_settings_view()); };
        // Space-root settings (wrench icon on SpaceRootView): the same
        // per-room-id permission gating / accept / avatar-upload plumbing
        // above works unchanged for a space's room id — including image
        // packs, which are ordinary room state so a space can host its own
        // (only the Media tab is skipped, since it has no meaning for a
        // space).
        main_app_->space_root()->on_settings_opened = [this](std::string room_id)
        {
            auto* v = main_app_->space_root()->settings_view();
            if (!v) return;
            if (!client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                seed_image_pack_tab_(room_id, v);
                return;
            }
            v->set_field_permissions(client_->can_set_room_name(room_id),
                                     client_->can_set_room_topic(room_id),
                                     client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                client_->can_set_room_encryption(room_id),
                client_->can_set_room_join_rules(room_id),
                client_->can_set_room_guest_access(room_id),
                client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(client_->room_power_levels(room_id));
            v->set_own_power_level(client_->room_own_power_level(room_id));
            fetch_room_security_state_(room_id);
            seed_image_pack_tab_(room_id, v);
        };
        main_app_->space_root()->on_settings_avatar_upload_requested =
            [this](std::string room_id)
        {
            stage_room_settings_avatar_upload_(
                room_id, main_app_->space_root()->settings_view());
        };
        main_app_->space_root()->settings_view()->on_accept =
            [this](std::string room_id, tesseract::views::RoomSettingsChanges changes)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_(
                [this, c, room_id = std::move(room_id),
                 changes = std::move(changes)]() mutable
                {
                    auto outcome = ShellBase::apply_room_settings_(c, room_id, changes);
                    post_to_ui_([this, outcome, room_id,
                                 media_override = changes.media_override]() mutable
                    {
                        if (!main_app_) return;
                        if (auto* v = main_app_->space_root()->settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            commit_room_media_preview_override_(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
                });
        };
        main_app_->space_root()->settings_view()->set_image_pack_provider(
            make_static_image_provider_with_fetch_(96, 96));
        main_app_->space_root()->settings_view()->on_image_pack_images_needed =
            [this](std::string pack_id)
        {
            handle_image_pack_images_needed_(
                pack_id, main_app_->space_root()->settings_view());
        };
        main_app_->space_root()->settings_view()->on_image_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        {
            handle_image_pack_pending_image_added_(
                local_id, bytes, mime, main_app_->space_root()->settings_view());
        };
        main_app_->space_root()->on_copy_to_clipboard = [this](std::string t)
        {
            if (main_app_surface_)
                main_app_surface_->host().set_clipboard_text(t);
        };
        main_app_->space_root()->on_layout_changed = [this]
        {
            if (main_app_surface_)
                main_app_surface_->relayout();
        };

        setup_dm_callbacks();
        room_view_->on_ignore_user = [this](std::string user_id)
        {
            if (client_)
                client_->ignore_user_async(std::move(user_id));
        };
        room_view_->set_repaint_requester(
            [this]
            {
                if (main_app_surface_)
                {
                    InvalidateRect(main_app_surface_->hwnd(), nullptr, FALSE);
                }
            });
        room_view_->set_post_delayed(
            [this](int ms, std::function<void()> fn)
            {
                if (main_app_surface_)
                {
                    main_app_surface_->host().post_delayed(ms, std::move(fn));
                }
            });
        room_view_->on_layout_changed = [this]
        {
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        };

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
                            already_saved ? L"Already in Saved Stickers"
                                          : L"Add to Saved Stickers");
                POINT sp{dip_to_phys(p.x), dip_to_phys(p.y)};
                ClientToScreen(main_app_surface_->hwnd(), &sp);
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         sp.x, sp.y, 0, hwnd_, nullptr);
                DestroyMenu(menu);
                if (cmd == 1)
                {
                    auto res = client_->save_sticker_to_user_pack(body, body,
                                                                  mxc, info);
                    if (!res.ok && hStatus_)
                    {
                        SetWindowTextW(hStatus_,
                                       utf8_to_wstr(res.message).c_str());
                        SetTimer(hwnd_, kStatusClearTimerId, 6000, nullptr);
                    }
                }
            });

        // ── File drop ───────────────────────────────────────────────────────
        // Drop routing is now tree-dispatched automatically (see
        // DropTarget::Drop -> Host::fire_file_drop -> Host::dispatch_file_drop);
        // RoomView::on_file_drop (the catch-all reached when a drop doesn't
        // land on anything more specific, e.g. the room's image-pack grid)
        // is driven by these provider fields instead of a per-surface lambda.
        room_view_->media_upload_limit_provider = [this]() -> std::uint64_t
        {
            return client_ ? client_->media_upload_limit() : 0;
        };
        room_view_->media_info_extractor =
            [this](std::uint32_t gen, std::vector<std::uint8_t> b, std::string m)
        {
            extract_drop_media_(gen, std::move(b), std::move(m));
        };
        room_view_->on_file_drop_outcome =
            [this](tesseract::views::FileDropOutcome outcome)
        {
            if (outcome == tesseract::views::FileDropOutcome::TooLarge)
                show_status_message_(tk::tr("File exceeds the upload limit"));
        };
        main_app_surface_->set_on_file_drop_error(
            [this](std::string reason)
            {
                show_status_message_(std::move(reason));
            });

        // ── Native overlays ──────────────────────────────────────────────────
        if (auto* sf = main_app_->room_list_view()->search_field())
        {
            sf->set_on_changed(
                [this](const std::string& q)
                {
                    pending_search_text_ = q;
                    debounce_(DebounceSlot::RoomSearch,
                              tesseract::views::RoomListView::kSearchDebounceMs,
                              [this]
                              {
                                  if (room_list_view_)
                                  {
                                      room_list_view_->set_search_text(
                                          pending_search_text_);
                                  }
                                  refresh_room_list();
                              });
                });
        }

        // Quick switcher (Ctrl+K) — search field is self-owned; only the
        // shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* qsf = main_app_->quick_switcher()->search_field())
        {
            qsf->set_overlay_inset(2.0f);
            qsf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* qs = main_app_->quick_switcher();
                    if (!qs || !qs->is_open())
                    {
                        return false;
                    }
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        qs->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        qs->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_quick_switch_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (auto* qs = main_app_->quick_switcher())
        {
            qs->on_close = [this] { close_quick_switch_(); };
        }

        // Message search (Ctrl+Shift+F) — search field is self-owned; only
        // the shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* msf = main_app_->message_search()->search_field())
        {
            msf->set_overlay_inset(2.0f);
            msf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* ms = main_app_->message_search();
                    if (!ms || !ms->is_open())
                    {
                        return false;
                    }
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        ms->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        ms->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_message_search_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (auto* ms = main_app_->message_search())
        {
            ms->on_close = [this] { close_message_search_(); };
        }

        // Forward room picker — search field is self-owned; only the
        // shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* fpf = main_app_->forward_picker()->search_field())
        {
            fpf->set_overlay_inset(2.0f);
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* fp = main_app_ ? main_app_->forward_picker() : nullptr;
                    if (!fp || !fp->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        fp->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        fp->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_forward_picker_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (main_app_ && main_app_->forward_picker())
            main_app_->forward_picker()->on_close = [this] { close_forward_picker_(); };
        // Per-room "find in conversation" (Ctrl+F) — search field is
        // self-owned; only the shell-level Up/Down/Escape nav and on_close
        // need wiring here.
        if (auto* rv = main_app_->room_view())
        {
            if (auto* bar = rv->room_search_bar())
            {
                if (auto* rif = bar->search_field())
                {
                    rif->set_overlay_inset(2.0f);
                    rif->push_popup_nav(
                        [this](tk::NavKey nk) -> bool
                        {
                            auto* rv2 = main_app_ ? main_app_->room_view() : nullptr;
                            if (!rv2 || !rv2->room_search_open())
                            {
                                return false;
                            }
                            switch (nk)
                            {
                            case tk::NavKey::Up:
                                if (rv2->on_room_search_navigate)
                                    rv2->on_room_search_navigate(-1);
                                return true;
                            case tk::NavKey::Down:
                                if (rv2->on_room_search_navigate)
                                    rv2->on_room_search_navigate(+1);
                                return true;
                            case tk::NavKey::Escape:
                                close_find_in_room_();
                                return true;
                            default:
                                return false;
                            }
                        });
                }
                bar->on_close = [this] { close_find_in_room_(); };
            }
        }

        room_text_area_ = main_app_->room_view()->compose_bar()->text_area();
        room_text_area_->set_image_resolver(make_static_image_provider_with_fetch_(28, 28));
        // All four composer popups (gif > slash > shortcode > mention) are
        // driven through the shared ComposePopups dispatch; the controllers are
        // created just below (mention, then slash/shortcode) and in the GIF
        // block.
        room_text_area_->set_on_changed(
            [this](const std::string& s)
            {
                handle_compose_text_changed_(s);
                if (room_view_)
                {
                    room_view_->set_current_text(s);
                }
                tesseract::views::dispatch_compose_text_changed(
                    s, room_text_area_->cursor_byte_pos(),
                    gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get());
            });
        room_text_area_->set_on_submit(
            [this]
            {
                if (tesseract::views::dispatch_compose_submit(
                        gif_controller_.get(), slash_controller_.get(),
                        shortcode_controller_.get(),
                        mention_controller_.get()))
                {
                    return;
                }
                on_send_clicked();
            });
        room_text_area_->push_popup_nav(
            [this](tk::NavKey nk) -> bool
            {
                return tesseract::views::dispatch_compose_nav(
                    nk, gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get());
            });

        // ── GIF picker (/gif <query>) ────────────────────────────────────────
        // Eager WS_POPUP host so the on_changed/on_submit lambdas can drive it.
        gif_popup_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                          L"STATIC", L"", WS_POPUP, 0, 0, 10, 10,
                                          nullptr, nullptr, hInst_, nullptr);
        gif_popup_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, gif_popup_hwnd_, main_app_surface_->theme());
        {
            auto w = std::make_unique<tesseract::views::GifPopup>();
            gif_popup_widget_ = w.get();
            gif_popup_surface_->set_root(std::move(w));
        }
        gif_popup_surface_->set_anim_cache(&account_manager_.anim_cache());
        // Two-stage GIF strip cell provider, parameterised on a `repaint`
        // callback so the identical body serves the main window's strip and
        // every pop-out's (each passes a repaint targeting its own popup
        // surface, self-guarded by that window's liveness token). Stored as a
        // member; pop-outs reach it via the gif_strip_image_() override.
        gif_strip_provider_ =
            [this](const tesseract::GifResult& result,
                   const std::function<void()>& repaint) -> const tk::Image*
            {
                // The strip animates strip_url (WebP/GIF, native decode), keyed
                // in anim_cache_. Serving a cached frame means animated content
                // is on screen, so ensure the tick timer runs: re-shown searches
                // take this path without re-fetching.
                if (const tk::Image* f = account_manager_.anim_cache().current_frame(result.strip_url))
                {
                    start_anim_tick_();
                    return f;
                }
                // NOTE: the static-preview fallback is returned at the *end* of
                // this lambda, AFTER the animated re-fetch is kicked below.
                // Returning it here would short-circuit re-animation on a
                // re-shown search whose anim_cache_ entry was evicted while its
                // static thumbnail lingers in gif_previews_.
                // Kick off static preview fetch only when not already cached.
                if (!gif_previews_.count(result.preview_url) &&
                    gif_preview_inflight_.insert(result.preview_url).second)
                {
                    auto alive = gif_alive_;
                    auto url = result.preview_url;
                    {
                        const std::string disk_key = gif_src_disk_key_(url);
                        auto req_id = begin_media_req_(0,
                            [this, url, disk_key, alive, repaint](
                                std::vector<std::uint8_t> bytes) mutable
                            {
                                gif_preview_inflight_.erase(url);
                                if (bytes.empty()) return;
                                // Decode off the UI thread; WIC work must not
                                // run on the message pump.
                                run_async_(
                                    [this, url, disk_key, alive, repaint,
                                     bytes = std::move(bytes)]() mutable
                                    {
                                        account_manager_.media_disk_cache().store(
                                            disk_key, bytes);
                                        using CW = tesseract::views::GifPopup;
                                        auto d = std::make_shared<DecodedImage>(
                                            decode_image_(bytes,
                                                          int(CW::kCellW) * 2,
                                                          int(CW::kCellH) * 2));
                                        post_to_ui_(
                                            [this, url, d, alive, repaint]() mutable
                                            {
                                                if (!*alive) return;
                                                if (d->still)
                                                    gif_previews_[url] =
                                                        std::move(d->still);
                                                repaint();
                                            });
                                    });
                            });
                        // Snapshot client_ on the UI thread to avoid a data
                        // race with the account-removal path.
                        auto* client_snap = client_;
                        if (!client_snap)
                        {
                            handle_media_ready_ui_(req_id, {});
                        }
                        else
                        {
                            run_async_(
                                [this, req_id, url, disk_key, client_snap]()
                                {
                                    auto bytes =
                                        account_manager_.media_disk_cache().load(disk_key);
                                    if (!bytes.empty())
                                    {
                                        post_to_ui_(
                                            [this, req_id,
                                             bytes = std::move(bytes)]() mutable
                                            {
                                                handle_media_ready_ui_(
                                                    req_id, std::move(bytes));
                                            });
                                        return;
                                    }
                                    client_snap->fetch_url_async(req_id, 0, url);
                                });
                        }
                    }
                }
                // Kick off the strip-display fetch (strip_url: WebP/GIF) — decode
                // on the worker thread. The MP4 send form is fetched at send time.
                if (gif_anim_inflight_.insert(result.strip_url).second)
                {
                    auto alive = gif_alive_;
                    auto anim_url = result.strip_url;
                    auto anim_mime = result.strip_mime;
                    {
                        const std::string disk_key = gif_src_disk_key_(anim_url);
                        auto req_id = begin_media_req_(0,
                            [this, anim_url, anim_mime, disk_key, alive, repaint](
                                std::vector<std::uint8_t> bytes) mutable
                            {
                                gif_anim_inflight_.erase(anim_url);
                                if (bytes.empty()) return;
                                run_async_(
                                    [this, anim_url, anim_mime, disk_key, alive,
                                     repaint, bytes = std::move(bytes)]() mutable
                                    {
                                        account_manager_.media_disk_cache().store(
                                            disk_key, bytes);
                                        using CW = tesseract::views::GifPopup;
                                        if (anim_mime == "video/mp4")
                                        {
                                            tk::DecodedVideoFrames dvf =
                                                tk::decode_video_frames(
                                                    bytes.data(), bytes.size(),
                                                    int(CW::kCellW) * 2,
                                                    int(CW::kCellH) * 2);
                                            post_to_ui_(
                                                [this, anim_url,
                                                 dvf = std::move(dvf),
                                                 alive, repaint]() mutable
                                                {
                                                    if (!*alive)
                                                        return;
                                                    auto& backend =
                                                        tk::win32::backend_singleton();
                                                    std::vector<
                                                        std::unique_ptr<tk::Image>>
                                                        imgs;
                                                    std::vector<int> delays;
                                                    for (auto& f : dvf.frames)
                                                    {
                                                        auto img =
                                                            tk::d2d::make_image_from_bgra(
                                                                backend, f.bgra.data(),
                                                                f.w, f.h);
                                                        if (img)
                                                        {
                                                            imgs.push_back(std::move(img));
                                                            delays.push_back(f.delay_ms);
                                                        }
                                                    }
                                                    if (!imgs.empty())
                                                    {
                                                        account_manager_.anim_cache().store(
                                                            anim_url, std::move(imgs),
                                                            std::move(delays),
                                                            static_cast<std::int64_t>(
                                                                GetTickCount64()));
                                                        start_anim_tick_();
                                                    }
                                                    repaint();
                                                });
                                        }
                                        else
                                        {
                                            auto d = std::make_shared<DecodedImage>(
                                                decode_image_(bytes,
                                                              int(CW::kCellW) * 2,
                                                              int(CW::kCellH) * 2));
                                            post_to_ui_(
                                                [this, anim_url, d, alive,
                                                 repaint]() mutable
                                                {
                                                    if (!*alive)
                                                        return;
                                                    if (!d->frames.empty())
                                                    {
                                                        account_manager_.anim_cache().store(
                                                            anim_url, std::move(d->frames),
                                                            std::move(d->delays_ms),
                                                            static_cast<std::int64_t>(
                                                                GetTickCount64()));
                                                        start_anim_tick_();
                                                    }
                                                    else if (d->still)
                                                    {
                                                        gif_previews_[anim_url] =
                                                            std::move(d->still);
                                                    }
                                                    repaint();
                                                });
                                        }
                                    });
                            });
                        auto* client_snap = client_;
                        if (!client_snap)
                        {
                            handle_media_ready_ui_(req_id, {});
                        }
                        else
                        {
                            run_async_(
                                [this, req_id, anim_url, disk_key, client_snap]()
                                {
                                    auto bytes =
                                        account_manager_.media_disk_cache().load(disk_key);
                                    if (!bytes.empty())
                                    {
                                        post_to_ui_(
                                            [this, req_id,
                                             bytes = std::move(bytes)]() mutable
                                            {
                                                handle_media_ready_ui_(
                                                    req_id, std::move(bytes));
                                            });
                                        return;
                                    }
                                    client_snap->fetch_url_async(req_id, 0, anim_url);
                                });
                        }
                    }
                }
                // Static JPEG preview shown while the animation decodes (or as
                // the permanent fallback for a non-animated result).
                if (auto it = gif_previews_.find(result.preview_url);
                    it != gif_previews_.end())
                    return it->second.get();
                return nullptr;
            };
        // The main window's own strip repaints its own surface.
        gif_popup_widget_->set_image_provider(
            [this](const tesseract::GifResult& result) -> const tk::Image*
            {
                return gif_strip_provider_(result,
                                           [this]
                                           {
                                               if (gif_popup_surface_)
                                                   gif_popup_surface_->relayout();
                                           });
            });
        {
            tesseract::views::GifController::Hooks gh;
            gh.show = [this] { show_gif_popup_(); };
            gh.hide = [this] { hide_gif_popup_(); };
            gh.repaint = [this]
            {
                if (gif_popup_surface_)
                    gif_popup_surface_->relayout();
            };
            gh.room_id = [this] { return current_room_id_; };
            gh.client = [this]() -> tesseract::Client* { return client_; };
            gh.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            gh.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            gh.post_delayed = [this](int ms, std::function<void()> fn)
            {
                if (main_app_surface_)
                    main_app_surface_->host().post_delayed(ms, std::move(fn));
            };
            gh.api_key = []() -> std::string
            { return tesseract::Settings::instance().gif_api_key; };
            gh.client_key = []() -> std::string { return "tesseract"; };
            gh.clear_composer = [this]
            {
                if (room_text_area_)
                    room_text_area_->set_text("");
                if (room_view_)
                    room_view_->set_current_text({});
            };
            gh.get_cached_gif_bytes =
                [this](const std::string& url) -> std::vector<std::uint8_t>
            {
                // Reuse the source bytes the strip persisted to disk on fetch.
                return account_manager_.media_disk_cache().load(gif_src_disk_key_(url));
            };
            gif_controller_ = std::make_unique<tesseract::views::GifController>(
                room_text_area_, gif_popup_widget_, std::move(gh));
        }

        room_text_area_->set_on_edit_last(
            [this]
            {
                return room_view_ && room_view_->edit_last_own();
            });
        // Auto-grow (set_on_height_changed) and image-paste
        // (set_on_image_paste) are wired internally by ComposeBar's own
        // constructor now — see ComposeBar::ComposeBar()'s text_area_ setup.

        // ── @mention autocomplete popup + controller ─────────────────────
        {
            mention_popup_hwnd_ = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, 0, 0,
                int(tesseract::views::MentionPopup::kWidth),
                int(tesseract::views::MentionPopup::kRowHeight), nullptr,
                nullptr, hInst_, nullptr);
            mention_popup_surface_ = std::make_unique<tk::win32::Surface>(
                hInst_, mention_popup_hwnd_, main_app_surface_->theme());
            auto pw = std::make_unique<tesseract::views::MentionPopup>();
            mention_popup_widget_ = pw.get();
            // Resolve candidate avatars from the shared avatar cache; the
            // controller prefetches them via hooks.fetch_avatar below and they
            // land here on a later repaint.
            mention_popup_widget_->set_image_provider(
                make_avatar_image_provider_());
            mention_popup_surface_->set_root(std::move(pw));

            tesseract::views::MentionController::Hooks hooks;
            hooks.show = [this](tk::Rect cursor, int rows)
            { show_mention_popup_(cursor, rows); };
            hooks.hide = [this] { hide_mention_popup_(); };
            hooks.repaint = [this]
            {
                if (mention_popup_surface_)
                    mention_popup_surface_->host().request_repaint();
            };
            hooks.room_id = [this] { return current_room_id_; };
            // Live client getter: this controller is built in on_create, before
            // client_ is assigned at login, so a snapshot would stay null. The
            // getter reads the current client on every fetch, also tracking
            // logout and account switches.
            hooks.client = [this] { return client_; };
            hooks.fetch_avatar = [this](const std::string& mxc)
            { ensure_user_avatar_(mxc); };
            hooks.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            hooks.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            mention_controller_ =
                std::make_unique<tesseract::views::MentionController>(
                    room_text_area_, client_, mention_popup_widget_,
                    std::move(hooks));
        }

        // ── /command autocomplete popup + controller ──────────────────────
        {
            slash_popup_hwnd_ = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, 0, 0,
                int(tesseract::views::SlashCommandPopup::kWidth),
                int(tesseract::views::SlashCommandPopup::kRowHeight), nullptr,
                nullptr, hInst_, nullptr);
            slash_popup_surface_ = std::make_unique<tk::win32::Surface>(
                hInst_, slash_popup_hwnd_, main_app_surface_->theme());
            {
                auto pw =
                    std::make_unique<tesseract::views::SlashCommandPopup>();
                slash_popup_widget_ = pw.get();
                slash_popup_surface_->set_root(std::move(pw));
            }
            tesseract::views::SlashCommandController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_slash_popup_(cursor, rows); };
            sh.hide = [this] { hide_slash_popup_(); };
            sh.repaint = [this]
            {
                if (slash_popup_surface_)
                    slash_popup_surface_->host().request_repaint();
            };
            sh.room_id = [this] { return current_room_id_; };
            sh.client = [this] { return client_; };
            sh.clear_composer = [this]
            {
                if (room_view_)
                    room_view_->clear_compose_text();
            };
            sh.on_selfie = [this]
            {
                if (!main_app_)
                    return;
                main_app_->is_call_active = [this] { return active_call() != nullptr; };
                main_app_->on_selfie_captured =
                    [this](std::vector<std::uint8_t> bgra,
                           std::uint32_t w, std::uint32_t h)
                    {
                        IWICImagingFactory* wic = nullptr;
                        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,
                                                    nullptr,
                                                    CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(&wic))))
                            return;
                        IStream* out = nullptr;
                        CreateStreamOnHGlobal(nullptr, TRUE, &out);
                        IWICBitmapEncoder* enc = nullptr;
                        if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg,
                                                         nullptr, &enc)) && out)
                        {
                            enc->Initialize(out, WICBitmapEncoderNoCache);
                            IWICBitmapFrameEncode* frame = nullptr;
                            if (SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)))
                            {
                                frame->Initialize(nullptr);
                                frame->SetSize(w, h);
                                // Wrap BGRA data in a WIC bitmap and let
                                // IWICFormatConverter strip the alpha channel
                                // to 24bppBGR — no manual pixel loop needed.
                                IWICBitmap* src_bmp = nullptr;
                                wic->CreateBitmapFromMemory(
                                    w, h, GUID_WICPixelFormat32bppBGRA,
                                    w * 4u, w * h * 4u,
                                    bgra.data(), &src_bmp);
                                if (src_bmp)
                                {
                                    IWICFormatConverter* conv = nullptr;
                                    if (SUCCEEDED(
                                            wic->CreateFormatConverter(&conv)))
                                    {
                                        conv->Initialize(
                                            src_bmp,
                                            GUID_WICPixelFormat24bppBGR,
                                            WICBitmapDitherTypeNone,
                                            nullptr, 0.0,
                                            WICBitmapPaletteTypeCustom);
                                        WICPixelFormatGUID fmt =
                                            GUID_WICPixelFormat24bppBGR;
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
                            std::vector<std::uint8_t> jpeg(
                                static_cast<std::size_t>(stat.cbSize.QuadPart));
                            ULONG nread = 0;
                            out->Read(jpeg.data(),
                                      static_cast<ULONG>(jpeg.size()), &nread);
                            if (jpeg.size() > 0 && main_app_ &&
                                main_app_->room_view()->compose_bar())
                            {
                                main_app_->room_view()->compose_bar()
                                    ->set_pending_image(std::move(jpeg),
                                                        "image/jpeg",
                                                        "selfie.jpg");
                            }
                        }
                        if (out) out->Release();
                        wic->Release();
                    };
                main_app_->open_camera_overlay();
            };
            sh.on_location = [this] { send_current_location_(current_room_id_); };
            slash_controller_ =
                std::make_unique<tesseract::views::SlashCommandController>(
                    room_text_area_, slash_popup_widget_, std::move(sh));
        }

        // ── :shortcode: emoji/emoticon autocomplete popup + controller ────
        {
            shortcode_popup_hwnd_ = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, 0, 0,
                int(tesseract::views::ShortcodePopup::kWidth),
                int(tesseract::views::ShortcodePopup::kRowHeight), nullptr,
                nullptr, hInst_, nullptr);
            shortcode_popup_surface_ = std::make_unique<tk::win32::Surface>(
                hInst_, shortcode_popup_hwnd_, main_app_surface_->theme());
            {
                auto pw = std::make_unique<tesseract::views::ShortcodePopup>();
                shortcode_popup_widget_ = pw.get();
                shortcode_popup_widget_->set_image_provider(
                    make_static_image_provider_with_fetch_(28, 28));
                shortcode_popup_surface_->set_root(std::move(pw));
            }
            tesseract::views::ShortcodeController::Hooks sch;
            sch.show = [this](tk::Rect cursor, int rows)
            { show_shortcode_popup_(cursor, rows); };
            sch.hide = [this] { hide_shortcode_popup_(); };
            sch.repaint = [this]
            {
                if (shortcode_popup_surface_)
                    shortcode_popup_surface_->host().request_repaint();
            };
            sch.emoticons =
                [this]() { return emoticons_for_room_(current_room_id_); };
            sch.fetch_image = [this](const std::string& url)
            { ensure_media_image_(url, 28, 28); };
            sch.resolve_image = make_static_image_provider_with_fetch_(28, 28);
            shortcode_controller_ =
                std::make_unique<tesseract::views::ShortcodeController>(
                    room_text_area_, shortcode_popup_widget_,
                    std::move(sch));
        }

        // The topic field, the new-pack-name/shortcode/rename fields, and the
        // paste-catcher are all self-owned by each RoomSettingsView instance
        // (room_view_'s and the space-root's, independently) — see
        // RoomSettingsView::name_field()/topic_field() and
        // ImagePackEditorView::new_pack_name_field()/shortcode_field()/
        // pack_name_field()/paste_catcher() — so no shell-side wiring is
        // needed for any of them.

        // ── VerificationBanner callbacks ─────────────────────────────────────
        verif_shared_->on_verify = [this]
        {
            if (client_)
            {
                client_->request_self_verification();
            }
        };
        verif_shared_->on_accept = [this]
        {
            if (client_)
            {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_match = [this]
        {
            if (client_)
            {
                client_->confirm_sas(active_verification_flow_id_);
            }
            if (verif_shared_)
            {
                verif_shared_->set_state(
                    tesseract::views::VerificationBanner::State::Confirming);
            }
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        };
        verif_shared_->on_mismatch = [this]
        {
            if (client_)
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_cancel = [this]
        {
            if (client_)
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_dismiss = [this]
        {
            verification_banner_dismissed_ = true;
            main_app_->show_verif_banner(false);
            verif_banner_visible_ = false;
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        };
        verif_shared_->on_done = [this]
        {
            main_app_->show_verif_banner(false);
            verif_banner_visible_ = false;
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        };
        verif_shared_->on_use_recovery_key = [this]
        {
            main_app_->show_verif_banner(false);
            verif_banner_visible_ = false;
            // The recovery-key entry path now lives in the encryption-setup
            // overlay (Recover mode); the old inline recovery-key banner was
            // removed.
            show_encryption_setup_overlay_(
                tesseract::views::EncryptionSetupOverlay::Mode::Recover);
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        };

        // ── Image + video viewers — providers / repaint / on_close ────────
        wire_main_app_viewers_(
            main_app_, main_app_surface_->host(),
            [this]
            {
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            });

        img_viewer_->on_save =
            [this](std::string source_url, std::string filename_hint)
        {
            std::wstring suggested(filename_hint.begin(), filename_hint.end());
            if (suggested.empty())
                suggested = L"image";
            std::wstring path = show_save_dialog_(
                suggested,
                L"Images\0*.jpg;*.jpeg;*.png;*.gif;*.webp\0All files\0*.*\0\0");
            if (path.empty())
                return;
            if (client_)
            {
                auto req_id = begin_media_req_(0,
                    [path](std::vector<std::uint8_t> bytes) mutable
                    {
                        if (!bytes.empty())
                        {
                            std::ofstream f(wstr_to_utf8(path.c_str()),
                                            std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        }
                    });
                client_->fetch_source_bytes_async(req_id, source_url);
            }
        };

        room_view_->on_image_clicked =
            [this](const tesseract::views::MessageListView::ImageHit& hit)
        {
            if (!img_viewer_ || !main_app_)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            img_viewer_->open(src_tok, thumb_tok, hit.body,
                              hit.natural_w, hit.natural_h);
            main_app_->show_image_viewer(true);
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
            // Move keyboard focus to the top-level window so its WM_KEYDOWN
            // handler receives Esc immediately. The viewer is opened by a
            // mouse click that may leave focus on a child HWND (surface /
            // native edit), which would otherwise swallow Esc until an
            // app deactivate/reactivate restores top-level focus.
            if (hwnd_)
            {
                SetFocus(hwnd_);
            }
            ensure_viewer_fullres_(src_tok);
        };

        // Avatar click → open the lightbox with the original avatar mxc.
        // Overrides the thumbnail-only wiring from
        // ShellBase::wire_main_app_widget_ so ensure_viewer_fullres_ fetches
        // the full-resolution bytes into viewer_fullres_; the viewer's
        // image_provider prefers that over the resized thumbnail entry.
        room_view_->on_avatar_clicked =
            [this](std::string url, std::string name)
        {
            if (url.empty() || !img_viewer_ || !main_app_)
            {
                return;
            }
            img_viewer_->open(url, url, name, 0, 0);
            main_app_->show_image_viewer(true);
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
            if (hwnd_)
            {
                SetFocus(hwnd_);
            }
            ensure_viewer_fullres_(url);
        };

        vid_viewer_->on_save =
            [this](std::string source_json, std::string mime_type)
        {
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            const std::string suggested_u8 = "video" + ext;
            std::wstring suggested(suggested_u8.begin(), suggested_u8.end());
            std::wstring path = show_save_dialog_(
                suggested,
                L"Videos\0*.mp4;*.webm;*.mkv\0All files\0*.*\0\0");
            if (path.empty())
                return;
            if (client_)
            {
                auto req_id = begin_media_req_(0,
                    [path](std::vector<std::uint8_t> bytes) mutable
                    {
                        if (!bytes.empty())
                        {
                            std::ofstream f(wstr_to_utf8(path.c_str()),
                                            std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        }
                    });
                client_->fetch_source_bytes_async(req_id, source_json);
            }
        };

        room_view_->on_video_clicked =
            [this](const tesseract::views::MessageListView::VideoHit& hit)
        {
            if (!vid_viewer_ || !main_app_)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            vid_viewer_->open(src_tok, thumb_tok, hit.mime_type,
                              hit.duration_ms, hit.natural_w, hit.natural_h,
                              hit.loop, hit.no_audio, hit.hide_controls);
            main_app_->show_video_viewer(true);
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
            // Focus the top-level window so its WM_KEYDOWN handler gets Esc
            // immediately (see the image-viewer path for the rationale).
            if (hwnd_)
            {
                SetFocus(hwnd_);
            }
            // Async byte fetch via begin_media_req_.
            std::string src = src_tok;
            if (client_)
            {
                auto req_id = begin_media_req_(0,
                    [this](std::vector<std::uint8_t> bytes) mutable
                    {
                        if (vid_viewer_ && !bytes.empty())
                            vid_viewer_->load_bytes(bytes.data(), bytes.size());
                    });
                client_->fetch_source_bytes_async(req_id, src);
            }
        };

        // Room media gallery cell clicks → the same lightboxes as the main
        // timeline. Per-shell (not wire_main_app_widget_) because opening a
        // lightbox needs to steal native keyboard focus so Esc is handled
        // immediately, mirroring room_view_->on_image_clicked/on_video_clicked
        // above exactly.
        if (room_media_view_)
        {
            room_media_view_->on_image_clicked =
                [this](const tesseract::views::MessageListView::ImageHit& hit)
            {
                if (!img_viewer_ || !main_app_)
                {
                    return;
                }
                const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
                const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
                img_viewer_->open(src_tok, thumb_tok, hit.body,
                                  hit.natural_w, hit.natural_h);
                main_app_->show_image_viewer(true);
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
                if (hwnd_)
                {
                    SetFocus(hwnd_);
                }
                ensure_viewer_fullres_(src_tok);
            };
            room_media_view_->on_video_clicked =
                [this](const tesseract::views::MessageListView::VideoHit& hit)
            {
                if (!vid_viewer_ || !main_app_)
                {
                    return;
                }
                const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
                const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
                vid_viewer_->open(src_tok, thumb_tok, hit.mime_type,
                                  hit.duration_ms, hit.natural_w, hit.natural_h,
                                  hit.loop, hit.no_audio, hit.hide_controls);
                main_app_->show_video_viewer(true);
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
                if (hwnd_)
                {
                    SetFocus(hwnd_);
                }
                std::string src = src_tok;
                if (client_)
                {
                    auto req_id = begin_media_req_(0,
                        [this](std::vector<std::uint8_t> bytes) mutable
                        {
                            if (vid_viewer_ && !bytes.empty())
                                vid_viewer_->load_bytes(bytes.data(), bytes.size());
                        });
                    client_->fetch_source_bytes_async(req_id, src);
                }
            };
        }

        room_view_->on_file_clicked =
            [this](const tesseract::views::MessageListView::FileHit& hit)
        {
            std::wstring suggested(hit.file_name.begin(), hit.file_name.end());
            if (suggested.empty())
                suggested = L"download";
            std::wstring path = show_save_dialog_(suggested,
                                                  L"All files\0*.*\0\0");
            if (path.empty())
                return;
            std::string url = hit.source ? hit.source->fetch_token() : std::string{};
            if (client_)
            {
                auto req_id = begin_media_req_(0,
                    [path](std::vector<std::uint8_t> bytes) mutable
                    {
                        if (!bytes.empty())
                        {
                            std::ofstream f(wstr_to_utf8(path.c_str()),
                                            std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        }
                    });
                client_->fetch_source_bytes_async(req_id, url);
            }
        };

        room_view_->set_video_player_factory(
            [this]()
            {
                return main_app_surface_->host().make_video_player();
            });
        room_view_->set_video_fetch_provider(
            [this](const std::string& src,
                   std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                if (client_)
                {
                    auto req_id = begin_media_req_(0,
                        [on_ready = std::move(on_ready)](
                            std::vector<std::uint8_t> bytes) mutable
                        {
                            on_ready(std::move(bytes));
                        });
                    client_->fetch_source_bytes_async(req_id, src);
                }
            });

        // ── set_on_layout: keep native overlays aligned ──────────────────────
        main_app_surface_->set_on_layout(
            [this]
            {
                if (!main_app_)
                {
                    return;
                }

                // Native overlays must be hidden while an image/video viewer is open —
                // Win32 child HWNDs always paint over canvas-drawn overlays.
                const bool hide = (img_viewer_ && img_viewer_->is_open()) ||
                                  (vid_viewer_ && vid_viewer_->is_open()) ||
                                  (main_app_ && main_app_->camera_overlay_open())
                                  || (main_app_ && main_app_->screen_picker_open())
                    ;

                // room_text_area_ already self-positioned/showed itself via
                // ComposeBar::arrange(), reached from the relayout that led
                // to this callback — only a `hide` force-off is needed here.
                if (room_text_area_)
                {
                    if (hide)
                        room_text_area_->set_visible(false);
                    const bool now_visible = room_text_area_->visible();
                    if (now_visible && !room_text_area_was_visible_ &&
                        focus_compose_on_show_)
                    {
                        room_text_area_->set_focused(true);
                        focus_compose_on_show_ = false;
                    }
                    room_text_area_was_visible_ = now_visible;
                }

                // Every RoomSettingsView/ImagePackEditorView field (name,
                // topic, new-pack-name, shortcode, pack-rename, paste-
                // catcher) is self-owned and hides itself via its own
                // arrange()-driven visibility — but Win32 child HWNDs always
                // paint over canvas-drawn overlays, so they still need
                // forcing off during an image/video viewer (see the
                // room-search/encryption fields' identical `hide` gating
                // above). Checked on both possible RoomSettingsView
                // instances (room_view_'s and the space-root's) — each
                // self-owns its own fields independently, so hiding both
                // unconditionally is a harmless no-op for whichever one
                // isn't currently open.
                if (hide)
                {
                    tesseract::views::RoomSettingsView* rsvs[] = {
                        main_app_ ? main_app_->room_view()->room_settings_view()
                                  : nullptr,
                        main_app_ ? main_app_->space_root()->settings_view()
                                  : nullptr,
                    };
                    for (auto* rsv : rsvs)
                    {
                        if (!rsv)
                            continue;
                        if (auto* nf = rsv->name_field())
                            nf->set_visible(false);
                        if (auto* tf = rsv->topic_field())
                            tf->set_visible(false);
                        auto* editor = rsv->image_pack_editor();
                        if (auto* f = editor->new_pack_name_field())
                            f->set_visible(false);
                        if (auto* f = editor->shortcode_field())
                            f->set_visible(false);
                        if (auto* f = editor->pack_name_field())
                            f->set_visible(false);
                        if (auto* f = editor->paste_catcher())
                            f->set_visible(false);
                    }
                }

                // Native overlays must be hidden while an image/video viewer
                // is open — Win32 child HWNDs always paint over canvas-drawn
                // overlays. Not covered by MainAppWidget's any_modal_open_()
                // gating (camera/screen-picker aren't "modals" there), so
                // force it here explicitly.
                if (hide)
                {
                    if (auto* sf = main_app_->room_list_view()->search_field())
                        sf->set_visible(false);
                    if (auto* ov = main_app_->encryption_setup())
                    {
                        if (auto* pf = ov->passphrase_field())
                            pf->set_visible(false);
                        if (auto* kf = ov->key_field())
                            kf->set_visible(false);
                    }
                    if (auto* tf = main_app_->room_view()->room_info_panel()->topic_field())
                        tf->set_visible(false);
                }
            });
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
    hStatus_ = CreateWindowExW(0, L"TesseractStatusBar", L"Not logged in",
                               WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr,
                               hInst_, nullptr);
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

    login_view_ = std::make_unique<LoginView>(hInst_, hwnd);
    // Route the homeserver-discovery debounce through the shell's worker
    // drain so a blocked discover_homeserver call can't outlive ~LoginView
    // and corrupt the heap (mirrors the SettingsController wiring below).
    login_view_->set_run_async(
        [this](std::function<void()> fn) { run_async_(std::move(fn)); });
    login_view_->set_on_success(
        [this]()
        {
            on_login_succeeded();
        });
    login_view_->set_on_cancel(
        [this]()
        {
            on_login_cancelled();
        });
    ShowWindow(login_view_->hwnd(), SW_HIDE);

    // Settings surface — same-sized child HWND, initially hidden.
    {
        settings_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hwnd_, tk::Theme::light());
        auto view = tk::create_root_widget<tesseract::views::SettingsView>(
            &settings_surface_->host());
        settings_view_ = view.get();
        stats_settings_view_ = settings_view_;
        settings_view_->on_close = [this]
        {
            close_settings_();
        };
        settings_view_->on_logout = [this]
        {
            close_settings_();
            logout_active_account();
        };
        settings_view_->on_reset_identity = [this]
        {
            // The reset overlay lives on the main window — leave settings
            // first, then start the reset flow.
            close_settings_();
            begin_crypto_identity_reset_();
        };
        settings_view_->on_theme_preference_changed =
            [this](tesseract::Settings::ThemePreference pref)
        {
            set_theme_preference_(pref);
        };
        settings_view_->on_notifications_changed = [this](bool enabled)
        {
            if (settings_controller_)
                settings_controller_->set_notifications_enabled(enabled);
        };
        settings_view_->on_hide_content_changed = [this](bool enabled)
        {
            tesseract::Settings::instance().notification_hide_content = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        settings_view_->on_image_previews_changed = [this](bool enabled)
        {
            tesseract::Settings::instance().notification_image_previews =
                enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        settings_view_->on_prefetch_changed = [this](bool enabled)
        {
            tesseract::Settings::instance().prefetch_full_media = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        settings_view_->on_group_inactive_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_inactive_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_view_->on_group_unread_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_unread_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_view_->on_inactive_period_changed = [this](int days)
        {
            auto& s = tesseract::Settings::instance();
            s.inactive_room_threshold_days = days;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_view_->on_autoscroll_unread_changed = [](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.autoscroll_unread_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
        };
        settings_view_->on_show_membership_events_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.show_room_join_leave_events = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (client_) client_->set_show_membership_events(enabled);
            if (client_ && !current_room_id_.empty())
                client_->subscribe_room(current_room_id_);
        };
        settings_view_->on_send_presence_changed = [this](bool enabled)
        {
            handle_send_presence_toggle_(enabled);
        };
        settings_view_->on_index_messages_changed = [this](bool enabled)
        {
            handle_index_messages_toggle_(enabled);
        };
#ifdef TESSERACT_GITHUB_REPO
        settings_view_->on_check_for_updates_changed = [this](bool enabled)
        {
            handle_check_for_updates_toggle_(enabled);
        };
#endif
        settings_view_->on_msc2545_legacy_compat_changed = [this](bool enabled)
        {
            handle_msc2545_legacy_compat_toggle_(enabled);
        };
        settings_view_->on_developer_mode_changed = [this](bool enabled)
        {
            handle_developer_mode_toggle_(enabled);
        };
        settings_view_->on_send_maps_urls_as_location_changed = [this](bool enabled)
        {
            handle_send_maps_urls_as_location_toggle_(enabled);
        };
        settings_view_->on_media_previews_changed =
            [this](tesseract::Settings::MediaPreviews mode)
        {
            apply_media_preview_config_(
                mode, tesseract::Settings::instance().invite_avatars);
        };
        settings_view_->on_invite_avatars_changed = [this](bool enabled)
        {
            apply_media_preview_config_(
                tesseract::Settings::instance().media_previews, enabled);
        };
        settings_view_->on_tab_changed = [this] { settings_surface_->relayout(); };
        settings_view_->on_clear_caches = [this]
        {
            clear_all_caches_([this](uint64_t local, uint64_t sdk,
                                     uint64_t memory,
                                     uint64_t mh, uint64_t mm,
                                     uint64_t dh, uint64_t dm)
            {
                if (settings_view_)
                    settings_view_->set_cache_sizes(local, sdk, memory,
                                                    mh, mm, dh, dm);
            });
        };
        settings_surface_->set_root(std::move(view));
        settings_surface_->set_theme(current_theme_);
        if (settings_surface_->hwnd())
        {
            ShowWindow(settings_surface_->hwnd(), SW_HIDE);
        }

        // Populate capture-device combos in the Media section.
        {
            auto& host = settings_surface_->host();
            settings_view_->set_audio_input_devices(
                host.enumerate_audio_inputs());
            settings_view_->set_audio_output_devices(
                host.enumerate_audio_outputs());
            settings_view_->set_camera_devices(host.enumerate_cameras());
            settings_view_->set_selected_audio_input(
                tesseract::Settings::instance().audio_input_device_id);
            settings_view_->set_selected_audio_output(
                tesseract::Settings::instance().audio_output_device_id);
            settings_view_->set_selected_camera(
                tesseract::Settings::instance().camera_device_id);
            settings_view_->on_audio_input_changed = [this](std::string id)
            {
                tesseract::Settings::instance().audio_input_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            settings_view_->on_audio_output_changed = [this](std::string id)
            {
                tesseract::Settings::instance().audio_output_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            settings_view_->on_camera_changed = [this](std::string id)
            {
                tesseract::Settings::instance().camera_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
        }
    }

    branding_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    branding_surface_->set_root(std::make_unique<tesseract::views::BrandView>());

    apply_current_theme_();

    {
        RECT wrc{};
        GetWindowRect(hwnd_, &wrc);
        picker_track_pos_ = {wrc.left, wrc.top};
    }

    // Defer login to the message loop. on_create() runs synchronously inside
    // CreateWindowExW (i.e. inside the constructor), before spawn_main_window_()
    // can call set_initial_account() on a spawned window. Posting start_login()
    // lets that pin land first, so a secondary window takes the bind path in
    // start_login() instead of re-restoring every account from disk.
    post_to_ui_([this] { start_login(); });
}

void MainWindow::on_destroy()
{
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
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the survivors;
    // it still drains its own per-window pools below.
    if (!is_pinned_window_)
    {
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
}

// run_async_ is implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h)
{
    const int STATUS_H = dip_to_phys(24.f);

    if (hStatus_)
    {
        SetWindowPos(hStatus_, nullptr, 0, h - STATUS_H, w, STATUS_H,
                     SWP_NOZORDER);
        SendMessageW(hStatus_, WM_SIZE, 0, 0);
    }

    int content_h = h - STATUS_H;

    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        SetWindowPos(branding_surface_->hwnd(), nullptr, 0, 0, w, content_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        branding_surface_->relayout();
        return;
    }

    if (login_visible_ && login_view_ && login_view_->hwnd())
    {
        SetWindowPos(login_view_->hwnd(), nullptr, 0, 0, w, content_h,
                     SWP_NOZORDER);
        login_view_->layout(w, content_h);
        return;
    }

    if (settings_visible_ && settings_surface_ && settings_surface_->hwnd())
    {
        SetWindowPos(settings_surface_->hwnd(), nullptr, 0, 0, w, content_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        settings_surface_->relayout();
        return;
    }

    // The MainAppWidget tree owns all sidebar/chat/banner layout internally.
    // Just resize the single surface to fill the content area and relayout.
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        SetWindowPos(main_app_surface_->hwnd(), nullptr, 0, 0, w, content_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        main_app_surface_->relayout();
    }
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

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Restoring session…"));

    // Migrate + restore every stored account (shared loop in ShellBase). The
    // native per-account notifier construction runs through
    // install_account_notifier_ below; Win32 has no UnifiedPush connector.
    auto restore = restore_all_accounts_();

    if (!restore.any_accounts)
    {
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_)
        {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
            if (restore.any_restore_failed)
                login_view_->show_restore_error(restore.restore_error,
                                                [this] { start_login(); });
        }
        show_login_view();
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Not logged in"));
        return;
    }

    finish_login_ui_(restore.active_uid);
}

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

void MainWindow::finish_login_ui_(const std::string& uid)
{
    switch_active_account(uid);
    ensure_settings_controller_();
}

void MainWindow::bind_settings_controller_()
{
    // settings_controller_ is freshly constructed by
    // ShellBase::ensure_settings_controller_(); install the native key/file
    // dialog hooks and bind it to the native settings view + name field.
    wire_key_dialog_callbacks_();

    if (settings_view_)
    {
        settings_view_->set_request_repaint([this]
        {
            if (settings_surface_) settings_surface_->relayout();
        });
        settings_view_->set_controller(settings_controller_.get());
        settings_view_->on_avatar_upload_requested = [this]
        { if (settings_controller_) settings_controller_->upload_avatar(); };
        settings_view_->on_avatar_remove_requested = [this]
        { if (settings_controller_) settings_controller_->remove_avatar(); };
        settings_view_->set_user_pack_image_provider(
            make_static_image_provider_with_fetch_(96, 96));
        settings_view_->on_user_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        {
            handle_user_pack_pending_image_added_(
                local_id, bytes, mime, settings_view_->user_pack_editor());
        };
    }

    settings_controller_->on_avatar_changed = [this](std::string mxc)
    {
        my_avatar_url_ = mxc;
        if (active_account_)
        {
            active_account_->avatar_url = my_avatar_url_;
        }
        settings_view_->set_avatar_url(mxc);
        settings_surface_->relayout();
        populate_user_strip();
    };

    // The name/pronouns/timezone/bio fields are self-owned by AccountSection
    // (see AccountSection::name_field()/pronouns_field()/tz_field()/
    // bio_field()) and wired by SettingsView::set_controller() above — only
    // the profile-field-changed forward remains shell-side.
    settings_view_->on_profile_field_changed =
        [this](std::string key, std::string value_json)
    {
        handle_profile_field_change_(key, value_json);
    };
}

void MainWindow::on_login_succeeded()
{
    // The pending client ran OAuth into a temp directory.
    // finalize_login_ drops it (releases SQLite handles), renames the temp dir
    // to its final per-account location, reopens a fresh client at the final
    // path, and adds the account; we do only the native finish here.
    if (!pending_login_client_)
    {
        return;
    }

    // login_view_ holds a raw alias to pending_login_client_; clear it before
    // finalize_login_ resets the client.
    if (login_view_)
    {
        login_view_->set_client(nullptr);
    }

    const auto fin = finalize_login_();

    // Reject if this account is already signed in.
    if (fin.rejected_duplicate)
    {
        if (login_view_)
        {
            login_view_->set_status_message(
                L"Already signed in as " +
                std::wstring(fin.user_id.begin(), fin.user_id.end()));
        }
        pending_login_is_add_account_ = false;
        if (add_account_return_idx_ >= 0 &&
            add_account_return_idx_ < static_cast<int>(account_manager_.accounts().size()))
        {
            switch_active_account(account_manager_.accounts()[add_account_return_idx_]->user_id);
        }
        add_account_return_idx_ = -1;
        return;
    }

    if (!fin.ok)
    {
        // Persist failed (e.g. cross-device rename + copy, or restore error);
        // re-arm the login view so the user can retry.
        if (login_view_)
        {
            pending_login_client_ = std::make_unique<tesseract::Client>();
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_status_message(L"Failed to save session.");
            login_view_->reset();
        }
        return;
    }

    switch_active_account(fin.user_id);
    ensure_settings_controller_();
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
}

void MainWindow::open_settings_()
{
    if (!settings_view_ || !settings_surface_)
    {
        return;
    }
    settings_view_->set_account_info(my_display_name_, my_user_id_,
                                     my_avatar_url_);
    settings_view_->set_image_provider(make_avatar_image_provider_());
    settings_view_->load_persisted_settings();
    settings_surface_->relayout();

    compute_cache_sizes_([this](uint64_t local, uint64_t sdk, uint64_t memory,
                                uint64_t mh, uint64_t mm,
                                uint64_t dh, uint64_t dm)
    {
        if (settings_view_)
            settings_view_->set_cache_sizes(local, sdk, memory, mh, mm, dh,
                                            dm);
    });

    settings_visible_ = true;
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_HIDE);
    }
    if (settings_surface_ && settings_surface_->hwnd())
    {
        ShowWindow(settings_surface_->hwnd(), SW_SHOW);
    }
    start_search_index_stats_poll_();

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::close_settings_()
{
    settings_visible_ = false;
    stop_search_index_stats_poll_();
    if (settings_surface_ && settings_surface_->hwnd())
    {
        ShowWindow(settings_surface_->hwnd(), SW_HIDE);
    }
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_SHOW);
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::show_login_view()
{
    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        branding_visible_ = false;
        ShowWindow(branding_surface_->hwnd(), SW_HIDE);
    }
    login_visible_ = true;
    if (login_view_ && login_view_->hwnd())
    {
        ShowWindow(login_view_->hwnd(), SW_SHOW);
    }
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_HIDE);
    }
    if (settings_surface_ && settings_surface_->hwnd())
    {
        ShowWindow(settings_surface_->hwnd(), SW_HIDE);
    }
    settings_visible_ = false;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::show_main_content()
{
    if (branding_visible_ && branding_surface_ && branding_surface_->hwnd())
    {
        branding_visible_ = false;
        ShowWindow(branding_surface_->hwnd(), SW_HIDE);
    }
    login_visible_ = false;
    settings_visible_ = false;
    if (login_view_ && login_view_->hwnd())
    {
        ShowWindow(login_view_->hwnd(), SW_HIDE);
    }
    if (settings_surface_ && settings_surface_->hwnd())
    {
        ShowWindow(settings_surface_->hwnd(), SW_HIDE);
    }
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        ShowWindow(main_app_surface_->hwnd(), SW_SHOW);
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

void MainWindow::on_send_clicked()
{
    if (room_view_)
    {
        room_view_->compose_bar()->trigger_send();
    }
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void MainWindow::on_tesseract_notify(const NotificationPayload* p)
{
    bool win_visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    bool win_focused = (GetForegroundWindow() == hwnd_);

    for (auto& sess : account_manager_.accounts())
    {
        if (sess->user_id != p->user_id)
        {
            continue;
        }
        // Already watching this exact room — suppress silently.
        if (win_focused && active_account_ &&
            active_account_->user_id == p->user_id &&
            current_room_id_ == p->room_id)
        {
            return;
        }
        // Window on screen: no popup. Flash if not focused.
        if (win_visible)
        {
            if (!win_focused)
            {
                request_attention_();
            }
            return;
        }
        // Window minimised / hidden: send system notification.
        if (sess->notifier)
        {
            tesseract::Notification n;
            n.room_id = p->room_id;
            n.room_name = p->room_name;
            n.sender = p->sender;
            n.body = p->body;
            n.is_mention = p->is_mention;
            n.avatar_bytes = p->avatar_bytes;
            n.image_bytes = p->image_bytes;
            sess->notifier->notify(n);
        }
        return;
    }
}

void MainWindow::request_attention_()
{
    FLASHWINFO fwi{};
    fwi.cbSize = sizeof(fwi);
    fwi.hwnd = hwnd_;
    fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    fwi.uCount = 3;
    FlashWindowEx(&fwi);
}

bool MainWindow::pre_translate_message(MSG* msg)
{
    if (accel_ && hwnd_ && TranslateAcceleratorW(hwnd_, accel_, msg))
    {
        return true;
    }
    return false;
}

void MainWindow::open_quick_switch_()
{
    if (!main_app_ || !main_app_->quick_switcher())
    {
        return;
    }
    // The Ctrl+K accelerator is application-wide (translated in the message
    // loop against this window), so it fires even while a pop-out room window
    // holds focus. Bring the main window forward (the switcher lives here) so
    // it is visible. Harmless no-op when this window is already foreground.
    if (IsIconic(hwnd_))
    {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    main_app_->show_quick_switch(true);
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::close_quick_switch_()
{
    if (main_app_)
    {
        main_app_->show_quick_switch(false);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    // Restore focus: prefer the compose bar when a room is open, otherwise
    // fall back to the main window so Esc / nav keys reach wnd_proc.
    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
    else
    {
        SetFocus(hwnd_);
    }
}

void MainWindow::open_message_search_()
{
    if (!main_app_ || !main_app_->message_search())
    {
        return;
    }
    // Application-wide accelerator: may fire while a pop-out holds focus. Bring
    // the main window forward (search lives here) so its field can focus.
    if (IsIconic(hwnd_))
    {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    main_app_->show_message_search(true);
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::close_message_search_()
{
    if (main_app_)
    {
        main_app_->show_message_search(false);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
    else
    {
        SetFocus(hwnd_);
    }
}

void MainWindow::close_forward_picker_()
{
    if (main_app_ && main_app_->forward_picker())
        main_app_->forward_picker()->close();
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

void MainWindow::open_find_in_room_()
{
    auto* rv = main_app_ ? main_app_->room_view() : nullptr;
    if (!rv || !rv->has_room())
    {
        return;
    }
    if (IsIconic(hwnd_))
    {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    rv->open_room_search();
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::close_find_in_room_()
{
    auto* rv = main_app_ ? main_app_->room_view() : nullptr;
    if (rv)
    {
        rv->close_room_search();
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
    else
    {
        SetFocus(hwnd_);
    }
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (room_list_view_)
    {
        room_list_view_->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
}

// ---------------------------------------------------------------------------
// Room selection
// ---------------------------------------------------------------------------

void MainWindow::on_room_selected(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }

    if (const auto* r = room_by_id_(room_id); r && r->is_space)
    {
        space_nav_frames_.push_back(SpaceNavFrame::capture(room_list_view_));
        space_stack_.push_back(room_id);
        refresh_room_list();
        SpaceNavFrame::enter(room_list_view_);
        return;
    }

    // Route through the controllers so their visible_ state stays in sync.
    if (slash_controller_)
        slash_controller_->hide();
    if (shortcode_controller_)
        shortcode_controller_->hide();
    if (mention_controller_)
        mention_controller_->hide();
    handle_compose_room_leaving_(current_room_id_);
    // (No unsubscribe-on-leave here: ShellBase::prune_warm_subscriptions_ owns
    // timeline lifecycle via the warm-subscription LRU.)
    current_room_id_ = room_id;
    clear_focused_state_(room_id);
    KillTimer(hwnd_, kMarkReadTimerId);
    SetTimer(hwnd_, kMarkReadTimerId,
             static_cast<UINT>(
                 tesseract::Settings::instance().mark_as_read_delay_ms),
             nullptr);
    reply_details_requested_.clear();
    persist_room_layout_pref_();
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
    }
    if (room_text_area_)
    {
        room_text_area_->set_text("");
        // Deliberately NOT redundant with RoomView::set_room()'s own
        // default-focus policy (which this call precedes): that policy
        // calls the exact same set_focused(true) path, so on a hidden HWND
        // (first room load) it would silently no-op too. This block's real
        // job is the deferred-retry below — always request focus, set it
        // immediately if the HWND is already visible (room switch), and if
        // it's hidden (first load), record the intent and apply it in the
        // layout callback once the text area transitions to visible.
        focus_compose_on_show_ = true;
        if (room_text_area_->visible())
            room_text_area_->set_focused(true);
    }
    if (room_view_)
    {
        room_view_->set_current_text({});
    }
    update_typing_bar_({}, false);

    if (const auto* r = room_by_id_(current_room_id_))
    {
        if (room_view_)
        {
            room_view_->set_room(*r);
        }
    }
    // Subscribe (mut pool) + initial history (shared pool). The split keeps the
    // network paginate off the single mut thread so the next switch's reset is
    // never blocked. See ShellBase::start_room_subscription_.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    start_room_subscription_(current_room_id_, std::move(visible_ids));
}

void MainWindow::request_more_history(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    state.in_flight = true;
    if (room_view_)
        room_view_->set_paginating(true);

    HWND hwnd = hwnd_;
    tesseract::Client* cl = client_;
    run_async_(
        [this, room_id, hwnd, cl]
        {
            auto pr = cl->paginate_back_with_status(room_id, kPaginationBatch);
            auto* p = new std::string(room_id);
            PostMessageW(hwnd, WM_TESSERACT_PAGINATE_DONE,
                         static_cast<WPARAM>(pr.ok && pr.reached_start),
                         reinterpret_cast<LPARAM>(p));
        });
}

void MainWindow::on_tesseract_paginate_done(std::string* room_id,
                                            bool reached_start)
{
    if (!room_id)
    {
        return;
    }
    push_paginate_result_(*room_id, reached_start);
    if (*room_id == current_room_id_ && room_view_)
    {
        room_view_->message_list()->reset_near_top_latch();
    }
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

void MainWindow::on_tesseract_rooms(RoomsPayload* payload)
{
    // push_rooms_ updates per_account_rooms_, rooms_, and calls on_rooms_updated_().
    push_rooms_(std::move(payload->user_id), std::move(payload->rooms));
}

void MainWindow::on_rooms_updated_()
{
    refresh_room_list();
    if (!current_room_id_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == current_room_id_)
            {
                if (room_view_)
                {
                    room_view_->set_room(r);
                }
                break;
            }
        }
    }
    else if (!pending_restore_rooms_.empty())
    {
        if (try_restore_tab_session_(pending_restore_rooms_,
                                     pending_restore_rooms_[0]))
            pending_restore_rooms_.clear();
    }

    update_secondary_room_infos_();
}

void MainWindow::on_invites_updated_()
{
    if (room_list_view_)
    {
        room_list_view_->set_invites(&invites_);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_space_children_cache_ready_ui_()
{
    refresh_room_list();
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refresh_room_list();
}

void MainWindow::on_join_room_outcome_ui_(bool ok, const std::string&)
{
    if (!ok && main_app_ && main_app_->room_preview())
        main_app_->room_preview()->set_state(
            tesseract::views::RoomPreviewView::State::Idle);
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
}

void MainWindow::refresh_room_list()
{
    refresh_room_list_();
}

void MainWindow::on_space_back()
{
    if (!space_stack_.empty())
        space_stack_.pop_back();
    if (main_app_)
        main_app_->hide_room_preview();
    if (main_app_)
        main_app_->hide_space_root();
    refresh_room_list();
    if (!space_nav_frames_.empty())
    {
        space_nav_frames_.back().restore(room_list_view_);
        space_nav_frames_.pop_back();
    }
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

// These helpers used to call the synchronous Rust FFI directly on
// the UI thread, which froze the message pump for minutes on first sync
// (one network round-trip per room avatar, serialised on the UI thread).
// Decode + cache + repaint now happens via WM_TESSERACT_MEDIA_BYTES;
// the call sites return immediately and the next paint shows an
// initials placeholder until the bytes land.
// ensure_room_avatar_, ensure_user_avatar_, ensure_media_image_, and
// ensure_reply_details_ are implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Animated media — multi-frame WIC decode + 60 Hz WM_TIMER tick
// ---------------------------------------------------------------------------

void MainWindow::try_load_animation(const std::string& url,
                                    std::span<const std::uint8_t> bytes)
{
    if (url.empty() || bytes.empty())
    {
        return;
    }
    if (account_manager_.anim_cache().has(url))
    {
        return;
    }
    auto frames = tk::win32::decode_animation(bytes);
    if (frames.size() < 2)
    {
        return;
    }

    std::vector<std::unique_ptr<tk::Image>> imgs;
    std::vector<int> delays;
    imgs.reserve(frames.size());
    delays.reserve(frames.size());
    for (auto& af : frames)
    {
        imgs.push_back(std::move(af.image));
        delays.push_back(af.delay_ms);
    }
    account_manager_.anim_cache().store(url, std::move(imgs), std::move(delays),
                      static_cast<std::int64_t>(GetTickCount64()));
    // Drop any static-cache leftover from a prior probe.
    account_manager_.image_cache().evict(url);

    if (!anim_timer_running_ && hwnd_)
    {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerHz, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::on_anim_tick()
{
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

void MainWindow::repaint_anim_frame_()
{
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        InvalidateRect(main_app_surface_->hwnd(), nullptr, FALSE);
    }
    if (sticker_picker_surface_ && sticker_picker_surface_->hwnd() &&
        hStickerPicker_ && IsWindowVisible(hStickerPicker_))
    {
        if (sticker_picker_shared_)
        {
            sticker_picker_shared_->invalidate_image_cache();
        }
        InvalidateRect(sticker_picker_surface_->hwnd(), nullptr, FALSE);
    }
    if (emoji_picker_surface_ && emoji_picker_surface_->hwnd() &&
        hEmojiPicker_ && IsWindowVisible(hEmojiPicker_))
    {
        if (emoji_picker_shared_)
        {
            emoji_picker_shared_->invalidate_image_cache();
        }
        InvalidateRect(emoji_picker_surface_->hwnd(), nullptr, FALSE);
    }
    if (gif_popup_surface_ && gif_popup_visible_())
        gif_popup_surface_->update_anim_regions();
    if (settings_visible_ && settings_surface_ && settings_surface_->hwnd())
    {
        // Settings' "Emojis & Stickers" tab (UserPackEditor/KnownPacksList)
        // can show animated stickers too — it has its own top-level surface,
        // separate from main_app_surface_, so it needs its own invalidate
        // here or animated frames only advance on mouse-move-driven repaints.
        InvalidateRect(settings_surface_->hwnd(), nullptr, FALSE);
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
        main_app_surface_->host().post_delayed(ms, std::move(fn));
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                       MediaKind kind,
                                       std::vector<uint8_t> bytes)
{
    // Called on the UI thread (via post_to_ui_ → WM_TESSERACT_POST_TO_UI).
    // Decode the bytes, store in the appropriate tk::Image cache, and repaint
    // the relevant surface.
    if (bytes.empty())
    {
        return;
    }

    if (!main_app_surface_)
    {
        return;
    }
    HWND invalidate_hwnd = main_app_surface_->hwnd();
    switch (kind)
    {
    case MediaKind::RoomAvatar:
    case MediaKind::UserAvatar:
    {
        if (account_manager_.thumbnail_cache().contains(cache_key))
        {
            return;
        }
        // Decode off the UI thread (WIC factory is free-threaded — same basis
        // as the MediaImage path below). A burst of avatar fetches (e.g. after
        // a room switch) would otherwise decode synchronously here and stall
        // the UI event queue that a just-sent message's local echo waits in.
        // Store + relayout hop back to the UI thread.
        run_async_(
            [this, cache_key, kind, invalidate_hwnd,
             bytes = std::move(bytes)]() mutable
            {
                auto d = std::make_shared<DecodedImage>(
                    decode_image_(bytes, 0, 0));
                post_to_ui_(
                    [this, cache_key, kind, invalidate_hwnd, d]() mutable
                    {
                        if (account_manager_.thumbnail_cache().contains(
                                cache_key))
                            return;
                        // Avatars render static: use the still, or the first
                        // frame of an animated source (matches the old
                        // factory().decode_image, which returned one frame).
                        std::unique_ptr<tk::Image> img;
                        if (d->still)
                            img = std::move(d->still);
                        else if (!d->frames.empty())
                            img = std::move(d->frames.front());
                        if (!img)
                            return;
                        account_manager_.thumbnail_cache().store(
                            cache_key, std::move(img));
                        if (kind == MediaKind::RoomAvatar)
                        {
                            // Coalescing — see the MediaThumbnail/MediaImage
                            // case below for why a direct, uncoalesced
                            // relayout() per completion is a problem for any
                            // burst (e.g. many room avatars after login).
                            schedule_relayout_();
                        }
                        else // UserAvatar
                        {
                            if (hAccountPicker_ &&
                                IsWindowVisible(hAccountPicker_) &&
                                account_picker_surface_)
                                account_picker_surface_->relayout();
                            if (mention_popup_visible_() &&
                                mention_popup_surface_)
                                mention_popup_surface_->relayout();
                        }
                        notify_secondary_media_ready_(cache_key, kind);
                        if (invalidate_hwnd)
                            InvalidateRect(invalidate_hwnd, nullptr, FALSE);
                    });
            });
        return;
    }
    case MediaKind::MediaImage:
    case MediaKind::MediaThumbnail:
    case MediaKind::Sticker:
    case MediaKind::Reaction:
    {
        // Decode off the UI thread — WIC factory is free-threaded (see
        // host_win32.h). decode_image_ handles both animated and still images.
        const bool is_thumb = (kind == MediaKind::MediaThumbnail);
        if ((is_thumb ? account_manager_.thumbnail_cache() : account_manager_.image_cache()).contains(cache_key) ||
            account_manager_.anim_cache().has(cache_key))
            return;
        run_async_(
            [this, cache_key, kind, is_thumb, invalidate_hwnd,
             bytes = std::move(bytes)]() mutable
            {
                auto d = std::make_shared<DecodedImage>(
                    decode_image_(bytes, 0, 0));
                post_to_ui_(
                    [this, cache_key, kind, is_thumb, invalidate_hwnd,
                     d]() mutable
                    {
                        auto& still_cache =
                            is_thumb ? account_manager_.thumbnail_cache() : account_manager_.image_cache();
                        if (still_cache.contains(cache_key) ||
                            account_manager_.anim_cache().has(cache_key))
                            return;
                        if (!d->frames.empty())
                        {
                            account_manager_.anim_cache().store(
                                cache_key, std::move(d->frames),
                                std::move(d->delays_ms),
                                static_cast<std::int64_t>(GetTickCount64()));
                            start_anim_tick_();
                            account_manager_.image_cache().evict(cache_key);
                        }
                        else if (d->still)
                        {
                            still_cache.store(cache_key,
                                              std::move(d->still));
                        }
                        else
                        {
                            if (!is_thumb)
                                account_manager_.media_disk_cache().evict(cache_key);
                            return;
                        }
                        if (room_view_)
                            room_view_->notify_image_ready(cache_key);
                        // Coalescing, not main_app_surface_->relayout()
                        // directly: a dense grid (the room media gallery)
                        // can land dozens of these completions in a tight
                        // burst, and an uncoalesced call here does a full
                        // app-wide arrange() per completion — including a
                        // full re-measure of the main chat timeline's rows
                        // (DirectWrite text-layout rebuilds on any
                        // LinkLayoutCache miss), which has nothing to do
                        // with a thumbnail arriving. schedule_relayout_()
                        // folds a burst of these into one deferred pass.
                        schedule_relayout_();
                        if (shortcode_popup_visible_() &&
                            shortcode_popup_surface_)
                            shortcode_popup_surface_->relayout();
                        if (settings_visible_ && settings_surface_ &&
                            settings_surface_->hwnd())
                            InvalidateRect(settings_surface_->hwnd(), nullptr,
                                          FALSE);
                        notify_secondary_media_ready_(cache_key, kind);
                        if (invalidate_hwnd)
                            InvalidateRect(invalidate_hwnd, nullptr, FALSE);
                    });
            });
        return;
    }
    case MediaKind::Tile:
        if (account_manager_.image_cache().contains(cache_key))
        {
            return;
        }
        if (auto img = main_app_surface_->factory().decode_image(bytes))
        {
            account_manager_.image_cache().store(cache_key, std::move(img));
            // A map tile fills a fixed-size map card and isn't a tracked row
            // media source, so it needs only a repaint — the shared
            // InvalidateRect below re-draws it (LocationMapPanner::paint re-reads
            // the tile from the cache). The old invalidate_data() + relayout()
            // did a full O(timeline) re-measure just to repaint a fixed card.
        }
        break;
    }
    notify_secondary_media_ready_(cache_key, kind);
    if (invalidate_hwnd)
    {
        InvalidateRect(invalidate_hwnd, nullptr, FALSE);
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
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.webp\0\0";
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
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int /*max_w*/,
                          int /*max_h*/)
{
    DecodedImage d;
    if (bytes.empty())
    {
        return d;
    }
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    auto frames = tk::d2d::decode_animation(backend, span);
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
    d.still = tk::d2d::decode_image(backend, span);
    return d;
}

std::int64_t MainWindow::monotonic_ms_()
{
    return static_cast<std::int64_t>(GetTickCount64());
}

void MainWindow::start_anim_tick_()
{
    if (!anim_timer_running_ && hwnd_)
    {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerHz, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::start_inflight_tick_()
{
    if (!inflight_timer_running_ && hwnd_)
    {
        SetTimer(hwnd_, kInflightTimerId, kAnimTimerHz, nullptr);
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

void MainWindow::repaint_pickers_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
        if (HWND h = main_app_surface_->hwnd())
        {
            InvalidateRect(h, nullptr, FALSE);
        }
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_ && emoji_picker_surface_->hwnd())
    {
        InvalidateRect(emoji_picker_surface_->hwnd(), nullptr, FALSE);
    }
    if (sticker_picker_surface_ && sticker_picker_surface_->hwnd())
    {
        InvalidateRect(sticker_picker_surface_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::extract_drop_media_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     std::string mime,
                                     tesseract::views::ComposeBar* target,
                                     std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes), mime = std::move(mime)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;

        // COM must be initialized on each background thread.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // ── Animated image detection (gif / webp) ──────────────────────────
        if (mime == "image/gif" || mime == "image/webp")
        {
            IWICImagingFactory* factory = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&factory))))
            {
                IStream* stream = SHCreateMemStream(
                    reinterpret_cast<const BYTE*>(bytes.data()),
                    static_cast<UINT>(bytes.size()));
                if (stream)
                {
                    IWICBitmapDecoder* decoder = nullptr;
                    if (SUCCEEDED(factory->CreateDecoderFromStream(
                            stream, nullptr, WICDecodeMetadataCacheOnDemand,
                            &decoder)))
                    {
                        UINT count = 0;
                        decoder->GetFrameCount(&count);
                        info.is_animated = count > 1;
                        decoder->Release();
                    }
                    stream->Release();
                }
                factory->Release();
            }
        }
        // ── Video: first-frame thumbnail + duration via Media Foundation ───
        else if (mime.rfind("video/", 0) == 0)
        {
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
        }
        // ── Audio: duration only via Media Foundation ──────────────────────
        else if (mime.rfind("audio/", 0) == 0)
        {
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
        }

        CoUninitialize();

        // Post result back to the UI thread. A pop-out target (guarded by its
        // liveness token) takes precedence; otherwise resolve the main
        // compose_bar() at call time to avoid a dangling pointer.
        post_to_ui_([this, info = std::move(info), target,
                     target_alive = std::move(target_alive)]() mutable
        {
            if (target)
            {
                if (target_alive && *target_alive)
                    target->update_pending_attachment(info);
            }
            else if (room_view_)
            {
                room_view_->compose_bar()->update_pending_attachment(info);
            }
        });
    });
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& /*video_url*/)
{
    // Win32 has no GStreamer/AVFoundation pipeline for first-frame extraction.
    // Record the event so the dedup set doesn't retry on every redraw; the
    // MessageListView will show its play-button placeholder over a grey card.
    video_thumb_in_flight_.insert(event_id);
}

void MainWindow::cache_rgba_image_(const std::string& key, int w, int h,
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

// ---------------------------------------------------------------------------
//  Event → MessageRowData + append into the shared MessageListView
// ---------------------------------------------------------------------------

void MainWindow::ensure_row_media(const tesseract::Event& ev)
{
    // Delegate to the ShellBase implementation which calls ensure_user_avatar_,
    // ensure_media_image_, generate_video_thumbnail_, etc.
    ensure_row_media_(ev);
}

void MainWindow::clear_messages()
{
    if (!room_view_)
    {
        return;
    }
    room_view_->clear_room();
    room_view_->set_messages({});
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_backup_progress(tesseract::BackupProgress* progress)
{
    if (!client_)
    {
        return;
    }

    last_backup_state_ = progress->state;
    last_imported_keys_ = progress->imported_keys;
    refresh_sync_status();
}

void MainWindow::on_room_list_state(tesseract::RoomListState state)
{
    push_room_list_state_(state);
    refresh_sync_status();
}

void MainWindow::refresh_sync_status()
{
    if (!hStatus_)
    {
        return;
    }
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init ||
                            last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy)
    {
        // Debounce 300 ms so already-warm sessions that flash through
        // Init→Running don't churn the status bar.
        if (!sync_progress_shown_ && sync_status_debounce_timer_id_ == 0)
        {
            sync_status_debounce_timer_id_ =
                SetTimer(hwnd_, kSyncStatusDebounceTimerId, 300, nullptr);
        }
        else if (sync_progress_shown_)
        {
            SetWindowTextW(hStatus_, L"Syncing rooms…");
        }
        return;
    }

    if (sync_status_debounce_timer_id_ != 0)
    {
        KillTimer(hwnd_, kSyncStatusDebounceTimerId);
        sync_status_debounce_timer_id_ = 0;
    }

    if (reconnecting)
    {
        sync_progress_shown_ = true;
        SetWindowTextW(hStatus_, L"Reconnecting…");
        return;
    }
    if (keys_busy)
    {
        sync_progress_shown_ = true;
        wchar_t buf[96];
        swprintf_s(buf, L"Downloading encryption keys (%llu)…",
                   static_cast<unsigned long long>(last_imported_keys_));
        SetWindowTextW(hStatus_, buf);
        return;
    }
    // Steady state: settle to "Connected" unless a persistent status override
    // (e.g., "Fetching older messages…" from in-room search) is still active.
    if (has_status_override_())
        return;
    sync_progress_shown_ = false;
    SetWindowTextW(hStatus_, L"Connected");
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populate_user_strip()
{
    if (!main_app_)
    {
        return;
    }
    auto* ui = main_app_->user_info();
    if (!ui)
    {
        return;
    }
    ui->set_display_name(my_display_name_);
    ui->set_user_id(my_user_id_);
    ui->set_avatar_url(my_avatar_url_);
    // Kick off async avatar fetch so the UserInfo disc fills in.
    if (!my_avatar_url_.empty())
    {
        ensure_user_avatar_(my_avatar_url_);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------
// Multi-account: switch / add / logout / cancel / picker
// ---------------------------------------------------------------------------

void MainWindow::switch_active_account(const std::string& user_id)
{
    // Platform-agnostic bookkeeping (unsubscribe previous room — folded in here
    // so Windows no longer leaks the old account's room subscription — clear
    // per-account state, swap active_account_ / aliases / identity, compute
    // pending restores, swap rooms_/invites_ snapshots, persist the index)
    // lives in ShellBase. Returns false (no-op) when the account isn't found
    // or is already active with a bound client.
    if (!switch_active_account_impl_(user_id))
    {
        return;
    }
    refresh_account_ui_after_switch_();
}

void MainWindow::refresh_account_ui_after_switch_()
{
    if (main_app_)
        main_app_->show_room();

    if (room_list_view_)
    {
        room_list_view_->set_rooms({});
    }
    if (room_view_)
    {
        room_view_->clear_room();
        room_view_->set_messages({});
    }

    if (main_app_)
    {
        main_app_->show_verif_banner(false);
    }
    verif_banner_visible_ = false;
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    populate_user_strip();
    refresh_room_list();

    show_main_content();
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Connected"));
    handle_verification_state_ui_(active_account_ && !active_account_->unverified);

    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<Win32TrayIcon>(
            hInst_,
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                ShowWindow(hwnd_, SW_SHOW);
                if (IsIconic(hwnd_))
                {
                    ShowWindow(hwnd_, SW_RESTORE);
                }
                SetForegroundWindow(hwnd_);
                navigate_tray_unread_();
            },
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                if (IsWindowVisible(hwnd_) && !last_tray_unread_)
                {
                    ShowWindow(hwnd_, SW_HIDE);
                }
                else
                {
                    ShowWindow(hwnd_, SW_SHOW);
                    if (IsIconic(hwnd_))
                    {
                        ShowWindow(hwnd_, SW_RESTORE);
                    }
                    SetForegroundWindow(hwnd_);
                    navigate_tray_unread_();
                }
            },
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
        }
    }
}

void MainWindow::begin_add_account()
{
    // Record the current active account's position in the accounts list so
    // Cancel can return to it. -1 means no active account.
    const auto& accs = account_manager_.accounts();
    add_account_return_idx_ = -1;
    if (active_account_)
    {
        for (int i = 0; i < static_cast<int>(accs.size()); ++i)
        {
            if (accs[i]->user_id == active_account_->user_id)
            {
                add_account_return_idx_ = i;
                break;
            }
        }
    }
    pending_login_is_add_account_ = true;
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    if (login_view_)
    {
        login_view_->set_client(pending_login_client_.get());
        login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
        login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
        login_view_->reset();
    }
    show_login_view();
}

void MainWindow::on_login_cancelled()
{
    if (login_view_)
    {
        login_view_->set_client(nullptr);
    }
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
        pending_login_temp_dir_.clear();
    }
    pending_login_is_add_account_ = false;

    const auto& accs = account_manager_.accounts();
    if (add_account_return_idx_ >= 0 &&
        add_account_return_idx_ < static_cast<int>(accs.size()))
    {
        switch_active_account(accs[add_account_return_idx_]->user_id);
        // The account may already be active (cancelled before completing OAuth),
        // so switch_active_account_impl_ returns false and skips the UI refresh.
        // Always navigate away from the login view explicitly.
        show_main_content();
    }
    else
    {
        show_login_view();
    }
    add_account_return_idx_ = -1;
}

void MainWindow::logout_active_account()
{
    // Platform-agnostic teardown (unsubscribe the room, up_connector/presence
    // logout, client_->logout() + failure surface, stop_sync, clear account
    // state, tray refresh, index update, and — when other accounts remain — the
    // switch to a survivor) lives in ShellBase.
    const auto result = logout_active_account_impl_();
    if (!result.logged_out)
    {
        return;
    }

    // Native widget cleanup of the now-empty surface (the remaining-account
    // branch already repainted via refresh_account_ui_after_switch_).
    if (!result.has_remaining)
    {
        if (room_list_view_)
        {
            room_list_view_->set_rooms({});
        }
        if (room_view_)
        {
            room_view_->clear_room();
            room_view_->set_messages({});
        }
        if (main_app_)
        {
            main_app_->clear_content();
            main_app_->show_verif_banner(false);
        }
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }
    verification_banner_dismissed_ = false;
    verif_banner_visible_ = false;

    if (!result.has_remaining)
    {
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_)
        {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
        show_login_view();
    }

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Signed out"));
}

void MainWindow::rebuild_account_picker()
{
    if (!hAccountPicker_)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = hInst_;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = L"TesseractAccountPicker";
            RegisterClassExW(&wc);
            registered = true;
        }
        const int kPickerW = dip_to_phys(260.f);
        const int kPickerH = dip_to_phys(
            56.f * static_cast<float>(account_manager_.accounts().size()));
        hAccountPicker_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TesseractAccountPicker", L"",
            WS_POPUP | WS_BORDER, 0, 0, kPickerW, kPickerH, hwnd_, nullptr,
            hInst_, nullptr);
        if (!hAccountPicker_)
        {
            return;
        }

        account_picker_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hAccountPicker_, current_theme_);
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(make_avatar_image_provider_());
        account_picker_->on_select = [this](const std::string& uid)
        {
            if (hAccountPicker_)
            {
                ShowWindow(hAccountPicker_, SW_HIDE);
            }
            on_account_picker_select_(uid);
        };
        account_picker_surface_->set_root(std::move(picker));
        if (HWND s = account_picker_surface_->hwnd())
        {
            SetWindowPos(s, nullptr, 0, 0, kPickerW, kPickerH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Rebuild entries.
    std::vector<tesseract::views::AccountEntry> entries;
    for (const auto& s : account_manager_.accounts())
    {
        entries.push_back({s->user_id, s->display_name, s->avatar_url,
                           s->user_id == my_user_id_});
        if (!s->avatar_url.empty())
        {
            ensure_user_avatar_(s->avatar_url);
        }
    }
    if (account_picker_)
    {
        account_picker_->set_entries(std::move(entries));
        if (account_picker_surface_)
        {
            account_picker_surface_->relayout();
        }
    }
}

void MainWindow::open_account_picker()
{
    if (account_manager_.accounts().size() < 2)
    {
        return;
    }
    rebuild_account_picker();
    if (!hAccountPicker_)
    {
        return;
    }

    const int kPickerW = dip_to_phys(260.f);
    const int kPickerH = dip_to_phys(
        56.f * static_cast<float>(account_manager_.accounts().size()));

    // Anchor to the bottom-left of the main app surface (where the user strip lives).
    RECT sr{};
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        GetWindowRect(main_app_surface_->hwnd(), &sr);
    }
    else if (hwnd_)
    {
        GetWindowRect(hwnd_, &sr);
    }
    int x = sr.left;
    int y = sr.bottom - kPickerH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (x + kPickerW > mi.rcWork.right)
        {
            x = mi.rcWork.right - kPickerW - 4;
        }
        if (y < mi.rcWork.top)
        {
            y = sr.bottom + 4;
        }
    }
    SetWindowPos(hAccountPicker_, HWND_TOPMOST, x, y, kPickerW, kPickerH,
                 SWP_NOACTIVATE);
    ShowWindow(hAccountPicker_, SW_SHOWNOACTIVATE);
}

void MainWindow::show_user_context_menu_(int screen_x, int screen_y)
{
    const auto items = build_user_menu_items_(
        [this] { open_settings_(); },
        [this] { begin_add_account(); },
        [this] { start_qr_grant_overlay(); },
        [this] { logout_active_account(); },
        [this] { quitting_ = true; DestroyWindow(hwnd_); });

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

// ---------------------------------------------------------------------------
// Emoji picker — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::EmojiPicker. The search field is a
// NativeTextField overlay (EDIT child); selection routes through
// insert_emoji_at_cursor below.
// ---------------------------------------------------------------------------

namespace
{
constexpr const wchar_t* kEmojiPickerClass = L"TesseractEmojiPicker";
} // namespace

void MainWindow::ensure_emoji_picker_created()
{
    if (hEmojiPicker_)
    {
        return;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // tk::win32::Surface paints the body
        wc.lpszClassName = kEmojiPickerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    hEmojiPicker_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kEmojiPickerClass, L"", WS_POPUP, 0,
        0, dip_to_phys(kEmojiPickW), dip_to_phys(kEmojiPickH), hwnd_, nullptr,
        hInst_, nullptr);
    if (!hEmojiPicker_)
    {
        return;
    }

    emoji_picker_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hEmojiPicker_, current_theme_);

    auto shared = tk::create_root_widget<tesseract::views::EmojiPicker>(
        &emoji_picker_surface_->host());
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_search_overlay_inset(1.0f);
    emoji_picker_shared_->set_client(client_);
    emoji_picker_shared_->on_selected = [this](const std::string& glyph)
    {
        insert_emoji_at_cursor(glyph);
    };
    emoji_picker_shared_->on_emoticon_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        pick_emoticon_at_cursor(img);
    };
    emoji_picker_shared_->set_image_provider(
        make_picker_image_provider_(false));
    emoji_picker_surface_->set_root(std::move(shared));

    if (HWND s = emoji_picker_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, dip_to_phys(kEmojiPickW),
                     dip_to_phys(kEmojiPickH), SWP_NOZORDER | SWP_NOACTIVATE);
    }

}

void MainWindow::reposition_visible_pickers_(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;
    auto move_if_visible = [&](HWND h)
    {
        if (!h || !IsWindowVisible(h))
            return;
        RECT r{};
        GetWindowRect(h, &r);
        SetWindowPos(h, nullptr, r.left + dx, r.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    move_if_visible(hEmojiPicker_);
    move_if_visible(hStickerPicker_);
}

void MainWindow::toggle_emoji_picker()
{
    ensure_emoji_picker_created();
    if (!hEmojiPicker_)
    {
        return;
    }

    if (IsWindowVisible(hEmojiPicker_))
    {
        ShowWindow(hEmojiPicker_, SW_HIDE);
        if (room_text_area_)
            room_text_area_->set_focused(true);
        return;
    }

    // Anchor the picker above the main app surface (compose bar is at its bottom).
    RECT btn_rc{};
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        GetWindowRect(main_app_surface_->hwnd(), &btn_rc);
    }
    else
    {
        GetWindowRect(hwnd_, &btn_rc);
    }
    const int pickerW = dip_to_phys(kEmojiPickW);
    const int pickerH = dip_to_phys(kEmojiPickH);
    int x = btn_rc.left + 8;
    int y = btn_rc.top - pickerH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y < mi.rcWork.top)
        {
            y = btn_rc.bottom + 4;
        }
    }

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST, x, y, pickerW, pickerH,
                 SWP_NOACTIVATE);

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
        emoji_picker_shared_->set_search_query("");
        if (auto* sf = emoji_picker_shared_->search_field())
        {
            sf->set_text("");
        }
    }

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (emoji_picker_shared_)
    {
        if (auto* sf = emoji_picker_shared_->search_field())
        {
            sf->set_focused(true);
        }
    }
}

// ---------------------------------------------------------------------------
// Slash-command popup — WS_POPUP HWND hosting a tk::win32::Surface that
// paints the shared tesseract::views::SlashCommandPopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    // Widget + controller created eagerly in on_create; this positions the
    // already-populated popup at the caret, clamped to the work area.
    if (!slash_popup_hwnd_ || !slash_popup_surface_ || !main_app_surface_)
    {
        return;
    }
    int w = dip_to_phys(tesseract::views::SlashCommandPopup::kWidth);
    int h = dip_to_phys(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    // cursor_local is already in physical pixels (Win32RichEditArea::cursor_rect
    // uses MapWindowPoints). Do NOT apply dip_to_phys to it.
    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    const int gap = dip_to_phys(4.f);
    int x = pt.x;
    int y_above = pt.y - h - gap;
    int y_below = pt.y + LONG(cursor_local.h) + gap;
    int y = (y_above >= mi.rcWork.top) ? y_above : y_below;
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
    SetWindowPos(slash_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // The Surface is a WS_CHILD; the STATIC popup parent never forwards
    // WM_SIZE, so stretch the child to fill the popup every show (row count
    // changes the height) or clicks on suggestion rows never reach it.
    if (HWND s = slash_popup_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    slash_popup_surface_->relayout();
}

void MainWindow::hide_slash_popup_()
{
    if (slash_popup_hwnd_)
    {
        ShowWindow(slash_popup_hwnd_, SW_HIDE);
    }
}

void MainWindow::show_gif_popup_()
{
    if (!gif_popup_widget_ || !room_text_area_ || !main_app_surface_ ||
        !gif_popup_hwnd_ || !gif_popup_surface_)
    {
        return;
    }
    // Full-width strip spanning the compose bar, floating just above it (like
    // the attachment preview band). content_size() drives only the height and
    // the empty/status check; the width comes from the compose bar.
    const tk::Rect cb = room_view_ ? room_view_->compose_bar_rect() : tk::Rect{};
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    const int w = dip_to_phys(cb.w);
    const int h = dip_to_phys(sz.h);

    HWND parent = main_app_surface_->hwnd();
    POINT pt{dip_to_phys(cb.x), dip_to_phys(cb.y)};
    ClientToScreen(parent, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    const int gap = dip_to_phys(4.f);
    int x = pt.x;                  // align with the compose bar's left edge
    int y = pt.y - h - gap;        // bottom edge just above the compose bar top
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);

    SetWindowPos(gif_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (HWND s = gif_popup_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    gif_popup_surface_->relayout();
}

void MainWindow::hide_gif_popup_()
{
    if (gif_popup_hwnd_)
    {
        ShowWindow(gif_popup_hwnd_, SW_HIDE);
    }
}

const tk::Image*
MainWindow::gif_strip_image_(const tesseract::GifResult& result,
                             const std::function<void()>& repaint)
{
    // Shared with every pop-out's GIF strip (RoomWindowBase::shell_gif_strip_image_
    // → here). The pop-out passes a repaint that refreshes its own popup surface.
    return gif_strip_provider_ ? gif_strip_provider_(result, repaint) : nullptr;
}

void MainWindow::handle_gif_results_ui_(std::uint64_t request_id,
                                        std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void MainWindow::handle_gif_search_failed_ui_(std::uint64_t request_id,
                                              std::string message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, std::move(message));
    }
}

// ---------------------------------------------------------------------------
// Shortcode popup — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::ShortcodePopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    if (!shortcode_popup_hwnd_ || !shortcode_popup_surface_ ||
        !main_app_surface_)
    {
        return;
    }
    int w = dip_to_phys(tesseract::views::ShortcodePopup::kWidth);
    int h = dip_to_phys(rows * tesseract::views::ShortcodePopup::kRowHeight);
    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    const int gap = dip_to_phys(4.f);
    int x = pt.x;
    int y_above = pt.y - h - gap;
    int y_below = pt.y + LONG(cursor_local.h) + gap;
    int y = (y_above >= mi.rcWork.top) ? y_above : y_below;
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
    SetWindowPos(shortcode_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // WS_CHILD surface: stretch to fill every show (see show_slash_popup_).
    if (HWND s = shortcode_popup_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    shortcode_popup_surface_->relayout();
}

void MainWindow::hide_shortcode_popup_()
{
    if (shortcode_popup_hwnd_)
    {
        ShowWindow(shortcode_popup_hwnd_, SW_HIDE);
    }
}

// ── @mention popup ─────────────────────────────────────────────────────────

void MainWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_hwnd_ || !main_app_surface_)
    {
        return;
    }
    int w = dip_to_phys(tesseract::views::MentionPopup::kWidth);
    int h = dip_to_phys(rows * tesseract::views::MentionPopup::kRowHeight);

    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    const int gap = dip_to_phys(4.f);
    int x = pt.x;
    int y_above = pt.y - h - gap;
    int y_below = pt.y + LONG(cursor_local.h) + gap;
    int y = (y_above >= mi.rcWork.top) ? y_above : y_below;
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);

    SetWindowPos(mention_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (HWND s = mention_popup_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    mention_popup_surface_->relayout();
}

void MainWindow::hide_mention_popup_()
{
    if (mention_popup_hwnd_)
    {
        ShowWindow(mention_popup_hwnd_, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::popup_emoji_at_rect(HWND parent_hwnd, tk::Rect local_rect)
{
    ensure_emoji_picker_created();
    if (!hEmojiPicker_ || !parent_hwnd)
    {
        return;
    }

    // Map the local rect into screen coordinates.
    POINT pt{dip_to_phys(local_rect.x), dip_to_phys(local_rect.y)};
    ClientToScreen(parent_hwnd, &pt);
    LONG rectW = dip_to_phys(local_rect.w);
    LONG rectH = dip_to_phys(local_rect.h);

    // Prefer above, centered on the rect; fall back to below if the
    // monitor doesn't have room. Clamp to the work area horizontally.
    const int pickerW = dip_to_phys(kEmojiPickW);
    const int pickerH = dip_to_phys(kEmojiPickH);
    int x = pt.x + rectW / 2 - pickerW / 2;
    int y = pt.y - pickerH - 4;
    POINT ptCenter{pt.x + rectW / 2, pt.y + rectH / 2};
    HMONITOR mon = MonitorFromPoint(ptCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (y < mi.rcWork.top)
        {
            y = pt.y + rectH + 4;
        }
        if (x + pickerW > mi.rcWork.right)
        {
            x = mi.rcWork.right - pickerW - 4;
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y + pickerH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - pickerH - 4;
        }
    }
    (void)rectW;

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST, x, y, pickerW, pickerH,
                 SWP_NOACTIVATE);

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
        emoji_picker_shared_->set_search_query("");
        if (auto* sf = emoji_picker_shared_->search_field())
        {
            sf->set_text("");
        }
    }

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (emoji_picker_shared_)
    {
        if (auto* sf = emoji_picker_shared_->search_field())
        {
            sf->set_focused(true);
        }
    }
}

void MainWindow::popup_sticker_at_rect(HWND parent_hwnd, tk::Rect local_rect)
{
    ensure_sticker_picker_created();
    if (!hStickerPicker_ || !parent_hwnd)
    {
        return;
    }

    POINT pt{dip_to_phys(local_rect.x), dip_to_phys(local_rect.y)};
    ClientToScreen(parent_hwnd, &pt);
    LONG rectW = dip_to_phys(local_rect.w);
    LONG rectH = dip_to_phys(local_rect.h);

    // Prefer above, centered on the rect; fall back to below if the
    // monitor doesn't have room. Clamp to the work area horizontally.
    const int pickerW = dip_to_phys(kStickerPickW);
    const int pickerH = dip_to_phys(kStickerPickH);
    int x = pt.x + rectW / 2 - pickerW / 2;
    int y = pt.y - pickerH - 4;
    POINT ptCenter{pt.x + rectW / 2, pt.y + rectH / 2};
    HMONITOR mon = MonitorFromPoint(ptCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (y < mi.rcWork.top)
        {
            y = pt.y + rectH + 4;
        }
        if (x + pickerW > mi.rcWork.right)
        {
            x = mi.rcWork.right - pickerW - 4;
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y + pickerH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - pickerH - 4;
        }
    }
    (void)rectW;

    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
        sticker_picker_shared_->set_search_query("");
        if (auto* sf = sticker_picker_shared_->search_field())
        {
            sf->set_text("");
        }
    }

    SetWindowPos(hStickerPicker_, HWND_TOPMOST, x, y, pickerW,
                 pickerH, SWP_NOACTIVATE);
    ShowWindow(hStickerPicker_, SW_SHOWNOACTIVATE);
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        if (auto* sf = sticker_picker_shared_->search_field())
        {
            sf->set_focused(true);
        }
    }
}

void MainWindow::insert_emoji_at_cursor(const std::string& glyph)
{
    if (glyph.empty())
    {
        return;
    }
    // Reaction mode: a "+" chip set pending_reaction_event_id_ before
    // opening the picker. Toggle the reaction (Rust handles add/remove)
    // and skip the compose insert.
    if (!pending_reaction_event_id_.empty())
    {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty())
        {
            client_->send_reaction(current_room_id_, ev, glyph);
        }
        if (hEmojiPicker_)
        {
            ShowWindow(hEmojiPicker_, SW_HIDE);
        }
        if (room_text_area_)
            room_text_area_->set_focused(true);
        return;
    }
    if (!room_text_area_)
    {
        return;
    }
    room_text_area_->insert_at_cursor(glyph);
    if (room_view_)
    {
        room_view_->set_current_text(room_text_area_->text());
    }
    room_text_area_->set_focused(true);
    // The shared picker already called recent_emoji_bump before invoking
    // this callback — no need to re-bump here.
}

void MainWindow::pick_emoticon_at_cursor(const tesseract::ImagePackImage& img)
{
    if (img.url.empty())
    {
        return;
    }
    // Reaction mode (parallel to insert_emoji_at_cursor): send an MSC4027
    // custom-image reaction with the mxc key + `:shortcode:`, hide the
    // picker, skip the compose insert.
    if (!pending_reaction_event_id_.empty())
    {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty())
        {
            client_->send_reaction_custom(current_room_id_, ev, img.url,
                                          ":" + img.shortcode + ":");
        }
        if (hEmojiPicker_)
        {
            ShowWindow(hEmojiPicker_, SW_HIDE);
        }
        if (room_text_area_)
            room_text_area_->set_focused(true);
        return;
    }
    // Compose mode. Windows' insert_emoticon renders a real inline image,
    // resolved asynchronously by uri via set_image_resolver — no bitmap to
    // pass here.
    if (!room_text_area_)
    {
        return;
    }
    int pos = room_text_area_->cursor_byte_pos();
    room_text_area_->insert_emoticon(pos, pos, img.shortcode, img.url, nullptr);
    if (room_view_)
    {
        room_view_->set_current_text(room_text_area_->text());
    }
    room_text_area_->set_focused(true);
}

// ---------------------------------------------------------------------------
// Sticker picker — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::StickerPicker. Mirrors the emoji picker.
// Selection routes through Client::send_sticker. The image_provider reuses
// the per-window tk_images_ cache populated by ensure_media_image; cells
// for entries the cache hasn't seen yet render a placeholder shimmer.
// ---------------------------------------------------------------------------

namespace
{
constexpr const wchar_t* kStickerPickerClass = L"TesseractStickerPicker";
} // namespace

void MainWindow::ensure_sticker_picker_created()
{
    if (hStickerPicker_)
    {
        return;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // tk::win32::Surface paints the body
        wc.lpszClassName = kStickerPickerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    hStickerPicker_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kStickerPickerClass, L"", WS_POPUP, 0,
        0, dip_to_phys(kStickerPickW), dip_to_phys(kStickerPickH), hwnd_,
        nullptr, hInst_, nullptr);
    if (!hStickerPicker_)
    {
        return;
    }

    sticker_picker_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hStickerPicker_, current_theme_);

    auto shared = tk::create_root_widget<tesseract::views::StickerPicker>(
        &sticker_picker_surface_->host());
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_search_overlay_inset(1.0f);
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (!current_room_id_.empty())
        {
            const std::string body =
                img.body.empty() ? img.shortcode : img.body;
            send_sticker_(body, img.url, img.info_json);
        }
        if (hStickerPicker_)
        {
            ShowWindow(hStickerPicker_, SW_HIDE);
        }
        if (room_text_area_)
            room_text_area_->set_focused(true);
    };
    // Image provider: synchronous best-effort lookup against the
    // animated + static caches populated by message-list rendering. On
    // miss, kick off an async decode through the shared ShellBase
    // image-cache path (worker-thread WIC decode → finalize_picker_image_
    // → repaint_pickers_) so the picker fills in stickers that haven't
    // appeared in any message yet.
    sticker_picker_shared_->set_image_provider(
        make_picker_image_provider_(true));
    sticker_picker_surface_->set_root(std::move(shared));

    if (HWND s = sticker_picker_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, dip_to_phys(kStickerPickW),
                     dip_to_phys(kStickerPickH), SWP_NOZORDER | SWP_NOACTIVATE);
    }

}

void MainWindow::toggle_sticker_picker()
{
    ensure_sticker_picker_created();
    if (!hStickerPicker_)
    {
        return;
    }

    if (IsWindowVisible(hStickerPicker_))
    {
        ShowWindow(hStickerPicker_, SW_HIDE);
        if (room_text_area_)
            room_text_area_->set_focused(true);
        return;
    }

    // Anchor above the main app surface (compose bar is at its bottom).
    RECT btn_rc{};
    if (main_app_surface_ && main_app_surface_->hwnd())
    {
        GetWindowRect(main_app_surface_->hwnd(), &btn_rc);
    }
    else
    {
        GetWindowRect(hwnd_, &btn_rc);
    }
    const int pickerW = dip_to_phys(kStickerPickW);
    const int pickerH = dip_to_phys(kStickerPickH);
    int x = btn_rc.right - pickerW - 8;
    int y = btn_rc.top - pickerH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (x + pickerW > mi.rcWork.right)
        {
            x = mi.rcWork.right - pickerW - 4;
        }
        if (y < mi.rcWork.top)
        {
            y = btn_rc.bottom + 4;
        }
        if (y + pickerH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - pickerH - 4;
        }
    }

    SetWindowPos(hStickerPicker_, HWND_TOPMOST, x, y, pickerW,
                 pickerH, SWP_NOACTIVATE);

    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
        sticker_picker_shared_->set_search_query("");
        if (auto* sf = sticker_picker_shared_->search_field())
        {
            sf->set_text("");
        }
    }

    ShowWindow(hStickerPicker_, SW_SHOWNOACTIVATE);
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        if (auto* sf = sticker_picker_shared_->search_field())
        {
            sf->set_focused(true);
        }
    }
}

void MainWindow::refresh_sticker_picker()
{
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_current_room_id(current_room_id_);
        sticker_picker_shared_->set_current_room_parent_spaces(
            parent_spaces_for_room_(current_room_id_));
        sticker_picker_shared_->refresh_packs();
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------
// Join room dialog — centred WS_POPUP hosting JoinRoomView.
// ---------------------------------------------------------------------------

namespace
{
constexpr const wchar_t* kJoinRoomClass = L"TesseractJoinRoom";
} // namespace

void MainWindow::ensure_join_room_created()
{
    if (hJoinRoom_)
    {
        return;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kJoinRoomClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    hJoinRoom_ =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kJoinRoomClass, L"",
                        WS_POPUP | WS_BORDER, 0, 0, kJoinRoomPickW,
                        kJoinRoomPickH, hwnd_, nullptr, hInst_, nullptr);
    if (!hJoinRoom_)
    {
        return;
    }

    join_room_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hJoinRoom_, tk::Theme::light());

    auto jrv = tk::create_root_widget<tesseract::views::JoinRoomView>(&join_room_surface_->host());
    join_room_shared_ = jrv.get();

    join_room_shared_->set_avatar_provider(make_avatar_image_provider_());

    join_room_shared_->on_lookup_requested = [this](const std::string& alias)
    {
        if (!client_ || alias.empty())
        {
            return;
        }
        if (join_room_shared_)
        {
            join_room_shared_->set_state(
                tesseract::views::JoinRoomView::State::Loading);
        }
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        HWND target = hwnd_;
        uint32_t gen = join_room_gen_;
        run_async_(
            [this, alias, target, gen, snap = client_]
            {
                auto* s =
                    new tesseract::RoomSummary(snap->get_room_summary(alias));
                if (!PostMessageW(target, WM_TESSERACT_JOIN_ROOM_LOOKUP_DONE,
                                  static_cast<WPARAM>(gen),
                                  reinterpret_cast<LPARAM>(s)))
                {
                    delete s;
                }
            });
    };

    join_room_shared_->on_join_requested =
        [this](const std::string& room_id_or_alias)
    {
        if (!client_ || room_id_or_alias.empty())
        {
            return;
        }
        if (join_room_shared_)
        {
            join_room_shared_->set_state(
                tesseract::views::JoinRoomView::State::Joining);
        }
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        HWND target = hwnd_;
        uint32_t gen = join_room_gen_;
        run_async_mut_(
            [this, room_id_or_alias, target, gen, snap = client_]
            {
                auto* rid = new std::string(snap->join_room(room_id_or_alias));
                if (!PostMessageW(target, WM_TESSERACT_JOIN_ROOM_DONE,
                                  static_cast<WPARAM>(gen),
                                  reinterpret_cast<LPARAM>(rid)))
                {
                    delete rid;
                }
            });
    };

    join_room_shared_->on_cancel = [this]
    {
        if (hJoinRoom_)
        {
            ShowWindow(hJoinRoom_, SW_HIDE);
        }
    };

    join_room_shared_->on_link_clicked = [this](std::string url)
    {
        if (tesseract::Client::parse_matrix_link(url).kind !=
            tesseract::Client::MatrixLink::Kind::Unknown)
            open_matrix_link(url);
        else
            tesseract::Client::open_in_browser(url);
    };
    join_room_shared_->on_link_hovered = [this](std::string url)
    {
        if (join_room_surface_)
            join_room_surface_->set_cursor(url.empty() ? tk::win32::Cursor::Default
                                                       : tk::win32::Cursor::Pointer);
    };

    join_room_surface_->set_root(std::move(jrv));

    if (HWND s = join_room_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, kJoinRoomPickW, kJoinRoomPickH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

}

void MainWindow::open_join_room_dialog()
{
    ensure_join_room_created();
    if (!hJoinRoom_)
    {
        return;
    }

    if (IsWindowVisible(hJoinRoom_))
    {
        ShowWindow(hJoinRoom_, SW_HIDE);
        return;
    }

    // Bump generation to invalidate any in-flight lookup/join callbacks.
    ++join_room_gen_;

    // Reset to Idle state.
    if (join_room_shared_)
    {
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Idle);
        join_room_shared_->set_alias_text("");
    }

    // Centre over the main window.
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int cx = (rc.left + rc.right) / 2 - kJoinRoomPickW / 2;
    int cy = (rc.top + rc.bottom) / 2 - kJoinRoomPickH / 2;
    SetWindowPos(hJoinRoom_, HWND_TOPMOST, cx, cy, kJoinRoomPickW,
                 kJoinRoomPickH, 0);

    ShowWindow(hJoinRoom_, SW_SHOW);
    if (join_room_surface_)
    {
        join_room_surface_->relayout();
    }
    if (join_room_shared_)
    {
        join_room_shared_->focus_alias_field();
    }
}

void MainWindow::refresh_emoji_picker()
{
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_current_room_id(current_room_id_);
        emoji_picker_shared_->set_current_room_parent_spaces(
            parent_spaces_for_room_(current_room_id_));
        emoji_picker_shared_->refresh_emoticon_packs();
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    if (!main_app_)
    {
        return;
    }

    auto* tb = main_app_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    main_app_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string name;
            if (const auto* r = room_by_id_(t.room_id))
            {
                name = r->name;
                const std::string& av_mxc = r->effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = account_manager_.thumbnail_cache().peek(av_mxc);
                }
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    if (active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        on_room_selected(active.room_id);
        if (!active.compose_draft.empty())
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(active.compose_draft);
            }
            if (room_view_)
            {
                room_view_->set_current_text(active.compose_draft);
            }
        }
    }

    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    if (room_text_area_ && room_text_area_->visible())
    {
        room_text_area_->set_focused(true);
    }
}

float MainWindow::get_message_scroll_fraction_()
{
    if (!room_view_ || !room_view_->message_list())
    {
        return 0.f;
    }
    return room_view_->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t)
{
    if (!room_view_ || !room_view_->message_list())
    {
        return;
    }
    room_view_->message_list()->scroll_to_offset(t);
}

std::string MainWindow::get_compose_draft_()
{
    if (!room_view_ || !room_view_->compose_bar())
    {
        return {};
    }
    return room_view_->compose_bar()->current_text();
}

void MainWindow::set_compose_draft_(const std::string& draft)
{
    if (room_text_area_)
    {
        room_text_area_->set_text(draft);
    }
    if (room_view_)
    {
        room_view_->set_current_text(draft);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::wire_key_dialog_callbacks_()
{
    settings_controller_->show_passphrase_prompt =
        [this](std::string title, std::function<void(std::string)> cb)
    {
        std::wstring pass =
            prompt_passphrase_w32(hwnd_, utf8_to_wstr(title).c_str());
        if (!pass.empty())
            cb(wstr_to_utf8(pass.c_str()));
    };

    settings_controller_->show_save_file_dialog =
        [this](std::string suggested_name, std::function<void(std::string)> cb)
    {
        std::wstring path = show_save_dialog_(
            utf8_to_wstr(suggested_name),
            L"Key files\0*.txt\0All files\0*.*\0\0");
        if (!path.empty())
            cb(wstr_to_utf8(path.c_str()));
    };

    settings_controller_->show_open_file_dialog =
        [this](std::function<void(std::string)> cb)
    {
        wchar_t buf[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd_;
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"Key files\0*.txt\0All files\0*.*\0\0";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
            cb(wstr_to_utf8(buf));
    };

    settings_controller_->on_export_keys_result =
        [this](bool ok, std::string error)
    {
        if (ok)
            MessageBoxW(hwnd_, L"Room keys exported successfully.",
                        L"Export complete", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hwnd_, utf8_to_wstr(error).c_str(),
                        L"Export failed", MB_OK | MB_ICONWARNING);
    };

    settings_controller_->on_import_keys_result =
        [this](bool ok, std::string error)
    {
        if (ok)
            MessageBoxW(hwnd_, L"Room keys imported successfully.",
                        L"Import complete", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hwnd_, utf8_to_wstr(error).c_str(),
                        L"Import failed", MB_OK | MB_ICONWARNING);
    };
}

// ─────────────────────────────────────────────────────────────────────────────

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

// ---------------------------------------------------------------------------
// EncryptionSetupOverlay — Win32 wiring
// ---------------------------------------------------------------------------

void MainWindow::open_join_room_dialog_ui_(const std::string& prefill)
{
    open_join_room_dialog();
    if (!prefill.empty() && join_room_shared_)
    {
        join_room_shared_->set_alias_text(prefill);
    }
}

void MainWindow::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    if (!main_app_)
        return;
    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // Reconfigure the overlay (clears prior callbacks + field text) before
    // wiring the shared callbacks via ShellBase.
    ov->reset(mode);

    wire_encryption_setup_callbacks_(*ov, main_app_surface_->host());

    main_app_->show_encryption_setup(true);
    if (main_app_surface_)
        main_app_surface_->relayout();
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

void MainWindow::switch_active_account_(const std::string& user_id)
{
    switch_active_account(user_id);
}

void MainWindow::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    auto* win = new win32::MainWindow(account_manager_, hInst_);
    win->set_initial_account(account);
    // Shared hand-off: re-point bridge at the new window, seed caches, pin, and
    // register dedicated — before the new window's deferred doLogin().
    hand_account_to_spawned_window_(win, account);
    win->raise_and_activate_();
}

} // namespace win32
