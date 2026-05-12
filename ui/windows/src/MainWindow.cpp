#include "MainWindow.h"
#include "LoginView.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"

#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/session_store.h>

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
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const auto& pal = theme::palette();

        RECT rc;
        GetClientRect(hwnd, &rc);
        int x0 = rc.left, y0 = rc.top;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Background
        Gdiplus::SolidBrush bg(theme::gpc(pal.chrome_bg));
        g.FillRectangle(&bg, x0, y0, w, h);

        // Bottom border
        Gdiplus::Pen border(theme::gpc(pal.separator), 1.0f);
        g.DrawLine(&border, (float)x0, (float)(rc.bottom - 1), (float)rc.right, (float)(rc.bottom - 1));

        const auto& info = self->current_room_info_;
        if (info.id.empty()) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        // Avatar
        Gdiplus::Bitmap* bmp = self->get_room_avatar(info.id);
        int ax = x0 + 16;
        int ay = y0 + (h - MainWindow::kRoomAvatarSize) / 2;
        if (bmp)
            self->draw_circle_bitmap(g, bmp, ax, ay, MainWindow::kRoomAvatarSize);
        else
            self->draw_initials_circle(g, info.name, ax, ay, MainWindow::kRoomAvatarSize);

        // Name + topic — DirectWrite/D2D so emoji in room names/topics render
        // in color via Segoe UI Emoji's COLR layers.
        int tx = ax + MainWindow::kRoomAvatarSize + 12;
        int text_w = rc.right - tx - 16;

        auto wname = utf8_to_wstr(info.name);
        RECT name_rc{ tx, y0 + 12, tx + text_w, y0 + 12 + 22 };
        win32::text::draw(hdc, name_rc, wname.c_str(), -1,
            win32::text::Style{
                .family = L"Segoe UI",
                .size   = 13.5f,
                .weight = win32::text::Weight::Bold,
                .color  = pal.text_primary,
                .trim   = win32::text::Trim::EllipsisChar,
            });

        // Topic
        if (!info.topic.empty()) {
            auto wtopic = utf8_to_wstr(info.topic);
            RECT topic_rc{ tx, y0 + 34, tx + text_w, y0 + 34 + 18 };
            win32::text::draw(hdc, topic_rc, wtopic.c_str(), -1,
                win32::text::Style{
                    .family = L"Segoe UI",
                    .size   = 10.0f,
                    .color  = pal.text_secondary,
                    .trim   = win32::text::Trim::EllipsisChar,
                });
        }

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
    // Re-apply DarkMode_Explorer scrollbar theming on listboxes / edits.
    theme::apply_control_theme(hRoomList_);
    theme::apply_control_theme(hMsgList_);
    theme::apply_control_theme(hInput_);
    theme::apply_control_theme(hRecoveryKeyEdit_);
    theme::apply_control_theme(hEmojiSearch_);
    theme::apply_control_theme(hEmojiGrid_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (hRoomList_)   InvalidateRect(hRoomList_,   nullptr, TRUE);
    if (hMsgList_)    InvalidateRect(hMsgList_,    nullptr, TRUE);
    if (hRoomHeader_) InvalidateRect(hRoomHeader_, nullptr, TRUE);
    if (hUserStrip_)  InvalidateRect(hUserStrip_,  nullptr, TRUE);
    if (hStatus_)     InvalidateRect(hStatus_,     nullptr, TRUE);
}

void MainWindow::paint_main_background(HDC hdc, const RECT& rc) {
    const auto& pal = theme::palette();
    FillRect(hdc, &rc, theme::brush(pal.window_bg));
}

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_message(tesseract::Event* ev) {
    auto* p = ev;
    PostMessage(hwnd_, WM_TESSERACT_MESSAGE, 0, reinterpret_cast<LPARAM>(p));
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

void EventHandler::on_timeline_reset(const std::string& room_id) {
    auto* p = new std::string(room_id);
    PostMessage(hwnd_, WM_TESSERACT_TIMELINE_RESET, 0, reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_session_saved(const std::string& session_json) {
    tesseract::SessionStore::save(session_json);
}

void EventHandler::on_backup_progress(const tesseract::BackupProgress& progress) {
    auto* p = new tesseract::BackupProgress(progress);
    PostMessage(hwnd_, WM_TESSERACT_BACKUP_PROGRESS, 0,
                reinterpret_cast<LPARAM>(p));
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
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_SEND)
            self->on_send_clicked();
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_EMOJI)
            self->toggle_emoji_picker();
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_RECOVERY_VERIFY)
            self->on_recovery_verify_clicked();
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_RECOVERY_DISMISS)
            self->on_recovery_dismiss_clicked();
        if (LOWORD(wParam) == IDM_LOGOUT)
            self->do_logout();
        if (HIWORD(wParam) == LBN_SELCHANGE && LOWORD(wParam) == IDC_ROOMLIST) {
            int idx = (int)SendMessageW(self->hRoomList_, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR) self->on_room_selected(idx);
        }
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_EMOJI_PICKER_SEARCH)
            self->on_emoji_search_changed();
        return 0;

    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlID == IDC_ROOMLIST) {
            mis->itemHeight = kRoomRowH;
        } else if (mis->CtlID == IDC_MSGLIST) {
            mis->itemHeight = mis->itemID < self->messages_.size()
                ? self->compute_message_height(mis->itemID) : 80;
        } else if (mis->CtlID == IDC_EMOJI_PICKER_GRID) {
            mis->itemHeight = 36;        // one row of glyphs
        } else if (mis->CtlID == IDC_EMOJI_PICKER_TABS) {
            mis->itemHeight = 32;        // tab strip is single-row
            mis->itemWidth  = 32;
        }
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID == IDC_ROOMLIST)
            self->draw_room_item(dis);
        else if (dis->CtlID == IDC_MSGLIST)
            self->draw_message_item(dis);
        else if (dis->CtlID == IDC_EMOJI_PICKER_GRID)
            self->draw_emoji_grid_item(dis);
        else if (dis->CtlID == IDC_EMOJI_PICKER_TABS)
            self->draw_emoji_tab_item(dis);
        else if (dis->CtlID == IDC_SIDE_SEPARATOR) {
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
        if (ctl == self->hRecoveryBanner_ || ctl == self->hRecoveryLabel_) {
            // Faint accent tint for the warning banner, regardless of mode.
            COLORREF banner = (theme::current_mode() == theme::Mode::Dark)
                ? RGB(0x32, 0x2E, 0x1A) : RGB(0xFF, 0xF7, 0xE0);
            SetBkColor(dc, banner);
            return reinterpret_cast<LRESULT>(theme::brush(banner));
        }
        // EDIT controls (compose, recovery key, emoji search) → compose-card bg.
        if (msg == WM_CTLCOLOREDIT) {
            SetBkColor(dc, pal.compose_card_bg);
            return reinterpret_cast<LRESULT>(theme::brush(pal.compose_card_bg));
        }
        // Owner-drawn LB empty-area brush: sidebar-tinted for the room list.
        if (msg == WM_CTLCOLORLISTBOX && ctl == self->hRoomList_) {
            SetBkColor(dc, pal.sidebar_bg);
            return reinterpret_cast<LRESULT>(theme::brush(pal.sidebar_bg));
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

    case WM_TESSERACT_MESSAGE: {
        auto* p = reinterpret_cast<tesseract::Event*>(lParam);
        self->on_tesseract_message(p);
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
        auto* p = reinterpret_cast<std::string*>(lParam);
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

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Input subclass: Enter sends, Shift+Enter inserts newline
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::input_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) {
            reinterpret_cast<MainWindow*>(dwRefData)->on_send_clicked();
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Intercepts WM_LBUTTONDOWN on the owner-drawn message list so a click on a
// reaction chip toggles the reaction instead of just selecting the row.
// Owner-drawn LISTBOX has no widget tree, so we hit-test against the
// per-message `chip_rects` recorded during the last paint of the row.
LRESULT CALLBACK MainWindow::msg_list_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    if (msg == WM_LBUTTONDOWN) {
        auto* self = reinterpret_cast<MainWindow*>(dwRefData);
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = LBItemFromPt(hwnd, pt, FALSE);
        if (idx >= 0 && (size_t)idx < self->messages_.size()) {
            const auto& m = self->messages_[(size_t)idx];
            RECT item_rc;
            if (SendMessageW(hwnd, LB_GETITEMRECT, (WPARAM)idx,
                             (LPARAM)&item_rc) != LB_ERR)
            {
                POINT local{ pt.x - item_rc.left, pt.y - item_rc.top };
                for (const auto& entry : m.chip_rects) {
                    const RECT& chip_rc = entry.first;
                    if (PtInRect(&chip_rc, local)) {
                        auto result = self->client_.send_reaction(
                            m.room_id, m.event_id, entry.second);
                        if (!result.ok && self->hStatus_) {
                            SetWindowTextW(self->hStatus_,
                                utf8_to_wstr("Failed to toggle reaction: "
                                             + result.message).c_str());
                        }
                        return 0; // consume — don't change list selection
                    }
                }
            }
        }
    }
    // Right-click on an own, non-redacted message → "Delete message" popup.
    // We treat WM_RBUTTONUP rather than WM_CONTEXTMENU because the latter
    // arrives in screen coords and we want the hit-test in client coords;
    // either works, but RBUTTONUP keeps the math symmetric with LBUTTONDOWN.
    if (msg == WM_RBUTTONUP) {
        auto* self = reinterpret_cast<MainWindow*>(dwRefData);
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = LBItemFromPt(hwnd, pt, FALSE);
        if (idx >= 0 && (size_t)idx < self->messages_.size()) {
            const auto& m = self->messages_[(size_t)idx];
            const bool is_redacted = (m.type == tesseract::EventType::Redacted);
            if (m.is_own && !is_redacted && !m.event_id.empty()) {
                POINT screen_pt = pt;
                ClientToScreen(hwnd, &screen_pt);
                HMENU menu = CreatePopupMenu();
                constexpr UINT kDeleteId = 1;
                AppendMenuW(menu, MF_STRING, kDeleteId, L"Delete message");
                UINT choice = TrackPopupMenu(menu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                    screen_pt.x, screen_pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (choice == kDeleteId) {
                    int confirm = MessageBoxW(self->hwnd_,
                        L"Delete this message? This cannot be undone.",
                        L"Delete message", MB_YESNO | MB_ICONQUESTION);
                    if (confirm == IDYES) {
                        auto res = self->client_.redact_event(
                            m.room_id, m.event_id, "");
                        if (!res.ok) {
                            MessageBoxW(self->hwnd_,
                                utf8_to_wstr(res.message).c_str(),
                                L"Delete failed", MB_ICONWARNING);
                        }
                    }
                }
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

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
        WS_OVERLAPPEDWINDOW,
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

    // Room list — fixed-height owner-drawn
    hRoomList_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 240, 600,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ROOMLIST)), hInst_, nullptr);
    theme::apply_control_theme(hRoomList_);
    apply_default_font(hRoomList_);

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
        WS_CHILD | WS_VISIBLE,
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

    // Message list — variable-height owner-drawn, not selectable.
    // No CLIENTEDGE: flat surface that blends with the chat area.
    hMsgList_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOSEL,
        240, kRoomHeaderH, 784, 700 - kRoomHeaderH,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MSGLIST)), hInst_, nullptr);
    theme::apply_control_theme(hMsgList_);
    apply_default_font(hMsgList_);

    // Multi-line compose input. Width is shrunk to leave room for the emoji +
    // send buttons that sit inside the compose card to its right.
    hInput_ = CreateWindowExW(
        0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        240, 700, 640, 60,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INPUT)), hInst_, nullptr);
    theme::apply_control_theme(hInput_);
    apply_default_font(hInput_);

    hEmoji_ = CreateWindowExW(
        0, L"BUTTON", L"\xD83D\xDE00",  // 😀 (UTF-16 surrogate pair)
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        884, 700, 36, 60,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EMOJI)), hInst_, nullptr);
    theme::register_button(hEmoji_, theme::ButtonStyle::Icon);

    hSend_ = CreateWindowExW(
        0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        924, 700, 100, 60,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEND)), hInst_, nullptr);
    theme::register_button(hSend_, theme::ButtonStyle::Primary);

    // Picker creation is deferred until first toggle so the cold-start
    // path stays cheap. Recents live in account-data (io.element.recent_emoji),
    // read on demand via client_.recent_emoji_top(...).
    register_emoji_class();

    // Custom flat status strip. Replaces STATUSCLASSNAMEW which carries a 9x
    // size-grip and chunky inset borders.
    register_status_bar_class(hInst_);
    hStatus_ = CreateWindowExW(
        0, L"TesseractStatusBar", L"Not logged in",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        hwnd, nullptr, hInst_, nullptr);

    // Recovery banner (Step 6) — initially hidden; toggled by
    // maybe_show_recovery_banner() after start_sync. Inline recovery: the
    // key edit + Verify button live in the banner itself; no modal dialog.
    hRecoveryBanner_ = CreateWindowExW(
        0, L"STATIC", nullptr,
        WS_CHILD | SS_NOTIFY,
        240, kRoomHeaderH, 784, 30,
        hwnd, nullptr, hInst_, nullptr);
    hRecoveryLabel_ = CreateWindowExW(
        0, L"STATIC", L"Verify this device:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        252, kRoomHeaderH + 8, 140, 18,
        hwnd, nullptr, hInst_, nullptr);
    apply_default_font(hRecoveryLabel_);
    hRecoveryKeyEdit_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        400, kRoomHeaderH + 4, 480, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_KEY)),
        hInst_, nullptr);
    theme::apply_control_theme(hRecoveryKeyEdit_);
    apply_default_font(hRecoveryKeyEdit_);
    hRecoveryVerify_ = CreateWindowExW(
        0, L"BUTTON", L"Verify",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        900, kRoomHeaderH + 4, 80, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_VERIFY)),
        hInst_, nullptr);
    theme::register_button(hRecoveryVerify_, theme::ButtonStyle::Primary);
    hRecoveryDismiss_ = CreateWindowExW(
        0, L"BUTTON", L"✕",  // ✕ — cross / cancel glyph
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        985, kRoomHeaderH + 4, 24, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_DISMISS)),
        hInst_, nullptr);
    theme::register_button(hRecoveryDismiss_, theme::ButtonStyle::Icon);
    ShowWindow(hRecoveryBanner_,  SW_HIDE);
    ShowWindow(hRecoveryLabel_,   SW_HIDE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    ShowWindow(hRecoveryDismiss_, SW_HIDE);

    SetWindowSubclass(hInput_, input_subclass_proc, 0,
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(hMsgList_, msg_list_subclass_proc, 0,
                      reinterpret_cast<DWORD_PTR>(this));

    login_view_ = std::make_unique<LoginView>(hInst_, hwnd, client_);
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    ShowWindow(login_view_->hwnd(), SW_HIDE);

    start_login();
}

void MainWindow::on_destroy() {
    if (hInput_)   RemoveWindowSubclass(hInput_,   input_subclass_proc,    0);
    if (hMsgList_) RemoveWindowSubclass(hMsgList_, msg_list_subclass_proc, 0);
    client_.stop_sync();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h) {
    constexpr int ROOM_W   = 240;
    constexpr int SEP_W    = 1;
    constexpr int CHAT_X   = ROOM_W + SEP_W;
    constexpr int SEND_W   = 100;
    constexpr int INPUT_H  = 60;
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
    int msg_h     = content_h - INPUT_H;

    constexpr int BANNER_H = 30;

    int msg_area_y = 0;
    int msg_area_h = msg_h;
    if (hRoomHeader_ && IsWindowVisible(hRoomHeader_)) {
        msg_area_y = kRoomHeaderH;
        msg_area_h -= kRoomHeaderH;
    }
    if (recovery_banner_visible_) {
        constexpr int LABEL_W = 140;
        SetWindowPos(hRecoveryBanner_, nullptr,
                     CHAT_X, msg_area_y, w - CHAT_X, BANNER_H, SWP_NOZORDER);
        SetWindowPos(hRecoveryLabel_, nullptr,
                     CHAT_X + 12, msg_area_y + 8, LABEL_W, 18, SWP_NOZORDER);
        // Edit fills the gap between label and the right-anchored buttons.
        int edit_x = CHAT_X + 12 + LABEL_W + 8;
        int edit_w = std::max(40, w - edit_x - 124 - 8);
        SetWindowPos(hRecoveryKeyEdit_, nullptr,
                     edit_x, msg_area_y + 4, edit_w, 22, SWP_NOZORDER);
        SetWindowPos(hRecoveryVerify_, nullptr,
                     w - 124, msg_area_y + 4, 80, 22, SWP_NOZORDER);
        SetWindowPos(hRecoveryDismiss_, nullptr,
                     w - 39,  msg_area_y + 4, 24, 22, SWP_NOZORDER);
        msg_area_y += BANNER_H;
        msg_area_h -= BANNER_H;
    }

    bool user_strip_visible = hUserStrip_ && IsWindowVisible(hUserStrip_);
    int room_list_h = user_strip_visible ? msg_h - kUserStripH : msg_h;

    SetWindowPos(hRoomList_, nullptr, 0, 0, ROOM_W, room_list_h, SWP_NOZORDER);
    if (hUserStrip_) {
        SetWindowPos(hUserStrip_, nullptr,
                     0, room_list_h, ROOM_W, kUserStripH, SWP_NOZORDER);
    }
    if (hSideSep_) {
        SetWindowPos(hSideSep_, nullptr, ROOM_W, 0, SEP_W, msg_h, SWP_NOZORDER);
    }
    SetWindowPos(hRoomHeader_, nullptr, CHAT_X, 0, w - CHAT_X, kRoomHeaderH, SWP_NOZORDER);
    SetWindowPos(hMsgList_,  nullptr, CHAT_X, msg_area_y, w - CHAT_X, msg_area_h, SWP_NOZORDER);
    constexpr int EMOJI_W = 40;
    SetWindowPos(hInput_,    nullptr, CHAT_X, msg_h,
                 w - CHAT_X - SEND_W - EMOJI_W, INPUT_H, SWP_NOZORDER);
    SetWindowPos(hEmoji_,    nullptr, w - SEND_W - EMOJI_W, msg_h,
                 EMOJI_W, INPUT_H, SWP_NOZORDER);
    SetWindowPos(hSend_,     nullptr, w - SEND_W, msg_h, SEND_W, INPUT_H, SWP_NOZORDER);
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
    ShowWindow(hRoomList_,        SW_HIDE);
    ShowWindow(hSideSep_,         SW_HIDE);
    ShowWindow(hRoomHeader_,      SW_HIDE);
    ShowWindow(hUserStrip_,       SW_HIDE);
    ShowWindow(hMsgList_,         SW_HIDE);
    ShowWindow(hInput_,           SW_HIDE);
    ShowWindow(hSend_,            SW_HIDE);
    ShowWindow(hEmoji_,           SW_HIDE);
    ShowWindow(hRecoveryBanner_,  SW_HIDE);
    ShowWindow(hRecoveryLabel_,   SW_HIDE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    ShowWindow(hRecoveryDismiss_, SW_HIDE);

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
    ShowWindow(hRoomList_, SW_SHOW);
    ShowWindow(hSideSep_,  SW_SHOW);
    ShowWindow(hMsgList_,  SW_SHOW);
    ShowWindow(hInput_,    SW_SHOW);
    ShowWindow(hSend_,     SW_SHOW);
    ShowWindow(hEmoji_,    SW_SHOW);

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
    if (current_room_id_.empty()) return;

    int len = GetWindowTextLengthW(hInput_);
    if (len <= 0) return;
    std::wstring wbuf(len, L'\0');
    GetWindowTextW(hInput_, &wbuf[0], len + 1);

    // Strip \r, trim trailing whitespace
    std::wstring trimmed;
    trimmed.reserve(wbuf.size());
    for (wchar_t c : wbuf) {
        if (c != L'\r') trimmed += c;
    }
    while (!trimmed.empty() && (trimmed.back() == L'\n' || trimmed.back() == L' '))
        trimmed.pop_back();
    if (trimmed.empty()) return;

    std::string body = wstr_to_utf8(trimmed.c_str());
    if (body.empty()) return;

    auto res = client_.send_message(current_room_id_, body);
    if (res) {
        SetWindowTextW(hInput_, L"");
    } else {
        MessageBoxW(hwnd_, utf8_to_wstr(res.message).c_str(), L"Send failed", MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------------------
// Room selection
// ---------------------------------------------------------------------------

void MainWindow::on_room_selected(int index) {
    if (index < 0 || index >= (int)rooms_.size()) return;

    const std::string new_id = rooms_[index].id;
    if (!current_room_id_.empty() && current_room_id_ != new_id)
        client_.unsubscribe_room(current_room_id_);

    current_room_id_ = new_id;
    current_room_info_ = rooms_[index];
    update_room_header(current_room_info_);
    auto res = client_.subscribe_room(current_room_id_);
    if (res) {
        client_.paginate_back(current_room_id_, 50);
        client_.start_background_backfill();
    }
}

void MainWindow::update_room_header(const tesseract::RoomInfo& info) {
    if (info.id.empty()) {
        ShowWindow(hRoomHeader_, SW_HIDE);
        return;
    }
    ShowWindow(hRoomHeader_, SW_SHOW);
    InvalidateRect(hRoomHeader_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

void MainWindow::on_tesseract_message(tesseract::Event* ev) {
    if (ev->room_id == current_room_id_)
        append_message(*ev);
}

void MainWindow::on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms) {
    rooms_ = *rooms;
    SendMessageW(hRoomList_, LB_RESETCONTENT, 0, 0);
    for (const auto& r : rooms_) {
        auto wname = utf8_to_wstr(r.name);
        SendMessageW(hRoomList_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wname.c_str()));
    }
}

void MainWindow::on_tesseract_timeline_reset(std::string* room_id) {
    if (*room_id == current_room_id_)
        clear_messages();
}

void MainWindow::append_message(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    // If we already have this event (e.g. sender profile resolved via a Set
    // diff, a message edit, or a reaction change), update it in place
    // instead of duplicating. Reactions update on every toggle, so we
    // refresh the full reaction set + invalidate the row to repaint.
    if (!ev.event_id.empty()) {
        for (size_t i = 0; i < messages_.size(); ++i) {
            if (messages_[i].event_id == ev.event_id) {
                messages_[i].body              = ev.body;
                messages_[i].sender_name       = ev.sender_name;
                messages_[i].sender_avatar_url = ev.sender_avatar_url;
                messages_[i].reactions         = ev.reactions;
                messages_[i].chip_rects.clear();
                // Reaction count can change the row height. Re-measure by
                // forcing the listbox to refresh item heights for this row.
                SendMessageW(hMsgList_, LB_SETITEMHEIGHT, (WPARAM)i,
                             MAKELPARAM(compute_message_height(i), 0));
                RECT rc;
                if (SendMessageW(hMsgList_, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&rc) != LB_ERR)
                    InvalidateRect(hMsgList_, &rc, FALSE);
                return;
            }
        }
    }

    MessageData msg;
    msg.event_id          = ev.event_id;
    msg.room_id           = ev.room_id;
    msg.body              = ev.body;
    msg.sender            = ev.sender;
    msg.sender_name       = ev.sender_name;
    msg.sender_avatar_url = ev.sender_avatar_url;
    msg.timestamp         = ev.timestamp;
    msg.is_own            = !my_user_id_.empty() && ev.sender == my_user_id_;
    msg.type              = ev.type;
    msg.reactions         = ev.reactions;
    messages_.push_back(std::move(msg));

    // LB_ADDSTRING triggers WM_MEASUREITEM for the variable-height list
    SendMessageW(hMsgList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));

    // Scroll so the new message is visible
    int count = (int)SendMessageW(hMsgList_, LB_GETCOUNT, 0, 0);
    if (count > 0)
        SendMessageW(hMsgList_, LB_SETTOPINDEX, count - 1, 0);
}

void MainWindow::clear_messages() {
    messages_.clear();
    SendMessageW(hMsgList_, LB_RESETCONTENT, 0, 0);
}

// ---------------------------------------------------------------------------
// Height computation (called from WM_MEASUREITEM)
// ---------------------------------------------------------------------------

int MainWindow::compute_message_height(size_t idx) {
    if (idx >= messages_.size()) return 80;
    const auto& msg = messages_[idx];

    RECT rc{};
    GetClientRect(hMsgList_, &rc);
    int avail_w = rc.right - rc.left;
    if (avail_w < 60) avail_w = 600;

    // Flat-text layout (see docs/UI-PARITY.md): avatar on the left, name +
    // body + footer to the right. No bubble padding.
    int body_max_w = std::min(kMsgMaxWidth,
                              avail_w - kMsgAvatarSize - 3 * tesseract::visual::kMsgAvatarGap);
    if (body_max_w < 60) body_max_w = 60;

    bool redacted = (msg.type == tesseract::EventType::Redacted);
    std::wstring wbody = redacted
        ? std::wstring(L"Message deleted")
        : utf8_to_wstr(msg.body);
    auto body_m = win32::text::measure(wbody.c_str(), -1,
        win32::text::Style{
            .family = L"Segoe UI",
            .size   = 10.0f,
            .slant  = redacted ? win32::text::Slant::Italic
                                : win32::text::Slant::Roman,
            .wrap   = win32::text::Wrap::Word,
        },
        body_max_w);

    int name_h      = tesseract::visual::kMsgSenderNameHeight + 2;   // +2 gap
    int body_h      = body_m.height;
    int reactions_h = msg.reactions.empty() ? 0 : (kReactionH + kReactionPad);
    int ts_h        = tesseract::visual::kMsgTimestampHeight + 2;
    int content_h   = name_h + body_h + reactions_h + ts_h;

    int row_h = kMsgRowPad + std::max(content_h, kMsgAvatarSize) + kMsgRowPad;
    return std::max(row_h, 48);
}

// ---------------------------------------------------------------------------
// Drawing: room list
// ---------------------------------------------------------------------------

void MainWindow::draw_room_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemID >= rooms_.size()) return;
    const auto& room = rooms_[dis->itemID];
    const auto& pal  = theme::palette();

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const RECT& rc = dis->rcItem;
    int x0 = rc.left, y0 = rc.top;
    int w  = rc.right  - rc.left;
    int h  = rc.bottom - rc.top;

    // Background — sidebar tint normally, accent pill when selected.
    bool sel = (dis->itemState & ODS_SELECTED) != 0;
    Gdiplus::SolidBrush bgBrush(theme::gpc(pal.sidebar_bg));
    g.FillRectangle(&bgBrush, x0, y0, w, h);
    if (sel) {
        Gdiplus::SolidBrush selBrush(theme::gpc(pal.sidebar_sel_bg));
        fill_rounded_rect(g, selBrush,
                          (float)x0 + 4.0f, (float)y0 + 2.0f,
                          (float)w - 8.0f, (float)h - 4.0f, 6.0f);
    }

    // Avatar
    int ax = x0 + 8;
    int ay = y0 + (h - kRoomAvatarSize) / 2;
    Gdiplus::Bitmap* bmp = get_room_avatar(room.id);
    if (bmp)
        draw_circle_bitmap(g, bmp, ax, ay, kRoomAvatarSize);
    else
        draw_initials_circle(g, room.name, ax, ay, kRoomAvatarSize);

    // Measure unread badge to know text area width
    float pill_w = 0.0f;
    std::wstring wcount;
    win32::text::Style badge_style{
        .family = L"Segoe UI",
        .size   = 8.0f,
        .weight = win32::text::Weight::Bold,
        .color  = pal.unread_badge_text,
        .halign = win32::text::HAlign::Center,
        .valign = win32::text::VAlign::Center,
    };
    if (room.unread_count > 0) {
        wcount = std::to_wstring(room.unread_count);
        auto bm = win32::text::measure(wcount.c_str(), -1, badge_style);
        pill_w = std::max(20.0f, (float)bm.width + 12.0f);
    }

    // Room name + preview text
    int tx     = ax + kRoomAvatarSize + 10;
    int text_w = rc.right - tx - (int)pill_w - 10;

    auto wname    = utf8_to_wstr(room.name);
    auto wpreview = utf8_to_wstr(room.last_message_body);

    RECT name_rc{ tx, y0 + 10, tx + text_w, y0 + 10 + 20 };
    win32::text::draw(dis->hDC, name_rc, wname.c_str(), -1,
        win32::text::Style{
            .family = L"Segoe UI",
            .size   = 10.0f,
            .weight = win32::text::Weight::Bold,
            .color  = pal.text_primary,
            .trim   = win32::text::Trim::EllipsisChar,
        });

    RECT preview_rc{ tx, y0 + 33, tx + text_w, y0 + 33 + 18 };
    win32::text::draw(dis->hDC, preview_rc, wpreview.c_str(), -1,
        win32::text::Style{
            .family = L"Segoe UI",
            .size   = 9.0f,
            .color  = pal.text_muted,
            .trim   = win32::text::Trim::EllipsisChar,
        });

    // Unread badge pill
    if (pill_w > 0.0f) {
        constexpr float pill_h = 18.0f;
        float pill_x = (float)(rc.right - 8) - pill_w;
        float pill_y = (float)(y0 + (h - (int)pill_h) / 2);

        Gdiplus::SolidBrush badgeBrush(theme::gpc(pal.unread_badge_bg));
        fill_rounded_rect(g, badgeBrush, pill_x, pill_y, pill_w, pill_h, 9.0f);

        RECT badge_rc{
            (LONG)pill_x, (LONG)pill_y,
            (LONG)(pill_x + pill_w), (LONG)(pill_y + pill_h),
        };
        win32::text::draw(dis->hDC, badge_rc, wcount.c_str(), -1, badge_style);
    }
}

// ---------------------------------------------------------------------------
// Drawing: message bubbles
// ---------------------------------------------------------------------------

void MainWindow::draw_message_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemID >= messages_.size()) return;
    const auto& msg = messages_[dis->itemID];
    const auto& pal = theme::palette();
    // Reset chip rects every paint — coordinates are row-relative and
    // become stale on resize / reorder.
    msg.chip_rects.clear();

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const RECT& rc = dis->rcItem;
    int x0 = rc.left, y0 = rc.top;
    int w  = rc.right  - rc.left;
    int h  = rc.bottom - rc.top;

    Gdiplus::SolidBrush bgBrush(theme::gpc(pal.window_bg));
    g.FillRectangle(&bgBrush, x0, y0, w, h);

    bool redacted = (msg.type == tesseract::EventType::Redacted);

    // Flat-text layout — same anatomy on every platform (see UI-PARITY.md):
    // 32 px avatar on the left for everyone, sender name above body, body
    // wraps at kMsgMaxWidth, footer with timestamp on the right.
    int ax = x0 + tesseract::visual::kMsgAvatarGap;
    int bx = ax + kMsgAvatarSize + tesseract::visual::kMsgAvatarGap;
    int body_max_w = std::min(kMsgMaxWidth,
                              w - (bx - x0) - tesseract::visual::kMsgAvatarGap);
    if (body_max_w < 60) body_max_w = 60;

    std::wstring wbody = redacted
        ? std::wstring(L"Message deleted")
        : utf8_to_wstr(msg.body);
    win32::text::Style body_style{
        .family = L"Segoe UI",
        .size   = 10.0f,
        .slant  = redacted ? win32::text::Slant::Italic
                            : win32::text::Slant::Roman,
        .color  = redacted ? pal.text_muted : pal.text_primary,
        .wrap   = win32::text::Wrap::Word,
    };
    auto body_m = win32::text::measure(wbody.c_str(), -1, body_style, body_max_w);
    int body_h = body_m.height;

    int y_cur = y0 + kMsgRowPad;

    // Avatar (always — own messages too).
    const std::string& disp = msg.sender_name.empty() ? msg.sender : msg.sender_name;
    {
        Gdiplus::Bitmap* bmp = get_user_avatar(msg.sender_avatar_url);
        if (bmp)
            draw_circle_bitmap(g, bmp, ax, y_cur, kMsgAvatarSize);
        else
            draw_initials_circle(g, disp, ax, y_cur, kMsgAvatarSize);
    }

    // Sender name (always — both own and other).
    {
        auto wdisp = utf8_to_wstr(disp);
        RECT name_rc{
            bx, y_cur,
            bx + body_max_w,
            y_cur + tesseract::visual::kMsgSenderNameHeight,
        };
        win32::text::draw(dis->hDC, name_rc, wdisp.c_str(), -1,
            win32::text::Style{
                .family = L"Segoe UI",
                .size   = (float)tesseract::visual::kFontSenderName,
                .weight = win32::text::Weight::Bold,
                .color  = pal.text_secondary,
                .trim   = win32::text::Trim::EllipsisChar,
            });
        y_cur += tesseract::visual::kMsgSenderNameHeight + 2;
    }

    // Body text — flat, no bubble. Redacted tombstone uses a dim grey.
    int body_left = bx;
    int body_top  = y_cur;
    {
        RECT body_rc{
            body_left, body_top,
            body_left + body_max_w,
            body_top + body_h,
        };
        win32::text::draw(dis->hDC, body_rc, wbody.c_str(), -1, body_style);
    }
    y_cur += body_h;

    // bubble_x / bubble_bottom retained for the reaction-chip block below.
    int bubble_x = body_left;
    int bubble_bottom = y_cur;

    // Reaction chips: rounded pills beneath the body, aligned with the
    // body's leading edge. Record each chip's screen-space RECT relative
    // to the row origin so WM_LBUTTONDOWN can hit-test against it.
    if (!msg.reactions.empty()) {
        float chip_x = (float)bubble_x;
        float chip_y = (float)(bubble_bottom + kReactionPad);
        for (const auto& r : msg.reactions) {
            std::wstring label = utf8_to_wstr(r.key) + L" " +
                                 std::to_wstring(r.count);
            COLORREF textc = r.reacted_by_me
                ? pal.reaction_chip_text_me
                : pal.reaction_chip_text;
            win32::text::Style chip_style{
                .family = L"Segoe UI",
                .size   = 8.5f,
                .color  = textc,
                .halign = win32::text::HAlign::Center,
                .valign = win32::text::VAlign::Center,
            };
            auto chip_m = win32::text::measure(label.c_str(), -1, chip_style);
            float pill_w = (float)chip_m.width + 14.0f;
            float pill_h = (float)kReactionH;

            Gdiplus::Color fill = r.reacted_by_me
                ? theme::gpc(pal.reaction_chip_bg_me)
                : theme::gpc(pal.reaction_chip_bg);
            Gdiplus::Color border = r.reacted_by_me
                ? theme::gpc(pal.reaction_chip_border_me)
                : theme::gpc(pal.reaction_chip_border);

            Gdiplus::SolidBrush fillBrush(fill);
            fill_rounded_rect(g, fillBrush, chip_x, chip_y,
                              pill_w, pill_h, pill_h / 2.0f);
            Gdiplus::Pen borderPen(border, 1.0f);
            Gdiplus::GraphicsPath path;
            path.AddArc(chip_x,                   chip_y, pill_h, pill_h,  90, 180);
            path.AddArc(chip_x + pill_w - pill_h, chip_y, pill_h, pill_h, 270, 180);
            path.CloseFigure();
            g.DrawPath(&borderPen, &path);

            RECT chip_text_rc{
                (LONG)chip_x, (LONG)chip_y,
                (LONG)(chip_x + pill_w), (LONG)(chip_y + pill_h),
            };
            win32::text::draw(dis->hDC, chip_text_rc, label.c_str(), -1, chip_style);

            // Store row-relative rect for hit-testing.
            RECT chip_rc{
                (LONG)(chip_x         - x0),
                (LONG)(chip_y         - y0),
                (LONG)(chip_x + pill_w - x0),
                (LONG)(chip_y + pill_h - y0),
            };
            msg.chip_rects.emplace_back(chip_rc, r.key);

            chip_x += pill_w + (float)kReactionPad;
        }
    }

    // Timestamp footer: HH:MM, secondary colour, right-aligned beneath the
    // body / reactions row. Matches the Qt and GTK footer style.
    if (msg.timestamp != 0) {
        int footer_y = msg.reactions.empty()
            ? bubble_bottom + 2
            : bubble_bottom + kReactionPad + kReactionH + 2;

        time_t t = static_cast<time_t>(msg.timestamp / 1000);
        struct tm tm_local;
#if defined(_WIN32)
        localtime_s(&tm_local, &t);
#else
        localtime_r(&t, &tm_local);
#endif
        wchar_t ts_buf[8] = {0};
        wcsftime(ts_buf, 8, L"%H:%M", &tm_local);

        int ts_right = body_left + body_max_w;
        RECT ts_rc{
            body_left, footer_y,
            ts_right,
            footer_y + tesseract::visual::kMsgTimestampHeight,
        };
        win32::text::draw(dis->hDC, ts_rc, ts_buf, -1,
            win32::text::Style{
                .family = L"Segoe UI",
                .size   = (float)tesseract::visual::kFontTimestamp,
                .color  = pal.text_muted,
                .halign = win32::text::HAlign::Trailing,
            });
    }
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
    if (!recovery_banner_visible_) {
        // Fresh prompt — restore the input row.
        SetWindowTextW(hRecoveryLabel_, L"Verify this device:");
        SetWindowTextW(hRecoveryKeyEdit_, L"");
        EnableWindow(hRecoveryKeyEdit_, TRUE);
        EnableWindow(hRecoveryVerify_,  TRUE);
        ShowWindow(hRecoveryBanner_,   SW_SHOW);
        ShowWindow(hRecoveryLabel_,    SW_SHOW);
        ShowWindow(hRecoveryKeyEdit_,  SW_SHOW);
        ShowWindow(hRecoveryVerify_,   SW_SHOW);
        ShowWindow(hRecoveryDismiss_,  SW_SHOW);
        // The LoginView is created last (highest z-order) and is normally
        // hidden by show_main_content() before we run, but if its 0×0 →
        // full-rect resize race ever leaves it on top of these controls
        // they become un-hittable. Hoist the interactive banner widgets
        // above every sibling explicitly. Banner STATIC must move first
        // so the edit/buttons end up on top of it.
        constexpr UINT kZFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        SetWindowPos(hRecoveryBanner_,   HWND_TOP, 0, 0, 0, 0, kZFlags);
        SetWindowPos(hRecoveryLabel_,    HWND_TOP, 0, 0, 0, 0, kZFlags);
        SetWindowPos(hRecoveryKeyEdit_,  HWND_TOP, 0, 0, 0, 0, kZFlags);
        SetWindowPos(hRecoveryVerify_,   HWND_TOP, 0, 0, 0, 0, kZFlags);
        SetWindowPos(hRecoveryDismiss_,  HWND_TOP, 0, 0, 0, 0, kZFlags);
        recovery_banner_visible_ = true;
        recovery_in_flight_      = false;
        RECT rc; GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
    }
}

void MainWindow::on_recovery_verify_clicked() {
    std::string key = narrow_edit_utf8(hRecoveryKeyEdit_);
    if (key.empty()) {
        SetWindowTextW(hRecoveryLabel_,
                       L"Please enter a recovery key or passphrase.");
        return;
    }
    EnableWindow(hRecoveryKeyEdit_, FALSE);
    EnableWindow(hRecoveryVerify_,  FALSE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    SetWindowTextW(hRecoveryLabel_, L"Verifying…");
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
        // The backup watcher will repaint into "Importing keys…" and hide
        // the banner once state reaches Enabled.
        SetWindowTextW(hRecoveryLabel_, L"Downloading historical keys…");
        return;
    }
    std::wstring txt = L"Recovery failed: ";
    txt += msg;
    SetWindowTextW(hRecoveryLabel_, txt.c_str());
    EnableWindow(hRecoveryKeyEdit_, TRUE);
    EnableWindow(hRecoveryVerify_,  TRUE);
    ShowWindow(hRecoveryKeyEdit_, SW_SHOW);
    ShowWindow(hRecoveryVerify_,  SW_SHOW);
    SetFocus(hRecoveryKeyEdit_);
    SendMessageW(hRecoveryKeyEdit_, EM_SETSEL, 0, -1);
    recovery_in_flight_ = false;
}

void MainWindow::on_recovery_dismiss_clicked() {
    recovery_banner_dismissed_ = true;
    ShowWindow(hRecoveryBanner_,  SW_HIDE);
    ShowWindow(hRecoveryLabel_,   SW_HIDE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    ShowWindow(hRecoveryDismiss_, SW_HIDE);
    recovery_banner_visible_ = false;
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::on_backup_progress(tesseract::BackupProgress* progress) {
    // Recovery state is populated asynchronously by the first sync cycle, so
    // re-evaluate the banner each time the SDK pings us.
    maybe_show_recovery_banner();

    // Live progress only when the input field is hidden, so we don't clobber
    // "Verify this device:" while the user is typing.
    if (recovery_banner_visible_
        && !IsWindowVisible(hRecoveryKeyEdit_)
        && progress->state == tesseract::BackupState::Downloading
        && progress->imported_keys > 0)
    {
        std::wstring txt = L"Importing keys from backup… "
            + std::to_wstring(progress->imported_keys) + L" imported.";
        SetWindowTextW(hRecoveryLabel_, txt.c_str());
    }
    if (progress->state == tesseract::BackupState::Enabled
        && !client_.needs_recovery())
    {
        ShowWindow(hRecoveryBanner_,  SW_HIDE);
        ShowWindow(hRecoveryLabel_,   SW_HIDE);
        ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
        ShowWindow(hRecoveryVerify_,  SW_HIDE);
        ShowWindow(hRecoveryDismiss_, SW_HIDE);
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
    messages_.clear();
    SendMessageW(hRoomList_, LB_RESETCONTENT, 0, 0);
    SendMessageW(hMsgList_,  LB_RESETCONTENT, 0, 0);
    ShowWindow(hRoomHeader_, SW_HIDE);
    ShowWindow(hUserStrip_,  SW_HIDE);
    ShowWindow(hRecoveryBanner_,  SW_HIDE);
    ShowWindow(hRecoveryLabel_,   SW_HIDE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    ShowWindow(hRecoveryDismiss_, SW_HIDE);
    recovery_banner_visible_   = false;
    recovery_banner_dismissed_ = false;
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
        reinterpret_cast<LPARAM>(res ? L"Signed out" : L"Sign out failed"));

    start_login();
}

// ---------------------------------------------------------------------------
// Emoji picker
// ---------------------------------------------------------------------------

namespace {
constexpr const wchar_t* kEmojiPickerClass = L"TesseractEmojiPicker";
} // namespace

void MainWindow::register_emoji_class() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;  // soft system drop shadow on the popup
    wc.lpfnWndProc   = MainWindow::emoji_picker_wnd_proc;
    wc.hInstance     = hInst_;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;        // painted in WM_ERASEBKGND via theme
    wc.lpszClassName = kEmojiPickerClass;
    RegisterClassExW(&wc);
    registered = true;
}

LRESULT CALLBACK MainWindow::emoji_picker_wnd_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    // Forward owner-draw + measure to the parent's handlers (same controls
    // we declared on the picker live as children of this popup, but their
    // CtlIDs are unique so the main wnd_proc dispatches correctly).
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 theme::brush(theme::palette().chrome_bg));
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto& pal = theme::palette();
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, pal.text_primary);
        SetBkColor(dc, pal.chrome_bg);
        return reinterpret_cast<LRESULT>(theme::brush(pal.chrome_bg));
    }
    case WM_MEASUREITEM:
        return SendMessageW(self->hwnd_, msg, wParam, lParam);
    case WM_DRAWITEM:
        return SendMessageW(self->hwnd_, msg, wParam, lParam);

    case WM_COMMAND: {
        const WORD id   = LOWORD(wParam);
        const WORD code = HIWORD(wParam);
        if (id == IDC_EMOJI_PICKER_SEARCH && code == EN_CHANGE) {
            self->on_emoji_search_changed();
            return 0;
        }
        if (id == IDC_EMOJI_PICKER_GRID && code == LBN_SELCHANGE) {
            int row = (int)SendMessageW(self->hEmojiGrid_, LB_GETCURSEL, 0, 0);
            // Hit-test against last click x to derive the column (kept in
            // a window long via WM_LBUTTONDOWN handler below). Approximate
            // column from a stashed value.
            int col = (int)GetWindowLongPtrW(self->hEmojiGrid_, GWLP_USERDATA);
            self->pick_emoji_at(row, col);
            // Clear the selection so consecutive clicks on the same item refire.
            SendMessageW(self->hEmojiGrid_, LB_SETCURSEL, (WPARAM)-1, 0);
            return 0;
        }
        if (id == IDC_EMOJI_PICKER_TABS && code == LBN_SELCHANGE) {
            int idx = (int)SendMessageW(self->hEmojiTabs_, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR) self->pick_emoji_tab(idx);
            return 0;
        }
        break;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::ensure_emoji_picker_created() {
    if (hEmojiPicker_) return;

    hEmojiPicker_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kEmojiPickerClass, L"",
        WS_POPUP,
        0, 0, kEmojiPickW, kEmojiPickH,
        hwnd_, nullptr, hInst_, this);

    // Round the popup corners on Win11; silently no-ops on older Windows.
    {
        constexpr DWORD kDwmaCorner = 33;
        int corner = 3;  // DWMWCP_ROUNDSMALL
        DwmSetWindowAttribute(hEmojiPicker_, kDwmaCorner, &corner, sizeof(corner));
    }

    // Search box at the top.
    hEmojiSearch_ = CreateWindowExW(
        0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        4, 4, kEmojiPickW - 8, 22,
        hEmojiPicker_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EMOJI_PICKER_SEARCH)),
        hInst_, nullptr);
    theme::apply_control_theme(hEmojiSearch_);

    // Owner-drawn grid in the middle.
    hEmojiGrid_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        4, 30, kEmojiPickW - 8, kEmojiPickH - 30 - 36,
        hEmojiPicker_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EMOJI_PICKER_GRID)),
        hInst_, nullptr);
    theme::apply_control_theme(hEmojiGrid_);

    // Tab strip at the bottom (owner-drawn LISTBOX with horizontal flow).
    hEmojiTabs_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_MULTICOLUMN,
        4, kEmojiPickH - 34, kEmojiPickW - 8, 32,
        hEmojiPicker_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EMOJI_PICKER_TABS)),
        hInst_, nullptr);
    SendMessageW(hEmojiTabs_, LB_SETCOLUMNWIDTH, 32, 0);

    // Subclass the grid so we can capture the click x-coordinate (needed
    // to derive the grid column under WM_LBUTTONDOWN).
    SetWindowSubclass(hEmojiGrid_,
        [](HWND h, UINT m, WPARAM w, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
            if (m == WM_LBUTTONDOWN) {
                int x = GET_X_LPARAM(lp);
                int col = std::min(x / kEmojiCellW, kEmojiCols - 1);
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)col);
            }
            return DefSubclassProc(h, m, w, lp);
        }, 0, 0);

    // Set Segoe UI Emoji 14pt on the search + grid for proper color rendering.
    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Emoji");
    if (font) {
        SendMessageW(hEmojiSearch_, WM_SETFONT, (WPARAM)font, TRUE);
    }

    // Seed the tab strip: ★ + the 8 category glyphs.
    emoji_tabs_.clear();
    emoji_tabs_.push_back(std::string("\xE2\x98\x85"));  // ★
    for (auto c : tesseract::emoji::kCategories)
        emoji_tabs_.push_back(tesseract::emoji::category_tab_glyph(c));
    for (size_t i = 0; i < emoji_tabs_.size(); ++i)
        SendMessageW(hEmojiTabs_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L""));
}

void MainWindow::toggle_emoji_picker() {
    ensure_emoji_picker_created();
    if (IsWindowVisible(hEmojiPicker_)) {
        ShowWindow(hEmojiPicker_, SW_HIDE);
        return;
    }
    // Default to Smileys & People when frequents are empty.
    auto top = client_.recent_emoji_top(1);
    int start_tab = top.empty() ? 1 : 0;
    SetWindowTextW(hEmojiSearch_, L"");
    pick_emoji_tab(start_tab);

    // Position above the emoji button.
    RECT rc{};
    GetWindowRect(hEmoji_, &rc);
    int x = rc.right - kEmojiPickW;
    int y = rc.top - kEmojiPickH - 4;
    SetWindowPos(hEmojiPicker_, HWND_TOPMOST,
                 x, y, kEmojiPickW, kEmojiPickH, SWP_SHOWWINDOW);
    SetFocus(hEmojiSearch_);
}

void MainWindow::on_emoji_search_changed() {
    wchar_t buf[256] = {};
    GetWindowTextW(hEmojiSearch_, buf, 255);
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string q;
    if (n > 1) {
        q.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, q.data(), n, nullptr, nullptr);
    }
    // Trim whitespace.
    auto first = q.find_first_not_of(" \t\r\n");
    auto last  = q.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        // Empty → restore last category tab.
        pick_emoji_tab(emoji_tab_idx_);
        return;
    }
    q = q.substr(first, last - first + 1);
    show_emoji_search_results(q);
}

void MainWindow::pick_emoji_tab(int idx) {
    emoji_tab_idx_ = idx;
    SendMessageW(hEmojiTabs_, LB_SETCURSEL, (WPARAM)idx, 0);
    if (idx == 0) {
        // Frequents — pulled from the SDK's local account-data cache.
        emoji_view_ = client_.recent_emoji_top(24);
    } else if (idx >= 1 && idx <= 8) {
        auto entries = tesseract::emoji::by_category(
            tesseract::emoji::kCategories[idx - 1]);
        emoji_view_.clear();
        emoji_view_.reserve(entries.size());
        for (const auto* e : entries) emoji_view_.emplace_back(e->glyph);
    }
    refresh_emoji_grid();
}

void MainWindow::show_emoji_search_results(const std::string& query) {
    auto results = tesseract::emoji::filter(query);
    emoji_view_.clear();
    emoji_view_.reserve(results.size());
    for (const auto* e : results) emoji_view_.emplace_back(e->glyph);
    SendMessageW(hEmojiTabs_, LB_SETCURSEL, (WPARAM)-1, 0);
    refresh_emoji_grid();
}

void MainWindow::refresh_emoji_grid() {
    SendMessageW(hEmojiGrid_, LB_RESETCONTENT, 0, 0);
    int rows = (static_cast<int>(emoji_view_.size()) + kEmojiCols - 1) / kEmojiCols;
    for (int i = 0; i < rows; ++i) {
        SendMessageW(hEmojiGrid_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L""));
    }
}

void MainWindow::pick_emoji_at(int row, int col) {
    if (row < 0) return;
    size_t idx = static_cast<size_t>(row) * kEmojiCols + col;
    if (idx >= emoji_view_.size()) return;
    insert_emoji_at_cursor(emoji_view_[idx]);
}

void MainWindow::insert_emoji_at_cursor(const std::string& glyph) {
    if (glyph.empty()) return;
    // Convert UTF-8 → UTF-16 then EM_REPLACESEL.
    int n = MultiByteToWideChar(CP_UTF8, 0, glyph.c_str(),
                                static_cast<int>(glyph.size()),
                                nullptr, 0);
    if (n <= 0) return;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, glyph.c_str(),
                        static_cast<int>(glyph.size()), w.data(), n);
    SendMessageW(hInput_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(w.c_str()));
    client_.recent_emoji_bump(glyph);
    SetFocus(hInput_);
}

void MainWindow::draw_emoji_grid_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemAction == ODA_FOCUS) return;
    int row = static_cast<int>(dis->itemID);
    const auto& pal = theme::palette();

    {
        Gdiplus::Graphics g(dis->hDC);
        Gdiplus::SolidBrush bg(theme::gpc(pal.chrome_bg));
        g.FillRectangle(&bg, (INT)dis->rcItem.left, (INT)dis->rcItem.top,
                        (INT)(dis->rcItem.right - dis->rcItem.left),
                        (INT)(dis->rcItem.bottom - dis->rcItem.top));
    }

    // Emoji rendered through DirectWrite + D2D with ENABLE_COLOR_FONT so
    // Segoe UI Emoji's COLR layers paint in full color (GDI+ DrawString
    // could only see the monochrome outline glyphs).
    win32::text::Style style{
        .family = L"Segoe UI Emoji",
        .size   = 18.0f,
        .unit   = win32::text::SizeUnit::Pixel,
        .color  = pal.text_primary,
        .halign = win32::text::HAlign::Center,
        .valign = win32::text::VAlign::Center,
    };
    for (int col = 0; col < kEmojiCols; ++col) {
        size_t idx = static_cast<size_t>(row) * kEmojiCols + col;
        if (idx >= emoji_view_.size()) break;
        const std::string& utf8 = emoji_view_[idx];
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                    (int)utf8.size(), nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                            w.data(), n);
        RECT rc{
            dis->rcItem.left + col * kEmojiCellW,
            dis->rcItem.top,
            dis->rcItem.left + (col + 1) * kEmojiCellW,
            dis->rcItem.bottom,
        };
        win32::text::draw(dis->hDC, rc, w.c_str(), (int)w.size(), style);
    }
}

void MainWindow::draw_emoji_tab_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemAction == ODA_FOCUS) return;
    if (dis->itemID >= emoji_tabs_.size()) return;
    const std::string& utf8 = emoji_tabs_[dis->itemID];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                            w.data(), n);

    const auto& pal = theme::palette();
    {
        Gdiplus::Graphics g(dis->hDC);
        Gdiplus::Color bgc = (dis->itemState & ODS_SELECTED)
            ? theme::gpc(pal.sidebar_sel_bg)
            : theme::gpc(pal.chrome_bg);
        Gdiplus::SolidBrush bg(bgc);
        g.FillRectangle(&bg, (INT)dis->rcItem.left, (INT)dis->rcItem.top,
                        (INT)(dis->rcItem.right - dis->rcItem.left),
                        (INT)(dis->rcItem.bottom - dis->rcItem.top));
    }

    win32::text::draw(dis->hDC, dis->rcItem, w.c_str(), (int)w.size(),
        win32::text::Style{
            .family = L"Segoe UI Emoji",
            .size   = 16.0f,
            .unit   = win32::text::SizeUnit::Pixel,
            .color  = pal.text_primary,
            .halign = win32::text::HAlign::Center,
            .valign = win32::text::VAlign::Center,
        });
}

} // namespace win32
