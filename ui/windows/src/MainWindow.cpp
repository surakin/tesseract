#include "MainWindow.h"
#include "LoginView.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"

#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <string>

namespace {

std::wstring utf8_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string wstr_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

Gdiplus::Bitmap* bitmap_from_bytes(const std::vector<uint8_t>& data) {
    if (data.empty()) return nullptr;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hg) return nullptr;
    void* p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return nullptr; }
    memcpy(p, data.data(), data.size());
    GlobalUnlock(hg);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stream))) {
        GlobalFree(hg);
        return nullptr;
    }
    auto* bmp = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

} // namespace

namespace win32 {

// ---------------------------------------------------------------------------
// Posted-payload types — shared between worker threads and the UI wnd_proc.
// Lives near the top of the TU so wnd_proc's WM_* cases can reinterpret_cast
// from LPARAM without forward declarations.
// ---------------------------------------------------------------------------

namespace {
struct StickerBytesPayload {
    std::string                 cache_key;
    std::vector<std::uint8_t>   bytes;
};
struct MediaBytesPayload {
    win32::MainWindow::MediaKind kind;
    std::string                  cache_key;   // mxc (room/user avatar) or url
    std::vector<std::uint8_t>    bytes;
};
} // namespace

// ---------------------------------------------------------------------------
// Room header WndProc
// ---------------------------------------------------------------------------

LRESULT CALLBACK room_header_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // Painted in WM_PAINT.
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Double-buffer through a memory DC. on_rooms_updated fires on every
        // sync delta (one per incoming event across any room), so without
        // a back-buffer the user sees the chrome_bg fill flash before the
        // avatar/name/topic redraw — a strobe-like flicker that visually
        // looks like the banner is overlapping the message list below.
        HDC     mem_dc  = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

        Gdiplus::Graphics g(mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const auto& pal = theme::palette();

        // Background
        Gdiplus::SolidBrush bg(theme::gpc(pal.chrome_bg));
        g.FillRectangle(&bg, 0, 0, w, h);

        // Bottom border
        Gdiplus::Pen border(theme::gpc(pal.separator), 1.0f);
        g.DrawLine(&border, 0.0f, (float)(h - 1), (float)w, (float)(h - 1));

        const auto& info = self->current_room_info_;
        if (!info.id.empty()) {
            // Avatar
            Gdiplus::Bitmap* bmp = self->get_room_avatar(info.id);
            int ax = 16;
            int ay = (h - MainWindow::kRoomAvatarSize) / 2;
            if (bmp)
                self->draw_circle_bitmap(g, bmp, ax, ay, MainWindow::kRoomAvatarSize);
            else
                self->draw_initials_circle(g, info.name, ax, ay, MainWindow::kRoomAvatarSize);

            // Name + topic — DirectWrite/D2D so emoji in room names/topics
            // render in color via Segoe UI Emoji's COLR layers.
            int tx     = ax + MainWindow::kRoomAvatarSize + 12;
            int text_w = w - tx - 16;

            auto wname = utf8_to_wstr(info.name);
            RECT name_rc{ tx, 12, tx + text_w, 12 + 22 };
            win32::text::draw(mem_dc, name_rc, wname.c_str(), -1,
                win32::text::Style{
                    .family = L"Segoe UI",
                    .size   = 13.5f,
                    .weight = win32::text::Weight::Bold,
                    .color  = pal.text_primary,
                    .trim   = win32::text::Trim::EllipsisChar,
                });

            if (!info.topic.empty()) {
                auto wtopic = utf8_to_wstr(info.topic);
                RECT topic_rc{ tx, 34, tx + text_w, 34 + 18 };
                win32::text::draw(mem_dc, topic_rc, wtopic.c_str(), -1,
                    win32::text::Style{
                        .family = L"Segoe UI",
                        .size   = 10.0f,
                        .color  = pal.text_secondary,
                        .trim   = win32::text::Trim::EllipsisChar,
                    });
            }
        }

        BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);

        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// User-strip WndProc (sidebar footer with avatar + display name + right-click)
// ---------------------------------------------------------------------------

LRESULT CALLBACK user_strip_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const auto& pal = theme::palette();

        RECT rc;
        GetClientRect(hwnd, &rc);
        int x0 = rc.left, y0 = rc.top;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Background
        Gdiplus::SolidBrush bg(theme::gpc(pal.sidebar_bg));
        g.FillRectangle(&bg, x0, y0, w, h);

        // Top border
        Gdiplus::Pen border(theme::gpc(pal.separator), 1.0f);
        g.DrawLine(&border, (float)x0, (float)y0, (float)rc.right, (float)y0);

        const std::string& shown = self->my_display_name_.empty()
            ? self->my_user_id_
            : self->my_display_name_;
        if (shown.empty()) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        constexpr int AVATAR = 32;
        int ax = x0 + 8;
        int ay = y0 + (h - AVATAR) / 2;
        if (self->user_avatar_bmp_)
            self->draw_circle_bitmap(g, self->user_avatar_bmp_, ax, ay, AVATAR);
        else
            self->draw_initials_circle(g, shown, ax, ay, AVATAR);

        int tx = ax + AVATAR + 10;
        int text_w = rc.right - tx - 8;
        auto wname = utf8_to_wstr(shown);
        RECT name_rc{ tx, y0, tx + text_w, y0 + h };
        win32::text::draw(hdc, name_rc, wname.c_str(), -1,
            win32::text::Style{
                .family = L"Segoe UI",
                .size   = 10.5f,
                .weight = win32::text::Weight::Bold,
                .color  = pal.text_primary,
                .valign = win32::text::VAlign::Center,
                .trim   = win32::text::Trim::EllipsisChar,
            });

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MainWindow::IDM_LOGOUT, L"Logout");
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        // For keyboard-triggered context menus (-1,-1) anchor to widget centre.
        if (x == -1 && y == -1) {
            RECT rc; GetWindowRect(hwnd, &rc);
            x = rc.left + (rc.right - rc.left) / 2;
            y = rc.top  + (rc.bottom - rc.top) / 2;
        }
        UINT pick = TrackPopupMenu(menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD,
            x, y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (pick == static_cast<UINT>(MainWindow::IDM_LOGOUT)) {
            // Forward to the parent so MainWindow::wnd_proc's WM_COMMAND
            // handler runs do_logout(). HIWORD=0 mimics a menu accelerator.
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(MainWindow::IDM_LOGOUT, 0), 0);
        }
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Status-bar WndProc — flat custom-painted strip replacing the comctl32
// STATUSCLASSNAMEW (which carries a 9x-style size grip and chunky borders).
// Stores the latest text via WM_SETTEXT / SB_SETTEXTW so existing callers
// using either message continue to work.
// ---------------------------------------------------------------------------

namespace {
constexpr UINT_PTR kStatusTextProp = 0xDEADBEEFu;
}

LRESULT CALLBACK status_bar_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE:
        SetPropW(hwnd, L"TesseractStatusText", nullptr);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NCDESTROY: {
        auto* p = static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        delete p;
        RemovePropW(hwnd, L"TesseractStatusText");
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_SETTEXT:
    case SB_SETTEXTW:
    case SB_SETTEXTA: {
        // Accept both SetWindowText and the comctl SB_SETTEXT messages so
        // existing callers ported from STATUSCLASSNAMEW continue to work.
        const wchar_t* txt = reinterpret_cast<const wchar_t*>(lParam);
        auto* p = static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (!p) { p = new std::wstring; SetPropW(hwnd, L"TesseractStatusText", p); }
        p->assign(txt ? txt : L"");
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        const auto& pal = theme::palette();
        RECT rc; GetClientRect(hwnd, &rc);

        // Solid background + 1px top separator.
        HBRUSH bg = theme::brush(pal.chrome_bg);
        FillRect(hdc, &rc, bg);
        RECT top = { rc.left, rc.top, rc.right, rc.top + 1 };
        FillRect(hdc, &top, theme::brush(pal.separator));

        auto* p = static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (p && !p->empty()) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, pal.text_secondary);
            HFONT old = (HFONT)SelectObject(hdc, theme::font(theme::FontRole::Small));
            RECT text_rc = { rc.left + 10, rc.top, rc.right - 10, rc.bottom };
            DrawTextW(hdc, p->c_str(), -1, &text_rc,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void register_status_bar_class(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = status_bar_wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TesseractStatusBar";
    RegisterClassExW(&wc);
    registered = true;
}

void MainWindow::apply_default_font(HWND h) {
    if (h) SendMessageW(h, WM_SETFONT,
                       reinterpret_cast<WPARAM>(theme::font(theme::FontRole::Ui)),
                       TRUE);
}

void MainWindow::on_system_theme_changed() {
    if (!theme::refresh_from_system()) return;
    theme::apply_window_attributes(hwnd_);
    // The room / message / compose lists are tk Surfaces; they paint
    // themselves on theme change via relayout.
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (room_surface_)    room_surface_->relayout();
    if (msg_surface_)     msg_surface_->relayout();
    if (compose_surface_) compose_surface_->relayout();
    if (hRoomHeader_)  InvalidateRect(hRoomHeader_, nullptr, TRUE);
    if (hUserStrip_)   InvalidateRect(hUserStrip_,  nullptr, TRUE);
    if (hStatus_)     InvalidateRect(hStatus_,     nullptr, TRUE);
}

void MainWindow::paint_main_background(HDC hdc, const RECT& rc) {
    const auto& pal = theme::palette();
    FillRect(hdc, &rc, theme::brush(pal.window_bg));
}

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_timeline_reset(
    const std::string& room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    auto* p = new MainWindow::PostedTimelineReset{ room_id, std::move(snapshot) };
    PostMessage(hwnd_, WM_TESSERACT_TIMELINE_RESET, 0,
                reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_message_inserted(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    auto* p = new MainWindow::PostedMessageEvent{
        room_id, index, std::move(ev),
    };
    PostMessage(hwnd_, WM_TESSERACT_MESSAGE_INSERTED, 0,
                reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_message_updated(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    auto* p = new MainWindow::PostedMessageEvent{
        room_id, index, std::move(ev),
    };
    PostMessage(hwnd_, WM_TESSERACT_MESSAGE_UPDATED, 0,
                reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_message_removed(
    const std::string& room_id,
    std::size_t index)
{
    auto* p = new MainWindow::PostedMessageEvent{
        room_id, index, nullptr,
    };
    PostMessage(hwnd_, WM_TESSERACT_MESSAGE_REMOVED, 0,
                reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) {
    auto* p = new std::vector<tesseract::RoomInfo>(rooms);
    PostMessage(hwnd_, WM_TESSERACT_ROOMS, 0, reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_sync_error(const std::string& context,
                                   const std::string& description,
                                   bool soft_logout)
{
    if (context == "sync_reconnect") {
        PostMessage(hwnd_, WM_TESSERACT_RECONNECT, 0, 0);
    } else if (context == "sync_auth_error") {
        PostMessage(hwnd_, WM_TESSERACT_AUTH_ERROR,
                    static_cast<WPARAM>(soft_logout), 0);
    } else {
        auto* p = new std::string(description);
        PostMessage(hwnd_, WM_TESSERACT_SYNC_ERROR, 0, reinterpret_cast<LPARAM>(p));
    }
}

void EventHandler::on_session_saved(const std::string& session_json) {
    tesseract::SessionStore::save(session_json);
}

void EventHandler::on_backup_progress(const tesseract::BackupProgress& progress) {
    auto* p = new tesseract::BackupProgress(progress);
    PostMessage(hwnd_, WM_TESSERACT_BACKUP_PROGRESS, 0,
                reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_image_packs_updated() {
    PostMessage(hwnd_, WM_TESSERACT_IMAGE_PACKS, 0, 0);
}

void EventHandler::on_account_prefs_updated(const std::string& json) {
    auto* s = new std::string(json);
    PostMessage(hwnd_, WM_TESSERACT_ACCOUNT_PREFS, 0,
                reinterpret_cast<LPARAM>(s));
}

// ---------------------------------------------------------------------------
// GDI+ helpers
// ---------------------------------------------------------------------------

void MainWindow::fill_rounded_rect(Gdiplus::Graphics& g, Gdiplus::Brush& brush,
                                     float x, float y, float w, float h, float r) {
    Gdiplus::GraphicsPath path;
    path.AddArc(x,         y,         r*2, r*2, 180, 90);
    path.AddArc(x+w-r*2,   y,         r*2, r*2, 270, 90);
    path.AddArc(x+w-r*2,   y+h-r*2,   r*2, r*2,   0, 90);
    path.AddArc(x,         y+h-r*2,   r*2, r*2,  90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

void MainWindow::draw_circle_bitmap(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp,
                                      int x, int y, int size) {
    Gdiplus::GraphicsPath clip;
    clip.AddEllipse(x, y, size, size);
    Gdiplus::Region region(&clip);
    g.SetClip(&region);
    Gdiplus::Rect dst(x, y, size, size);
    g.DrawImage(bmp, dst);
    g.ResetClip();
}

void MainWindow::draw_initials_circle(Gdiplus::Graphics& g, const std::string& name,
                                        int x, int y, int size) {
    static const Gdiplus::ARGB kColors[] = {
        0xFF5B6ABF, 0xFF3A9BD5, 0xFF2ECC71,
        0xFFE74C3C, 0xFF9B59B6, 0xFF1ABC9C,
    };
    int ci = name.empty() ? 0 : (unsigned char)name[0] % 6;
    Gdiplus::SolidBrush bg(kColors[ci]);
    g.FillEllipse(&bg, x, y, size, size);

    std::wstring wn = utf8_to_wstr(name);
    wchar_t init[2] = { wn.empty() ? L'?' : static_cast<wchar_t>(towupper(wn[0])), L'\0' };

    // GDI+ Graphics holds the DC; flush before handing the DC to D2D so
    // any pending circle-fill operations are present when text draws on top.
    g.Flush(Gdiplus::FlushIntentionSync);
    HDC hdc = g.GetHDC();
    RECT rc{ x, y, x + size, y + size };
    win32::text::draw(hdc, rc, init, 1,
        win32::text::Style{
            .family = L"Segoe UI",
            .size   = (float)size * 0.38f,
            .unit   = win32::text::SizeUnit::Pixel,
            .weight = win32::text::Weight::Bold,
            .color  = RGB(0xFF, 0xFF, 0xFF),
            .halign = win32::text::HAlign::Center,
            .valign = win32::text::VAlign::Center,
        });
    g.ReleaseHDC(hdc);
}

Gdiplus::Bitmap* MainWindow::get_room_avatar(const std::string& room_id) {
    auto it = avatar_cache_.find(room_id);
    if (it != avatar_cache_.end()) return it->second;
    auto bytes = client_.fetch_avatar_bytes(room_id);
    Gdiplus::Bitmap* bmp = bitmap_from_bytes(bytes);
    avatar_cache_[room_id] = bmp;
    return bmp;
}

Gdiplus::Bitmap* MainWindow::get_user_avatar(const std::string& mxc_url) {
    if (mxc_url.empty()) return nullptr;
    auto it = user_avatar_cache_.find(mxc_url);
    if (it != user_avatar_cache_.end()) return it->second;
    auto bytes = client_.fetch_media_bytes(mxc_url);
    Gdiplus::Bitmap* bmp = bitmap_from_bytes(bytes);
    user_avatar_cache_[mxc_url] = bmp;
    return bmp;
}

// ---------------------------------------------------------------------------
// wnd_proc
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE:
        self->on_create(hwnd);
        return 0;

    case WM_DESTROY:
        self->on_destroy();
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        self->on_size(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        // Compose bar Send / Emoji + recovery banner clicks go through
        // the shared widgets' callbacks now — no WM_COMMAND wiring. The
        // emoji-picker search field is a NativeTextField overlay handled
        // by its set_on_changed lambda. Only the logout menu and space
        // navigation remain.
        if (LOWORD(wParam) == IDC_SPACE_BACK)
            self->on_space_back();
        if (LOWORD(wParam) == IDM_LOGOUT)
            self->do_logout();
        return 0;

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID == IDC_SIDE_SEPARATOR) {
            FillRect(dis->hDC, &dis->rcItem,
                     theme::brush(theme::palette().separator));
        }
        else if (dis->CtlType == ODT_BUTTON) {
            theme::draw_button(dis);
        }
        return TRUE;
    }

    case WM_ERASEBKGND: {
        // The parent paints behind any pixel a child doesn't cover. Use the
        // themed window background — also keeps Mica fade-in clean during
        // resize, since the OS lerps from our colour to the new layout.
        RECT rc; GetClientRect(hwnd, &rc);
        self->paint_main_background(reinterpret_cast<HDC>(wParam), rc);
        return 1;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        // Theme any flat children that don't owner-draw themselves: status
        // colours on the EDIT controls + recovery STATIC labels.
        const auto& pal = theme::palette();
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, pal.text_primary);
        // The recovery banner is now a tk::win32::Surface — it paints
        // its own background; no WM_CTLCOLOR tinting needed.
        // EDIT controls (compose) → compose-card bg.
        if (msg == WM_CTLCOLOREDIT) {
            SetBkColor(dc, pal.compose_card_bg);
            return reinterpret_cast<LRESULT>(theme::brush(pal.compose_card_bg));
        }
        SetBkColor(dc, pal.window_bg);
        return reinterpret_cast<LRESULT>(theme::brush(pal.window_bg));
    }

    case WM_SETTINGCHANGE: {
        // Watch for OS dark/light flip. lParam carries the changed area name
        // as a wide string when it originated in Personalize.
        if (lParam) {
            auto* name = reinterpret_cast<const wchar_t*>(lParam);
            if (name && wcscmp(name, L"ImmersiveColorSet") == 0) {
                self->on_system_theme_changed();
            }
        }
        return 0;
    }

    case WM_DPICHANGED:
        win32::text::on_dpi_changed(LOWORD(wParam));
        return 0;

    case WM_TESSERACT_MESSAGE_INSERTED: {
        auto* p = reinterpret_cast<MainWindow::PostedMessageEvent*>(lParam);
        self->on_tesseract_message_inserted(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_MESSAGE_UPDATED: {
        auto* p = reinterpret_cast<MainWindow::PostedMessageEvent*>(lParam);
        self->on_tesseract_message_updated(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_MESSAGE_REMOVED: {
        auto* p = reinterpret_cast<MainWindow::PostedMessageEvent*>(lParam);
        self->on_tesseract_message_removed(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_PAGINATE_DONE: {
        auto* p = reinterpret_cast<std::string*>(lParam);
        self->on_tesseract_paginate_done(p, wParam != 0);
        delete p;
        return 0;
    }
    case WM_TESSERACT_SUBSCRIBE_DONE: {
        auto* p = reinterpret_cast<std::string*>(lParam);
        self->on_tesseract_subscribe_done(p, wParam != 0);
        delete p;
        return 0;
    }
    case WM_TESSERACT_ROOMS: {
        auto* p = reinterpret_cast<std::vector<tesseract::RoomInfo>*>(lParam);
        self->on_tesseract_rooms(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_SYNC_ERROR: {
        auto* p = reinterpret_cast<std::string*>(lParam);
        MessageBoxA(hwnd, p->c_str(), "Sync error", MB_ICONWARNING);
        delete p;
        return 0;
    }
    case WM_TESSERACT_TIMELINE_RESET: {
        auto* p = reinterpret_cast<MainWindow::PostedTimelineReset*>(lParam);
        self->on_tesseract_timeline_reset(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_RECONNECT:
        self->on_reconnect();
        return 0;
    case WM_TESSERACT_AUTH_ERROR:
        self->on_auth_error(static_cast<bool>(wParam));
        return 0;
    case WM_TESSERACT_BACKUP_PROGRESS: {
        auto* p = reinterpret_cast<tesseract::BackupProgress*>(lParam);
        self->on_backup_progress(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_RECOVER_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_recover_done(wParam != 0, std::move(*p));
        delete p;
        return 0;
    }
    case WM_TESSERACT_IMAGE_PACKS:
        self->refresh_sticker_picker();
        return 0;
    case WM_TESSERACT_ACCOUNT_PREFS: {
        auto* s = reinterpret_cast<std::string*>(lParam);
        auto prefs = tesseract::Prefs::parse(*s);
        delete s;
        if (!prefs.last_room.empty() &&
            self->pending_restore_room_.empty() &&
            self->current_room_id_.empty())
        {
            self->pending_restore_room_ = prefs.last_room;
        }
        return 0;
    }
    case WM_TESSERACT_STICKER_BYTES: {
        auto* p = reinterpret_cast<StickerBytesPayload*>(lParam);
        self->sticker_fetches_in_flight_.erase(p->cache_key);
        if (!p->bytes.empty()) {
            // Animated takes priority over static. try_load_animation is
            // a no-op when the buffer is single-frame; in that case fall
            // through to the static-decode path.
            self->try_load_animation(p->cache_key, p->bytes);
            if (!self->tk_anim_images_.count(p->cache_key) &&
                self->msg_surface_)
            {
                if (auto img = self->msg_surface_->factory().decode_image(p->bytes))
                    self->tk_images_.emplace(p->cache_key, std::move(img));
            }
            if (self->sticker_picker_shared_)
                self->sticker_picker_shared_->invalidate_image_cache();
            if (self->sticker_picker_surface_ &&
                self->sticker_picker_surface_->hwnd())
            {
                InvalidateRect(self->sticker_picker_surface_->hwnd(),
                                nullptr, FALSE);
            }
        }
        delete p;
        return 0;
    }
    case WM_TESSERACT_MEDIA_BYTES: {
        auto* p = reinterpret_cast<MediaBytesPayload*>(lParam);
        self->media_fetches_in_flight_.erase(p->cache_key);
        if (!p->bytes.empty()) {
            using Kind = MainWindow::MediaKind;
            HWND invalidate_hwnd = nullptr;
            switch (p->kind) {
            case Kind::RoomAvatar:
                if (self->room_surface_) {
                    if (auto img = self->room_surface_->factory()
                                       .decode_image(p->bytes))
                        self->tk_avatars_.emplace(p->cache_key,
                                                    std::move(img));
                    invalidate_hwnd = self->room_surface_->hwnd();
                }
                break;
            case Kind::UserAvatar:
                if (self->msg_surface_) {
                    if (auto img = self->msg_surface_->factory()
                                       .decode_image(p->bytes))
                        self->tk_avatars_.emplace(p->cache_key,
                                                    std::move(img));
                    invalidate_hwnd = self->msg_surface_->hwnd();
                }
                break;
            case Kind::MediaImage:
                if (self->msg_surface_) {
                    self->try_load_animation(p->cache_key, p->bytes);
                    if (!self->tk_anim_images_.count(p->cache_key)) {
                        if (auto img = self->msg_surface_->factory()
                                           .decode_image(p->bytes))
                            self->tk_images_.emplace(p->cache_key,
                                                       std::move(img));
                    }
                    invalidate_hwnd = self->msg_surface_->hwnd();
                }
                break;
            }
            if (invalidate_hwnd)
                InvalidateRect(invalidate_hwnd, nullptr, FALSE);
        }
        delete p;
        return 0;
    }
    case WM_TIMER:
        if (wParam == kSearchDebounceTimer) {
            KillTimer(hwnd, kSearchDebounceTimer);
            if (self->room_list_view_)
                self->room_list_view_->set_search_text(self->pending_search_text_);
            return 0;
        }
        if (wParam == kAnimTimerId) { self->on_anim_tick(); return 0; }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

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

bool MainWindow::register_class(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWindow::wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    // No class brush: WM_ERASEBKGND handler fills with the themed window_bg.
    // This lets DWM Mica show through during the brief moments where a child
    // control hasn't yet repainted (resize) and avoids a stale-white flash on
    // dark-mode startup.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    // Big icon: Alt+Tab, taskbar. Small icon: titlebar, system menu.
    // LoadImage picks the best-matching frame from the multi-resolution .ico.
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!wc.hIcon)   wc.hIcon   = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    return RegisterClassExW(&wc) != 0;
}

MainWindow::MainWindow(HINSTANCE hInst) : hInst_(hInst) {}

MainWindow::~MainWindow() {
    client_.stop_sync();
    // login_view_ holds a reference to client_ and calls cancel_oauth() +
    // joins its worker on destruction. Tear it down here so client_ is
    // still alive when ~LoginView runs.
    login_view_.reset();
    for (auto& [k, v] : avatar_cache_)      delete v;
    for (auto& [k, v] : user_avatar_cache_) delete v;
    theme::shutdown();
    win32::text::shutdown();
    if (gdiplus_token_)
        Gdiplus::GdiplusShutdown(gdiplus_token_);
}

bool MainWindow::create(int nCmdShow) {
    hwnd_ = CreateWindowExW(
        0, CLASS_NAME, L"Tesseract",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::on_create(HWND hwnd) {
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus_token_, &gsi, nullptr);
    win32::text::init();
    HDC dpi_dc = GetDC(hwnd);
    UINT dpi = dpi_dc ? GetDeviceCaps(dpi_dc, LOGPIXELSY) : 96;
    if (dpi_dc) ReleaseDC(hwnd, dpi_dc);
    win32::text::on_dpi_changed(dpi);

    // Initialise theme + DWM attributes for the caption + Mica backdrop
    // before any child controls are created so the first paint already
    // reflects dark / light mode.
    theme::register_main_window(hwnd);
    theme::apply_window_attributes(hwnd);

    // Room list — shared toolkit Surface hosting tesseract::views::RoomListView.
    room_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto view = std::make_unique<tesseract::views::RoomListView>();
        room_list_view_ = view.get();
        room_list_view_->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        room_list_view_->on_room_selected =
            [this](const std::string& room_id) { on_room_selected(room_id); };
        room_surface_->set_root(std::move(view));

        // Search field — host-overlaid NativeTextField shown when the
        // list overflows the viewport (decided inside RoomListView).
        room_search_field_ = room_surface_->host().make_text_field();
        room_search_field_->set_placeholder("Search rooms");
        room_search_field_->set_visible(false);
        room_search_field_->set_on_changed([this](const std::string& q) {
            pending_search_text_ = q;
            KillTimer(hwnd_, kSearchDebounceTimer);
            SetTimer(hwnd_, kSearchDebounceTimer, 500, nullptr);
        });
        room_surface_->set_on_layout([this] {
            if (!room_list_view_ || !room_search_field_) return;
            bool visible = room_list_view_->search_field_visible();
            room_search_field_->set_visible(visible);
            if (visible) {
                room_search_field_->set_rect(
                    room_list_view_->search_field_rect());
            }
        });
    }
    if (HWND rs = room_surface_->hwnd()) {
        SetWindowPos(rs, nullptr, 0, 0, 240, 600,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 1px vertical separator between the sidebar and the chat area.
    hSideSep_ = CreateWindowExW(
        0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        240, 0, 1, 600,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SIDE_SEPARATOR)),
        hInst_, nullptr);

    // Room header — above message area
    WNDCLASSEXW rhwc{};
    rhwc.cbSize = sizeof(rhwc);
    rhwc.lpfnWndProc = room_header_wnd_proc;
    rhwc.hInstance = hInst_;
    rhwc.lpszClassName = L"TesseractRoomHeader";
    RegisterClassExW(&rhwc);

    hRoomHeader_ = CreateWindowExW(
        0, L"TesseractRoomHeader", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        240, 0, 784, kRoomHeaderH,
        hwnd, nullptr, hInst_, nullptr);
    SetWindowLongPtrW(hRoomHeader_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hRoomHeader_, SW_HIDE);

    // User identity strip (sidebar footer)
    WNDCLASSEXW uswc{};
    uswc.cbSize = sizeof(uswc);
    uswc.lpfnWndProc = user_strip_wnd_proc;
    uswc.hInstance = hInst_;
    uswc.lpszClassName = L"TesseractUserStrip";
    uswc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&uswc);

    hUserStrip_ = CreateWindowExW(
        0, L"TesseractUserStrip", nullptr,
        WS_CHILD,
        0, 600 - kUserStripH, 240, kUserStripH,
        hwnd, nullptr, hInst_, nullptr);
    SetWindowLongPtrW(hUserStrip_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Space navigation bar: ← back button + space name label.
    // Hidden until the user drills into a space; refresh_room_list shows them.
    hSpaceNavBack_ = CreateWindowExW(
        0, L"BUTTON", L"\x2190",
        WS_CHILD | BS_PUSHBUTTON,
        4, 8, 28, 20,
        hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPACE_BACK)),
        hInst_, nullptr);
    if (hSpaceNavBack_) apply_default_font(hSpaceNavBack_);

    hSpaceNavLabel_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
        36, 0, 240 - 40, kSpaceNavBarH,
        hwnd, nullptr, hInst_, nullptr);
    if (hSpaceNavLabel_) apply_default_font(hSpaceNavLabel_);

    // Message list — shared toolkit Surface hosting MessageListView.
    msg_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto view = std::make_unique<tesseract::views::MessageListView>();
        message_list_view_ = view.get();
        message_list_view_->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        message_list_view_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                // Animated entries take priority — `on_anim_tick` keeps
                // `current` valid; static cache is the second hop.
                auto ait = tk_anim_images_.find(mxc);
                if (ait != tk_anim_images_.end() && !ait->second.frames.empty())
                    return ait->second.frames[ait->second.current].get();
                auto it = tk_images_.find(mxc);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        message_list_view_->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key) {
                if (current_room_id_.empty()) return;
                client_.send_reaction(current_room_id_, event_id, key);
            };
        message_list_view_->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor) {
                if (current_room_id_.empty()) return;
                pending_reaction_event_id_ = event_id;
                // anchor is in MessageListView-local coords; the view is
                // the root of msg_surface_, so the rect maps directly to
                // surface client coords.
                if (msg_surface_ && msg_surface_->hwnd())
                    popup_emoji_at_rect(msg_surface_->hwnd(), anchor);
                else
                    toggle_emoji_picker();
            };
        message_list_view_->on_reply_requested =
            [this](const std::string& event_id,
                   const std::string& sender_name,
                   const std::string& body_preview) {
                if (!compose_shared_) return;
                compose_shared_->set_reply_to(event_id, sender_name,
                                              body_preview);
                if (compose_text_area_) compose_text_area_->set_focused(true);
            };
        message_list_view_->on_edit_requested =
            [this](const std::string& event_id,
                   const std::string& current_body) {
                if (!compose_shared_) return;
                compose_shared_->set_editing(event_id);
                if (compose_text_area_) {
                    compose_text_area_->set_text(current_body);
                    compose_shared_->set_current_text(current_body);
                    compose_text_area_->set_focused(true);
                }
            };
        message_list_view_->on_near_top = [this]{
            if (current_room_id_.empty()) return;
            request_more_history(current_room_id_);
        };
        msg_surface_->set_root(std::move(view));
    }
    if (HWND ms = msg_surface_->hwnd()) {
        SetWindowPos(ms, nullptr, 240, kRoomHeaderH, 784, 700 - kRoomHeaderH,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Compose bar — shared widget on a tk::win32::Surface. The text input
    // is a NativeTextArea overlay on the bar's text_area_rect; emoji and
    // send buttons paint into the toolkit.
    compose_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto bar = std::make_unique<tesseract::views::ComposeBar>();
        compose_shared_ = bar.get();
        compose_shared_->on_send  = [this](const std::string& body) {
            std::string trimmed = body;
            auto l = trimmed.find_first_not_of(" \t\n\r");
            auto r = trimmed.find_last_not_of(" \t\n\r");
            if (l == std::string::npos) return;
            trimmed = trimmed.substr(l, r - l + 1);
            if (trimmed.empty() || current_room_id_.empty()) return;
            auto res = client_.send_message(current_room_id_, trimmed);
            if (res) {
                if (compose_text_area_) compose_text_area_->set_text("");
                compose_shared_->set_current_text({});
            } else {
                MessageBoxW(hwnd_, utf8_to_wstr(res.message).c_str(),
                             L"Send failed", MB_ICONWARNING);
            }
        };
        compose_shared_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                                  std::string mime,
                                                  std::string filename,
                                                  std::string caption,
                                                  std::uint32_t /*src_w*/,
                                                  std::uint32_t /*src_h*/,
                                                  std::string reply_event_id) {
            if (current_room_id_.empty() || !compose_surface_) return;
            const bool compress =
                tesseract::Settings::instance().image_quality
                == tesseract::Settings::ImageQuality::Compressed;
            auto enc = compose_surface_->host().encode_for_send(
                bytes.data(), bytes.size(), compress);
            if (enc.bytes.empty()) return;
            std::string out_name = filename;
            if (enc.mime == "image/jpeg") {
                auto dot = out_name.find_last_of('.');
                if (dot != std::string::npos) out_name = out_name.substr(0, dot);
                out_name += ".jpg";
            }
            auto res = client_.send_image(current_room_id_, enc.bytes, enc.mime,
                                            out_name, caption,
                                            enc.width, enc.height,
                                            reply_event_id);
            if (res) {
                if (compose_text_area_) compose_text_area_->set_text("");
                if (compose_shared_)    compose_shared_->set_current_text({});
            }
        };
        compose_shared_->on_size_changed = [this] {
            if (!hwnd_) return;
            RECT rc; GetClientRect(hwnd_, &rc);
            on_size(rc.right, rc.bottom);
        };
        compose_shared_->on_emoji   = [this] { toggle_emoji_picker(); };
        compose_shared_->on_sticker = [this] { toggle_sticker_picker(); };
        compose_shared_->on_send_reply = [this](const std::string& reply_event_id,
                                                 const std::string& body) {
            if (body.empty() || current_room_id_.empty()) return;
            client_.send_reply(current_room_id_, reply_event_id, body);
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        };
        compose_surface_->set_root(std::move(bar));

        compose_text_area_ = compose_surface_->host().make_text_area();
        compose_text_area_->set_placeholder("Message…");
        compose_text_area_->set_on_changed([this](const std::string& s) {
            if (compose_shared_) compose_shared_->set_current_text(s);
        });
        compose_text_area_->set_on_submit([this] { on_send_clicked(); });
        compose_text_area_->set_on_height_changed([this](float h) {
            if (!compose_shared_) return;
            compose_shared_->set_text_area_natural_height(h);
            // Re-run the host layout to reposition the compose surface
            // (its height tracks the shared widget's natural_height()).
            RECT rc; GetClientRect(hwnd_, &rc);
            on_size(rc.right, rc.bottom);
        });
        compose_text_area_->set_on_image_paste(
            [this](std::vector<std::uint8_t> bytes, std::string mime) {
                if (compose_shared_)
                    compose_shared_->set_pending_image(std::move(bytes),
                                                        std::move(mime));
            });

        // Drag-and-drop: any file dropped on the message list or
        // composer parks in the compose bar. Images use the preview
        // band; everything else uses the file chip.
        auto on_file_drop = [this](std::vector<std::uint8_t> bytes,
                                   std::string mime,
                                   std::string filename) {
            if (!compose_shared_) return;
            const auto limit = client_.media_upload_limit();
            if (limit > 0 && bytes.size() > limit) {
                if (hStatus_) SetWindowTextW(hStatus_,
                    L"File exceeds server limit");
                return;
            }
            if (mime.rfind("image/", 0) == 0) {
                compose_shared_->set_pending_image(std::move(bytes),
                                                   std::move(mime),
                                                   std::move(filename));
            } else {
                compose_shared_->set_pending_file(std::move(bytes),
                                                  std::move(mime),
                                                  std::move(filename));
            }
        };
        compose_surface_->set_on_file_drop(on_file_drop);
        if (msg_surface_) msg_surface_->set_on_file_drop(on_file_drop);

        compose_shared_->on_send_file =
            [this](std::vector<std::uint8_t> bytes,
                   std::string mime,
                   std::string filename,
                   std::string caption,
                   std::string reply_event_id) {
            if (current_room_id_.empty()) return;
            auto res = client_.send_file(current_room_id_, bytes, mime,
                                          filename, caption, reply_event_id);
            if (res) {
                if (compose_text_area_) compose_text_area_->set_text("");
                if (compose_shared_)    compose_shared_->set_current_text({});
            } else {
                if (hStatus_) SetWindowTextW(hStatus_, L"Send file failed");
            }
        };
        compose_shared_->on_send_edit = [this](const std::string& event_id,
                                                const std::string& new_body) {
            if (new_body.empty() || current_room_id_.empty()) return;
            client_.send_edit(current_room_id_, event_id, new_body);
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        };
        compose_shared_->on_edit_cancelled = [this] {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        };

        compose_surface_->set_on_layout([this] {
            if (compose_shared_ && compose_text_area_)
                compose_text_area_->set_rect(compose_shared_->text_area_rect());
        });
    }

    // Emoji picker creation is deferred until first toggle so the cold-
    // start path stays cheap. Recents live in account-data
    // (io.element.recent_emoji), read on demand by the shared picker.

    // Custom flat status strip. Replaces STATUSCLASSNAMEW which carries a 9x
    // size-grip and chunky inset borders.
    register_status_bar_class(hInst_);
    hStatus_ = CreateWindowExW(
        0, L"TesseractStatusBar", L"Not logged in",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        hwnd, nullptr, hInst_, nullptr);

    // Recovery banner — shared widget on a tk::win32::Surface. Initially
    // hidden; toggled by maybe_show_recovery_banner() after start_sync.
    recovery_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::RecoveryBanner>();
        recovery_shared_ = banner.get();
        recovery_shared_->on_verify =
            [this](const std::string& /*key*/) { on_recovery_verify_clicked(); };
        recovery_shared_->on_dismiss =
            [this] { on_recovery_dismiss_clicked(); };
        recovery_surface_->set_root(std::move(banner));

        recovery_key_field_ = recovery_surface_->host().make_text_field();
        recovery_key_field_->set_placeholder("Recovery key or passphrase");
        recovery_key_field_->set_password(true);
        recovery_key_field_->set_on_changed([this](const std::string& k) {
            if (recovery_shared_) recovery_shared_->set_current_key(k);
        });
        recovery_key_field_->set_on_submit([this] { on_recovery_verify_clicked(); });
        recovery_surface_->set_on_layout([this] {
            if (!recovery_shared_ || !recovery_key_field_) return;
            recovery_key_field_->set_visible(
                recovery_shared_->recovery_key_field_visible());
            recovery_key_field_->set_rect(
                recovery_shared_->recovery_key_field_rect());
        });
    }
    if (HWND rb = recovery_surface_->hwnd()) {
        SetWindowPos(rb, nullptr, 240, kRoomHeaderH, 784, 48,
                      SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(rb, SW_HIDE);
    }

    login_view_ = std::make_unique<LoginView>(hInst_, hwnd, client_);
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    ShowWindow(login_view_->hwnd(), SW_HIDE);

    start_login();
}

void MainWindow::on_destroy() {
    if (anim_timer_running_ && hwnd_) {
        KillTimer(hwnd_, kAnimTimerId);
        anim_timer_running_ = false;
    }
    client_.stop_sync();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h) {
    constexpr int ROOM_W   = 240;
    constexpr int SEP_W    = 1;
    constexpr int CHAT_X   = ROOM_W + SEP_W;
    constexpr int STATUS_H = 24;

    // Custom status bar is anchored to the bottom. Position it explicitly.
    if (hStatus_) {
        SetWindowPos(hStatus_, nullptr,
                     0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    }

    if (login_visible_ && login_view_ && login_view_->hwnd()) {
        SetWindowPos(login_view_->hwnd(), nullptr,
                     0, 0, w, h - STATUS_H, SWP_NOZORDER);
        login_view_->layout(w, h - STATUS_H);
        return;
    }

    int content_h = h - STATUS_H;
    // Drive layout from the compose bar's actual height so it never
    // overflows below the window when a reply/edit banner or attachment
    // is shown.
    int compose_h = compose_shared_
        ? static_cast<int>(compose_shared_->natural_height())
        : static_cast<int>(tesseract::views::ComposeBar::kMinHeight);
    compose_h = std::clamp(compose_h,
                           static_cast<int>(tesseract::views::ComposeBar::kMinHeight),
                           content_h / 2);
    int msg_h = content_h - compose_h;

    constexpr int BANNER_H = 48;

    int msg_area_y = 0;
    int msg_area_h = msg_h;
    if (hRoomHeader_ && IsWindowVisible(hRoomHeader_)) {
        msg_area_y = kRoomHeaderH;
        msg_area_h -= kRoomHeaderH;
    }
    if (recovery_banner_visible_ && recovery_surface_
        && recovery_surface_->hwnd())
    {
        SetWindowPos(recovery_surface_->hwnd(), nullptr,
                      CHAT_X, msg_area_y, w - CHAT_X, BANNER_H,
                      SWP_NOZORDER | SWP_NOACTIVATE);
        msg_area_y += BANNER_H;
        msg_area_h -= BANNER_H;
    }

    // Sidebar fills the full content height (status bar excluded): the
    // user strip sits flush against the bottom, with the room list above.
    // The compose bar lives at x=CHAT_X so the left sidebar continues
    // all the way down to content_h to avoid an empty band beneath the
    // user strip.
    bool nav_bar_visible = hSpaceNavBack_ && IsWindowVisible(hSpaceNavBack_);
    int  room_surface_y  = nav_bar_visible ? kSpaceNavBarH : 0;
    if (nav_bar_visible) {
        SetWindowPos(hSpaceNavBack_,  nullptr, 4,  8,               28,               20,               SWP_NOZORDER);
        SetWindowPos(hSpaceNavLabel_, nullptr, 36, 0, ROOM_W - 40, kSpaceNavBarH, SWP_NOZORDER);
    }

    bool user_strip_visible = hUserStrip_ && IsWindowVisible(hUserStrip_);
    int sidebar_h   = content_h;
    int room_list_h = (user_strip_visible ? sidebar_h - kUserStripH : sidebar_h) - room_surface_y;

    if (room_surface_ && room_surface_->hwnd()) {
        SetWindowPos(room_surface_->hwnd(), nullptr,
                      0, room_surface_y, ROOM_W, room_list_h,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hUserStrip_) {
        SetWindowPos(hUserStrip_, nullptr,
                     0, room_surface_y + room_list_h, ROOM_W, kUserStripH, SWP_NOZORDER);
    }
    if (hSideSep_) {
        SetWindowPos(hSideSep_, nullptr, ROOM_W, 0, SEP_W, sidebar_h, SWP_NOZORDER);
    }
    SetWindowPos(hRoomHeader_, nullptr, CHAT_X, 0, w - CHAT_X, kRoomHeaderH, SWP_NOZORDER);
    if (msg_surface_ && msg_surface_->hwnd()) {
        SetWindowPos(msg_surface_->hwnd(), nullptr,
                      CHAT_X, msg_area_y, w - CHAT_X, msg_area_h,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (compose_surface_ && compose_surface_->hwnd()) {
        SetWindowPos(compose_surface_->hwnd(), nullptr,
                      CHAT_X, msg_h, w - CHAT_X, compose_h,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SendMessageW(hStatus_, WM_SIZE, 0, 0);
}

// ---------------------------------------------------------------------------
// Login / reconnect
// ---------------------------------------------------------------------------

void MainWindow::start_login() {
    std::wstring status_msg;
    if (auto saved = tesseract::SessionStore::load()) {
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Restoring session…"));
        auto res = client_.restore_session(*saved);
        if (res) {
            my_user_id_       = client_.get_user_id();
            my_display_name_  = client_.get_display_name();
            my_avatar_url_    = client_.get_avatar_url();
            pending_restore_room_ = tesseract::Prefs::parse(client_.load_prefs_json()).last_room;
            populate_user_strip();
            event_handler_ = std::make_unique<EventHandler>(hwnd_);
            client_.start_sync(event_handler_.get());
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Connected"));
            show_main_content();
            maybe_show_recovery_banner();
            return;
        }
        tesseract::SessionStore::clear();
        status_msg = L"Saved session expired: ";
        status_msg += utf8_to_wstr(res.message);
    }

    if (login_view_) {
        login_view_->reset();
        login_view_->set_status_message(status_msg);
    }
    show_login_view();
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Not logged in"));
}

void MainWindow::on_login_succeeded() {
    my_user_id_       = client_.get_user_id();
    my_display_name_  = client_.get_display_name();
    my_avatar_url_    = client_.get_avatar_url();
    pending_restore_room_ = tesseract::Prefs::parse(client_.load_prefs_json()).last_room;
    populate_user_strip();
    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(hwnd_);
    client_.start_sync(event_handler_.get());
    SendMessageW(hStatus_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Connected"));
    show_main_content();
    maybe_show_recovery_banner();
}

void MainWindow::show_login_view() {
    login_visible_ = true;
    if (login_view_ && login_view_->hwnd()) {
        ShowWindow(login_view_->hwnd(), SW_SHOW);
    }
    // Hide all main-content widgets.
    if (room_surface_ && room_surface_->hwnd())
        ShowWindow(room_surface_->hwnd(), SW_HIDE);
    ShowWindow(hSideSep_,         SW_HIDE);
    ShowWindow(hRoomHeader_,      SW_HIDE);
    ShowWindow(hUserStrip_,       SW_HIDE);
    if (msg_surface_ && msg_surface_->hwnd())
        ShowWindow(msg_surface_->hwnd(), SW_HIDE);
    if (compose_surface_ && compose_surface_->hwnd())
        ShowWindow(compose_surface_->hwnd(), SW_HIDE);
    if (recovery_surface_ && recovery_surface_->hwnd())
        ShowWindow(recovery_surface_->hwnd(), SW_HIDE);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::show_main_content() {
    login_visible_ = false;
    if (login_view_ && login_view_->hwnd()) {
        ShowWindow(login_view_->hwnd(), SW_HIDE);
    }
    // Unconditional main-content widgets. Conditional widgets
    // (hRoomHeader_, hUserStrip_, recovery banner) are shown by their own
    // code paths once their state is set.
    if (room_surface_ && room_surface_->hwnd())
        ShowWindow(room_surface_->hwnd(), SW_SHOW);
    ShowWindow(hSideSep_,  SW_SHOW);
    if (msg_surface_ && msg_surface_->hwnd())
        ShowWindow(msg_surface_->hwnd(), SW_SHOW);
    if (compose_surface_ && compose_surface_->hwnd())
        ShowWindow(compose_surface_->hwnd(), SW_SHOW);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::on_reconnect() {
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Sync error: reconnecting…"));
    client_.stop_sync();
    start_login();
}

void MainWindow::on_auth_error(bool soft_logout) {
    if (soft_logout) {
        if (auto saved = tesseract::SessionStore::load()) {
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Reconnecting session…"));
            if (client_.restore_session(*saved)) {
                my_user_id_       = client_.get_user_id();
                my_display_name_  = client_.get_display_name();
                my_avatar_url_    = client_.get_avatar_url();
                populate_user_strip();
                event_handler_ = std::make_unique<EventHandler>(hwnd_);
                client_.start_sync(event_handler_.get());
                SendMessageW(hStatus_, SB_SETTEXTW, 0,
                             reinterpret_cast<LPARAM>(L"Reconnected"));
                maybe_show_recovery_banner();
                return;
            }
        }
    }
    tesseract::SessionStore::clear();
    client_.stop_sync();
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Session expired; please log in again."));
    start_login();
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

void MainWindow::on_send_clicked() {
    if (compose_shared_) compose_shared_->trigger_send();
}

// ---------------------------------------------------------------------------
// Room selection
// ---------------------------------------------------------------------------

void MainWindow::on_room_selected(const std::string& room_id) {
    if (room_id.empty()) return;

    for (const auto& r : rooms_) {
        if (r.id == room_id && r.is_space) {
            space_stack_.push_back(room_id);
            refresh_room_list();
            return;
        }
    }

    if (!current_room_id_.empty() && current_room_id_ != room_id)
        client_.unsubscribe_room(current_room_id_);

    current_room_id_ = room_id;
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_.load_prefs_json());
        prefs.last_room = room_id;
        client_.save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (compose_shared_) {
        compose_shared_->clear_reply();
        compose_shared_->clear_editing();
    }
    for (const auto& r : rooms_) {
        if (r.id == current_room_id_) {
            current_room_info_ = r;
            update_room_header(r);
            break;
        }
    }
    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the Win32 message pump stays responsive.
    HWND hwnd = hwnd_;
    std::string sub_room = current_room_id_;
    std::thread([this, sub_room, hwnd]{
        auto res = client_.subscribe_room(sub_room);
        bool reached = false;
        if (res) {
            auto pr = client_.paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            client_.start_background_backfill();
        }
        auto* p = new std::string(sub_room);
        PostMessageW(hwnd, WM_TESSERACT_SUBSCRIBE_DONE,
                      static_cast<WPARAM>(reached), reinterpret_cast<LPARAM>(p));
    }).detach();
}

void MainWindow::request_more_history(const std::string& room_id) {
    if (room_id.empty()) return;
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    HWND hwnd = hwnd_;
    std::thread([this, room_id, hwnd]{
        auto pr = client_.paginate_back_with_status(room_id, kPaginationBatch);
        auto* p = new std::string(room_id);
        PostMessageW(hwnd, WM_TESSERACT_PAGINATE_DONE,
                      static_cast<WPARAM>(pr.ok && pr.reached_start),
                      reinterpret_cast<LPARAM>(p));
    }).detach();
}

void MainWindow::on_tesseract_paginate_done(std::string* room_id,
                                              bool reached_start) {
    if (!room_id) return;
    auto it = pagination_.find(*room_id);
    if (it == pagination_.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached_start;
    if (*room_id == current_room_id_ && message_list_view_)
        message_list_view_->reset_near_top_latch();
}

void MainWindow::update_room_header(const tesseract::RoomInfo& info) {
    bool was_visible = hRoomHeader_ && IsWindowVisible(hRoomHeader_);
    if (info.id.empty()) {
        ShowWindow(hRoomHeader_, SW_HIDE);
        if (was_visible && hwnd_) {
            RECT rc; GetClientRect(hwnd_, &rc);
            on_size(rc.right, rc.bottom);
        }
        return;
    }
    ShowWindow(hRoomHeader_, SW_SHOW);
    InvalidateRect(hRoomHeader_, nullptr, FALSE);
    if (!was_visible && hwnd_) {
        RECT rc; GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
    }
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

void MainWindow::on_tesseract_timeline_reset(PostedTimelineReset* payload) {
    if (!payload) return;
    if (payload->room_id != current_room_id_) return;
    if (!message_list_view_) return;

    std::vector<tesseract::views::MessageRowData> rows;
    rows.reserve(payload->snapshot.size());
    for (auto& ev : payload->snapshot) {
        if (!ev) continue;
        ensure_row_media(*ev);
        ensure_reply_details(ev->in_reply_to_id);
        rows.push_back(to_row_data(*ev));
    }
    message_list_view_->set_messages(std::move(rows));
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_message_inserted(PostedMessageEvent* payload) {
    if (!payload || !payload->event) return;
    if (payload->room_id != current_room_id_) return;
    if (payload->event->type == tesseract::EventType::Unhandled) return;
    if (!message_list_view_) return;

    ensure_row_media(*payload->event);
    ensure_reply_details(payload->event->in_reply_to_id);
    message_list_view_->insert_message(payload->index,
                                         to_row_data(*payload->event));
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_message_updated(PostedMessageEvent* payload) {
    if (!payload || !payload->event) return;
    if (payload->room_id != current_room_id_) return;
    if (payload->event->type == tesseract::EventType::Unhandled) return;
    if (!message_list_view_) return;

    ensure_row_media(*payload->event);
    ensure_reply_details(payload->event->in_reply_to_id);
    message_list_view_->update_message(payload->index,
                                         to_row_data(*payload->event));
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_message_removed(PostedMessageEvent* payload) {
    if (!payload) return;
    if (payload->room_id != current_room_id_) return;
    if (!message_list_view_) return;
    message_list_view_->remove_message(payload->index);
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms) {
    rooms_ = std::move(*rooms);
    refresh_room_list();
    if (!current_room_id_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == current_room_id_) { update_room_header(r); break; }
        }
    } else if (!pending_restore_room_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == pending_restore_room_ && !r.is_space) {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                on_room_selected(target);
                break;
            }
        }
    }
}

void MainWindow::on_tesseract_subscribe_done(std::string* room_id,
                                               bool reached_start) {
    if (!room_id || *room_id != current_room_id_) return;
    auto& state = pagination_[*room_id];
    state.in_flight     = false;
    state.reached_start = reached_start;
}

void MainWindow::refresh_room_list() {
    if (!room_list_view_) return;

    std::vector<tesseract::RoomInfo> filtered;
    if (space_stack_.empty()) {
        // Hide the space navigation bar and build the root list, excluding
        // rooms that are children of any space (they appear only when drilled in).
        if (hSpaceNavBack_)  ShowWindow(hSpaceNavBack_,  SW_HIDE);
        if (hSpaceNavLabel_) ShowWindow(hSpaceNavLabel_, SW_HIDE);
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
            if (r.is_space)
                for (const auto& id : client_.space_children(r.id))
                    in_space.insert(id);
        for (const auto& r : rooms_) if (!r.is_space && !in_space.count(r.id)) filtered.push_back(r);
        for (const auto& r : rooms_) if ( r.is_space) filtered.push_back(r);
    } else {
        // Show the navigation bar with the current space's name.
        for (const auto& r : rooms_) {
            if (r.id == space_stack_.back()) {
                if (hSpaceNavLabel_)
                    SetWindowTextW(hSpaceNavLabel_, utf8_to_wstr(r.name).c_str());
                break;
            }
        }
        if (hSpaceNavBack_)  ShowWindow(hSpaceNavBack_,  SW_SHOWNA);
        if (hSpaceNavLabel_) ShowWindow(hSpaceNavLabel_, SW_SHOWNA);
        auto child_ids = client_.space_children(space_stack_.back());
        for (const auto& r : rooms_) {
            if (std::find(child_ids.begin(), child_ids.end(), r.id)
                != child_ids.end()) {
                filtered.push_back(r);
            }
        }
    }
    for (const auto& r : filtered) ensure_room_avatar(r);

    room_list_view_->set_rooms(filtered);
    if (!current_room_id_.empty())
        room_list_view_->set_selected_room(current_room_id_);

    // Re-run the size layout so the room surface shifts up/down as the
    // nav bar appears or disappears.
    if (hwnd_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
    }
    if (room_surface_) room_surface_->relayout();
}

void MainWindow::on_space_back() {
    if (!space_stack_.empty()) space_stack_.pop_back();
    refresh_room_list();
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

// These three helpers used to call the synchronous Rust FFI directly on
// the UI thread. `fetch_avatar_bytes` / `fetch_media_bytes` do a
// `tokio::block_on` inside, so on first sync of an account with many
// rooms `refresh_room_list` was freezing the message pump for minutes
// (one network round-trip per room avatar, serialised on the UI thread).
// Decode + cache + repaint now happens via WM_TESSERACT_MEDIA_BYTES;
// the call sites return immediately and the next paint shows an
// initials placeholder until the bytes land.
void MainWindow::ensure_room_avatar(const tesseract::RoomInfo& r) {
    if (!room_surface_) return;
    request_room_avatar(r.id, r.avatar_url);
}

void MainWindow::ensure_user_avatar_tk(const std::string& mxc) {
    if (!msg_surface_) return;
    request_user_avatar(mxc);
}

void MainWindow::ensure_media_image(const std::string& url,
                                      int /*max_w*/, int /*max_h*/) {
    if (!msg_surface_) return;
    request_media_image(url);
}

void MainWindow::ensure_reply_details(const std::string& event_id) {
    if (event_id.empty() || current_room_id_.empty()) return;
    if (!reply_details_requested_.insert(event_id).second) return;
    client_.fetch_reply_details(current_room_id_, event_id);
}

// ---------------------------------------------------------------------------
// Animated media — multi-frame WIC decode + 60 Hz WM_TIMER tick
// ---------------------------------------------------------------------------

void MainWindow::try_load_animation(const std::string& url,
                                      std::span<const std::uint8_t> bytes)
{
    if (url.empty() || bytes.empty()) return;
    if (tk_anim_images_.count(url))   return;
    auto frames = tk::win32::decode_animation(bytes);
    if (frames.size() < 2) return;

    AnimatedImage entry;
    entry.frames.reserve(frames.size());
    entry.delays_ms.reserve(frames.size());
    for (auto& af : frames) {
        entry.frames.push_back(std::move(af.image));
        entry.delays_ms.push_back(af.delay_ms);
    }
    entry.current = 0;
    entry.next_advance_ms =
        static_cast<std::int64_t>(GetTickCount64()) + entry.delays_ms[0];

    tk_anim_images_.emplace(url, std::move(entry));
    // Drop any static-cache leftover from a prior probe to keep providers
    // from racing with two caches for the same URL.
    tk_images_.erase(url);

    if (!anim_timer_running_ && hwnd_) {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerHz, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::on_anim_tick() {
    if (tk_anim_images_.empty()) {
        if (anim_timer_running_ && hwnd_) {
            KillTimer(hwnd_, kAnimTimerId);
            anim_timer_running_ = false;
        }
        return;
    }
    const std::int64_t now = static_cast<std::int64_t>(GetTickCount64());
    bool any_changed = false;
    for (auto& [_, entry] : tk_anim_images_) {
        if (entry.frames.size() <= 1) continue;
        // Walk forward through every elapsed delay so we don't drift on
        // slow ticks (e.g. dragging the window). Cap at one full loop so
        // long pauses don't spin in catch-up.
        std::size_t steps = 0;
        while (now >= entry.next_advance_ms &&
                steps < entry.frames.size())
        {
            entry.current = (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms += entry.delays_ms[entry.current];
            ++steps;
        }
        if (steps > 0) any_changed = true;
    }
    if (!any_changed) return;
    if (msg_surface_ && msg_surface_->hwnd())
        InvalidateRect(msg_surface_->hwnd(), nullptr, FALSE);
    if (sticker_picker_surface_ && sticker_picker_surface_->hwnd() &&
        hStickerPicker_ && IsWindowVisible(hStickerPicker_))
    {
        if (sticker_picker_shared_) sticker_picker_shared_->invalidate_image_cache();
        InvalidateRect(sticker_picker_surface_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::request_sticker_image(const std::string& cache_key) {
    if (cache_key.empty()) return;
    if (tk_images_.count(cache_key) || tk_anim_images_.count(cache_key)) return;
    if (!sticker_fetches_in_flight_.insert(cache_key).second) return;

    HWND target = hwnd_;
    std::thread([this, target, cache_key]() {
        auto bytes = client_.fetch_source_bytes(cache_key);
        auto* p    = new StickerBytesPayload{ cache_key, std::move(bytes) };
        if (!PostMessageW(target, WM_TESSERACT_STICKER_BYTES, 0,
                          reinterpret_cast<LPARAM>(p)))
        {
            delete p;   // window already gone; drop the payload.
        }
    }).detach();
}

void MainWindow::request_room_avatar(const std::string& room_id,
                                       const std::string& mxc) {
    if (room_id.empty() || mxc.empty()) return;
    if (tk_avatars_.count(mxc)) return;
    if (!media_fetches_in_flight_.insert(mxc).second) return;

    HWND target = hwnd_;
    std::thread([this, target, room_id, mxc]() {
        auto bytes = client_.fetch_avatar_bytes(room_id);
        auto* p    = new MediaBytesPayload{
            MainWindow::MediaKind::RoomAvatar, mxc, std::move(bytes) };
        if (!PostMessageW(target, WM_TESSERACT_MEDIA_BYTES, 0,
                          reinterpret_cast<LPARAM>(p)))
        {
            delete p;
        }
    }).detach();
}

void MainWindow::request_user_avatar(const std::string& mxc) {
    if (mxc.empty()) return;
    if (tk_avatars_.count(mxc)) return;
    if (!media_fetches_in_flight_.insert(mxc).second) return;

    HWND target = hwnd_;
    std::thread([this, target, mxc]() {
        auto bytes = client_.fetch_media_bytes(mxc);
        auto* p    = new MediaBytesPayload{
            MainWindow::MediaKind::UserAvatar, mxc, std::move(bytes) };
        if (!PostMessageW(target, WM_TESSERACT_MEDIA_BYTES, 0,
                          reinterpret_cast<LPARAM>(p)))
        {
            delete p;
        }
    }).detach();
}

void MainWindow::request_media_image(const std::string& url) {
    if (url.empty()) return;
    if (tk_images_.count(url) || tk_anim_images_.count(url)) return;
    if (!media_fetches_in_flight_.insert(url).second) return;

    HWND target = hwnd_;
    std::thread([this, target, url]() {
        // `url` may be a plain `mxc://` (plain images/stickers) or a JSON
        // MediaSource (encrypted images, stickers, reaction sources).
        // `fetch_source_bytes` accepts both shapes; `fetch_media_bytes`
        // only handles plain mxc and would return empty for encrypted.
        auto bytes = client_.fetch_source_bytes(url);
        auto* p    = new MediaBytesPayload{
            MainWindow::MediaKind::MediaImage, url, std::move(bytes) };
        if (!PostMessageW(target, WM_TESSERACT_MEDIA_BYTES, 0,
                          reinterpret_cast<LPARAM>(p)))
        {
            delete p;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
//  Event → MessageRowData + append into the shared MessageListView
// ---------------------------------------------------------------------------

tesseract::views::MessageRowData MainWindow::to_row_data(
    const tesseract::Event& ev)
{
    using Kind = tesseract::views::MessageRowData::Kind;
    tesseract::views::MessageRowData row;
    row.event_id          = ev.event_id;
    row.sender            = ev.sender;
    row.sender_name       = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body              = ev.body;
    row.timestamp_ms      = ev.timestamp;
    row.is_own            = !my_user_id_.empty() && ev.sender == my_user_id_;
    row.reactions         = ev.reactions;
    row.read_receipts     = ev.read_receipts;

    row.in_reply_to_id          = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    row.in_reply_to_body        = ev.in_reply_to_body;
    row.is_edited               = ev.is_edited;

    switch (ev.type) {
        case tesseract::EventType::Text:    row.kind = Kind::Text;    break;
        case tesseract::EventType::Image: {
            row.kind = Kind::Image;
            const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
            row.media_url            = img.image_url;
            row.media_w              = static_cast<int>(img.width);
            row.media_h              = static_cast<int>(img.height);
            row.has_filename_caption = !img.filename.empty();
            break;
        }
        case tesseract::EventType::Sticker: {
            row.kind = Kind::Sticker;
            const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
            row.media_url = s.image_url;
            row.media_w   = static_cast<int>(s.width);
            row.media_h   = static_cast<int>(s.height);
            break;
        }
        case tesseract::EventType::File: {
            row.kind = Kind::File;
            const auto& f = static_cast<const tesseract::FileEvent&>(ev);
            row.file_name = f.file_name;
            row.file_size = f.file_size;
            row.media_url = f.file_url;
            break;
        }
        case tesseract::EventType::Voice: {
            row.kind = Kind::Voice;
            const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
            row.audio_source = v.audio_source;
            row.audio_mime   = v.mime_type;
            row.duration_ms  = v.duration_ms;
            row.waveform     = v.waveform;
            break;
        }
        case tesseract::EventType::Redacted:  row.kind = Kind::Redacted;  break;
        case tesseract::EventType::Unhandled: row.kind = Kind::Unhandled; break;
    }
    return row;
}

void MainWindow::ensure_row_media(const tesseract::Event& ev) {
    ensure_user_avatar_tk(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts) {
        ensure_user_avatar_tk(rr.avatar_url);
    }
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        ensure_media_image(img.image_url,
                            tesseract::visual::kMaxInlineImageWidth,
                            tesseract::visual::kMaxInlineImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        ensure_media_image(s.image_url,
                            tesseract::visual::kStickerSize,
                            tesseract::visual::kStickerSize);
    } else if (ev.type == tesseract::EventType::Voice) {
        const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            voice_prefetched_.insert(v.audio_source).second) {
            // Win32 has no audio backend yet (`make_audio_player()`
            // returns nullptr) — prefetch is still worth doing so the
            // bytes land in the SDK cache for whenever a backend lands.
            std::thread([this, src = v.audio_source]() mutable {
                (void)client_.fetch_source_bytes(src);
            }).detach();
        }
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            ensure_media_image(r.source_json, 20, 20);
    }
}

void MainWindow::clear_messages() {
    if (!message_list_view_) return;
    message_list_view_->set_messages({});
    if (msg_surface_) msg_surface_->relayout();
}

// ---------------------------------------------------------------------------
// Recovery banner (Step 6) — inline key entry, no modal dialog.
// ---------------------------------------------------------------------------

namespace {
std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow_edit_utf8(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len), L'\0');
    GetWindowTextW(hEdit, buf.data(), len + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, buf.data(), len,
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), len,
                        out.data(), n, nullptr, nullptr);
    return out;
}
} // namespace

void MainWindow::maybe_show_recovery_banner() {
    if (recovery_banner_dismissed_) return;
    if (!client_.needs_recovery())  return;
    if (recovery_banner_visible_) return;
    if (!recovery_surface_ || !recovery_surface_->hwnd()) return;

    if (recovery_shared_) {
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Form);
        recovery_shared_->set_current_key("");
    }
    if (recovery_key_field_) {
        recovery_key_field_->set_text("");
        recovery_key_field_->set_enabled(true);
    }
    ShowWindow(recovery_surface_->hwnd(), SW_SHOW);
    recovery_surface_->relayout();
    recovery_banner_visible_ = true;
    recovery_in_flight_      = false;
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::on_recovery_verify_clicked() {
    std::string key;
    if (recovery_key_field_) key = recovery_key_field_->text();
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of (" \t\r\n");
    if (a == std::string::npos) {
        if (recovery_shared_) {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            recovery_shared_->set_failure_message(
                "Please enter a recovery key or passphrase.");
            if (recovery_surface_) recovery_surface_->relayout();
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (recovery_shared_)
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    if (recovery_key_field_) recovery_key_field_->set_enabled(false);
    if (recovery_surface_) recovery_surface_->relayout();
    recovery_in_flight_ = true;

    HWND target = hwnd_;
    std::thread([this, target, key]() {
        auto res = client_.recover(key);
        WPARAM ok = res.ok ? 1 : 0;
        auto*  p  = new std::wstring(widen_utf8(res.message));
        PostMessageW(target, WM_TESSERACT_RECOVER_DONE,
                     ok, reinterpret_cast<LPARAM>(p));
    }).detach();
}

void MainWindow::on_recover_done(bool ok, std::wstring msg) {
    if (ok) {
        if (recovery_shared_) {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Importing);
        }
        if (recovery_surface_) recovery_surface_->relayout();
        return;
    }
    if (recovery_shared_) {
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Failed);
        // wstring → utf8 for the failure detail.
        int n = WideCharToMultiByte(CP_UTF8, 0, msg.data(),
                                     static_cast<int>(msg.size()),
                                     nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg.data(),
                             static_cast<int>(msg.size()),
                             utf8.data(), n, nullptr, nullptr);
        recovery_shared_->set_failure_message(utf8);
    }
    if (recovery_key_field_) {
        recovery_key_field_->set_enabled(true);
        recovery_key_field_->set_focused(true);
    }
    if (recovery_surface_) recovery_surface_->relayout();
    recovery_in_flight_ = false;
}

void MainWindow::on_recovery_dismiss_clicked() {
    recovery_banner_dismissed_ = true;
    if (recovery_surface_ && recovery_surface_->hwnd())
        ShowWindow(recovery_surface_->hwnd(), SW_HIDE);
    recovery_banner_visible_ = false;
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::on_backup_progress(tesseract::BackupProgress* progress) {
    maybe_show_recovery_banner();

    if (recovery_banner_visible_
        && recovery_shared_
        && recovery_shared_->state()
            == tesseract::views::RecoveryBanner::State::Importing
        && progress->state == tesseract::BackupState::Downloading
        && progress->imported_keys > 0)
    {
        recovery_shared_->set_import_progress(progress->imported_keys);
        if (recovery_surface_) recovery_surface_->relayout();
    }
    if (progress->state == tesseract::BackupState::Enabled
        && !client_.needs_recovery())
    {
        if (recovery_surface_ && recovery_surface_->hwnd())
            ShowWindow(recovery_surface_->hwnd(), SW_HIDE);
        recovery_banner_visible_ = false;
        RECT rc; GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populate_user_strip() {
    if (user_avatar_bmp_) {
        delete user_avatar_bmp_;
        user_avatar_bmp_ = nullptr;
    }
    if (!my_avatar_url_.empty()) {
        auto bytes = client_.fetch_media_bytes(my_avatar_url_);
        if (!bytes.empty()) user_avatar_bmp_ = bitmap_from_bytes(bytes);
    }
    ShowWindow(hUserStrip_, SW_SHOW);
    InvalidateRect(hUserStrip_, nullptr, FALSE);
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::do_logout() {
    auto res = client_.logout();
    tesseract::SessionStore::clear();
    client_.stop_sync();
    event_handler_.reset();

    // Reset visible state.
    current_room_id_.clear();
    current_room_info_ = tesseract::RoomInfo{};
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    if (user_avatar_bmp_) { delete user_avatar_bmp_; user_avatar_bmp_ = nullptr; }
    rooms_.clear();
    if (room_list_view_)    room_list_view_->set_rooms({});
    if (message_list_view_) message_list_view_->set_messages({});
    if (room_surface_) room_surface_->relayout();
    if (msg_surface_)  msg_surface_->relayout();
    ShowWindow(hRoomHeader_, SW_HIDE);
    ShowWindow(hUserStrip_,  SW_HIDE);
    if (recovery_surface_ && recovery_surface_->hwnd())
        ShowWindow(recovery_surface_->hwnd(), SW_HIDE);
    recovery_banner_visible_   = false;
    recovery_banner_dismissed_ = false;
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
        reinterpret_cast<LPARAM>(res ? L"Signed out" : L"Sign out failed"));

    start_login();
}

// ---------------------------------------------------------------------------
// Emoji picker — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::EmojiPicker. The search field is a
// NativeTextField overlay (EDIT child); selection routes through
// insert_emoji_at_cursor below.
// ---------------------------------------------------------------------------

namespace {
constexpr const wchar_t* kEmojiPickerClass = L"TesseractEmojiPicker";
} // namespace

void MainWindow::ensure_emoji_picker_created() {
    if (hEmojiPicker_) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = hInst_;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // tk::win32::Surface paints the body
        wc.lpszClassName = kEmojiPickerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    hEmojiPicker_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kEmojiPickerClass, L"",
        WS_POPUP | WS_BORDER,
        0, 0, 320, 360,
        hwnd_, nullptr, hInst_, nullptr);
    if (!hEmojiPicker_) return;

    emoji_picker_surface_ =
        std::make_unique<tk::win32::Surface>(hInst_, hEmojiPicker_,
                                              tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(&client_);
    emoji_picker_shared_->on_selected =
        [this](const std::string& glyph) { insert_emoji_at_cursor(glyph); };
    emoji_picker_surface_->set_root(std::move(shared));

    if (HWND s = emoji_picker_surface_->hwnd()) {
        SetWindowPos(s, nullptr, 0, 0, 320, 360,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }

    emoji_picker_search_field_ = emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder("Search emoji");
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q) {
            if (emoji_picker_shared_) emoji_picker_shared_->set_search_query(q);
            if (emoji_picker_surface_) emoji_picker_surface_->relayout();
        });
    emoji_picker_surface_->set_on_layout([this] {
        if (emoji_picker_search_field_ && emoji_picker_shared_) {
            emoji_picker_search_field_->set_rect(
                emoji_picker_shared_->search_field_rect());
        }
    });
}

void MainWindow::toggle_emoji_picker() {
    ensure_emoji_picker_created();
    if (!hEmojiPicker_) return;

    if (IsWindowVisible(hEmojiPicker_)) {
        ShowWindow(hEmojiPicker_, SW_HIDE);
        return;
    }

    // Anchor the picker above the compose bar, clamped to the screen.
    // (With the shared ComposeBar there's no standalone emoji-button HWND
    // — the bar's surface is the closest available anchor.)
    RECT btn_rc{};
    if (compose_surface_ && compose_surface_->hwnd())
        GetWindowRect(compose_surface_->hwnd(), &btn_rc);
    else
        GetWindowRect(hwnd_, &btn_rc);
    int x = btn_rc.left + 8;
    int y = btn_rc.top - kEmojiPickH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        if (x < mi.rcWork.left) x = mi.rcWork.left + 4;
        if (y < mi.rcWork.top)  y = btn_rc.bottom + 4;
    }

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST,
                  x, y, kEmojiPickW, kEmojiPickH,
                  SWP_NOACTIVATE);

    if (emoji_picker_shared_) emoji_picker_shared_->refresh_frequents();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_text("");
    if (emoji_picker_shared_) emoji_picker_shared_->set_search_query("");

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_focused(true);
}

void MainWindow::popup_emoji_at_rect(HWND parent_hwnd, tk::Rect local_rect) {
    ensure_emoji_picker_created();
    if (!hEmojiPicker_ || !parent_hwnd) return;

    // Map the local rect into screen coordinates.
    POINT pt{ static_cast<LONG>(local_rect.x),
              static_cast<LONG>(local_rect.y) };
    ClientToScreen(parent_hwnd, &pt);
    LONG rectW = static_cast<LONG>(local_rect.w);
    LONG rectH = static_cast<LONG>(local_rect.h);

    // Prefer above, left-aligned with the rect; fall back to below if the
    // monitor doesn't have room. Clamp to the work area horizontally.
    int x = pt.x;
    int y = pt.y - kEmojiPickH - 4;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        if (y < mi.rcWork.top) y = pt.y + rectH + 4;
        if (x + kEmojiPickW > mi.rcWork.right)
            x = mi.rcWork.right - kEmojiPickW - 4;
        if (x < mi.rcWork.left) x = mi.rcWork.left + 4;
        if (y + kEmojiPickH > mi.rcWork.bottom)
            y = mi.rcWork.bottom - kEmojiPickH - 4;
    }
    (void)rectW;

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST,
                  x, y, kEmojiPickW, kEmojiPickH,
                  SWP_NOACTIVATE);

    if (emoji_picker_shared_) emoji_picker_shared_->refresh_frequents();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_text("");
    if (emoji_picker_shared_) emoji_picker_shared_->set_search_query("");

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_focused(true);
}

void MainWindow::insert_emoji_at_cursor(const std::string& glyph) {
    if (glyph.empty()) return;
    // Reaction mode: a "+" chip set pending_reaction_event_id_ before
    // opening the picker. Toggle the reaction (Rust handles add/remove)
    // and skip the compose insert.
    if (!pending_reaction_event_id_.empty()) {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty()) {
            client_.send_reaction(current_room_id_, ev, glyph);
        }
        if (hEmojiPicker_) ShowWindow(hEmojiPicker_, SW_HIDE);
        return;
    }
    if (!compose_text_area_) return;
    std::string cur = compose_text_area_->text();
    cur += glyph;
    compose_text_area_->set_text(cur);
    if (compose_shared_) compose_shared_->set_current_text(cur);
    compose_text_area_->set_focused(true);
    // The shared picker already called recent_emoji_bump before invoking
    // this callback — no need to re-bump here.
}

// ---------------------------------------------------------------------------
// Sticker picker — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::StickerPicker. Mirrors the emoji picker.
// Selection routes through Client::send_sticker. The image_provider reuses
// the per-window tk_images_ cache populated by ensure_media_image; cells
// for entries the cache hasn't seen yet render a placeholder shimmer.
// ---------------------------------------------------------------------------

namespace {
constexpr const wchar_t* kStickerPickerClass = L"TesseractStickerPicker";
} // namespace

void MainWindow::ensure_sticker_picker_created() {
    if (hStickerPicker_) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = hInst_;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // tk::win32::Surface paints the body
        wc.lpszClassName = kStickerPickerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    hStickerPicker_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kStickerPickerClass, L"",
        WS_POPUP | WS_BORDER,
        0, 0, kStickerPickW, kStickerPickH,
        hwnd_, nullptr, hInst_, nullptr);
    if (!hStickerPicker_) return;

    sticker_picker_surface_ =
        std::make_unique<tk::win32::Surface>(hInst_, hStickerPicker_,
                                              tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(&client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img) {
            if (!current_room_id_.empty()) {
                const std::string body =
                    img.body.empty() ? img.shortcode : img.body;
                client_.send_sticker(current_room_id_, body,
                                      img.url, img.info_json);
            }
            if (hStickerPicker_) ShowWindow(hStickerPicker_, SW_HIDE);
        };
    // Image provider: synchronous best-effort lookup against the
    // animated + static caches populated by message-list rendering. On
    // miss, kick off an async fetch via `request_sticker_image` so the
    // picker fills in stickers that haven't appeared in any message
    // yet. Decoded bytes land back via WM_TESSERACT_STICKER_BYTES.
    sticker_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            auto ait = tk_anim_images_.find(cache_key);
            if (ait != tk_anim_images_.end() && !ait->second.frames.empty())
                return ait->second.frames[ait->second.current].get();
            auto sit = tk_images_.find(cache_key);
            if (sit != tk_images_.end()) return sit->second.get();
            const_cast<MainWindow*>(this)->request_sticker_image(cache_key);
            return nullptr;
        });
    sticker_picker_surface_->set_root(std::move(shared));

    if (HWND s = sticker_picker_surface_->hwnd()) {
        SetWindowPos(s, nullptr, 0, 0, kStickerPickW, kStickerPickH,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder("Search stickers");
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q) {
            if (sticker_picker_shared_) sticker_picker_shared_->set_search_query(q);
            if (sticker_picker_surface_) sticker_picker_surface_->relayout();
        });
    sticker_picker_surface_->set_on_layout([this] {
        if (sticker_picker_search_field_ && sticker_picker_shared_) {
            sticker_picker_search_field_->set_rect(
                sticker_picker_shared_->search_field_rect());
        }
    });
}

void MainWindow::toggle_sticker_picker() {
    ensure_sticker_picker_created();
    if (!hStickerPicker_) return;

    if (IsWindowVisible(hStickerPicker_)) {
        ShowWindow(hStickerPicker_, SW_HIDE);
        return;
    }

    // Anchor above the compose bar, clamped to the work area.
    RECT btn_rc{};
    if (compose_surface_ && compose_surface_->hwnd())
        GetWindowRect(compose_surface_->hwnd(), &btn_rc);
    else
        GetWindowRect(hwnd_, &btn_rc);
    int x = btn_rc.right - kStickerPickW - 8;
    int y = btn_rc.top   - kStickerPickH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        if (x < mi.rcWork.left)
            x = mi.rcWork.left + 4;
        if (x + kStickerPickW > mi.rcWork.right)
            x = mi.rcWork.right - kStickerPickW - 4;
        if (y < mi.rcWork.top)
            y = btn_rc.bottom + 4;
        if (y + kStickerPickH > mi.rcWork.bottom)
            y = mi.rcWork.bottom - kStickerPickH - 4;
    }

    SetWindowPos(hStickerPicker_, HWND_TOPMOST,
                  x, y, kStickerPickW, kStickerPickH,
                  SWP_NOACTIVATE);

    if (sticker_picker_shared_)       sticker_picker_shared_->refresh_packs();
    if (sticker_picker_search_field_) sticker_picker_search_field_->set_text("");
    if (sticker_picker_shared_)       sticker_picker_shared_->set_search_query("");

    ShowWindow(hStickerPicker_, SW_SHOWNOACTIVATE);
    if (sticker_picker_surface_)      sticker_picker_surface_->relayout();
    if (sticker_picker_search_field_) sticker_picker_search_field_->set_focused(true);
}

void MainWindow::refresh_sticker_picker() {
    if (sticker_picker_shared_) {
        sticker_picker_shared_->refresh_packs();
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_) sticker_picker_surface_->relayout();
}

} // namespace win32
