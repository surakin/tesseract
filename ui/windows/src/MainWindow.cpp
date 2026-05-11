#include "MainWindow.h"
#include "LoginView.h"

#include <thread>

#include <tesseract/session_store.h>

#include <windowsx.h>

#include <algorithm>
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int x0 = rc.left, y0 = rc.top;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Background
        Gdiplus::SolidBrush bg(Gdiplus::Color(0xFFFFFFFF));
        g.FillRectangle(&bg, x0, y0, w, h);

        // Bottom border
        Gdiplus::Pen border(Gdiplus::Color(0xFFD0D3D8), 1.0f);
        g.DrawLine(&border, (float)x0, (float)(rc.bottom - 1), (float)rc.right, (float)(rc.bottom - 1));

        const auto& info = self->current_room_info_;
        if (info.id.empty()) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        // Avatar
        Gdiplus::Bitmap* bmp = self->get_room_avatar(info.id);
        int ax = x0 + 16;
        int ay = y0 + (h - kRoomAvatarSize) / 2;
        if (bmp)
            self->draw_circle_bitmap(g, bmp, ax, ay, kRoomAvatarSize);
        else
            self->draw_initials_circle(g, info.name, ax, ay, kRoomAvatarSize);

        // Name
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font nameFont(&ff, 15.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush nameBrush(Gdiplus::Color(0xFF111111));
        Gdiplus::StringFormat sf;
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        int tx = ax + kRoomAvatarSize + 12;
        int text_w = rc.right - tx - 16;

        auto wname = utf8_to_wstr(info.name);
        g.DrawString(wname.c_str(), -1, &nameFont,
                     Gdiplus::RectF((float)tx, (float)(y0 + 14), (float)text_w, 22.0f),
                     &sf, &nameBrush);

        // Topic
        if (!info.topic.empty()) {
            Gdiplus::Font topicFont(&ff, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush topicBrush(Gdiplus::Color(0xFF65676B));
            auto wtopic = utf8_to_wstr(info.topic);
            g.DrawString(wtopic.c_str(), -1, &topicFont,
                         Gdiplus::RectF((float)tx, (float)(y0 + 34), (float)text_w, 18.0f),
                         &sf, &topicBrush);
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int x0 = rc.left, y0 = rc.top;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Background
        Gdiplus::SolidBrush bg(Gdiplus::Color(0xFFE8EAEE));
        g.FillRectangle(&bg, x0, y0, w, h);

        // Top border
        Gdiplus::Pen border(Gdiplus::Color(0xFFD0D3D8), 1.0f);
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

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font nameFont(&ff, 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush nameBrush(Gdiplus::Color(0xFF111111));
        Gdiplus::StringFormat sf;
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        int tx = ax + AVATAR + 10;
        int text_w = rc.right - tx - 8;
        auto wname = utf8_to_wstr(shown);
        g.DrawString(wname.c_str(), -1, &nameFont,
                     Gdiplus::RectF((float)tx, (float)y0, (float)text_w, (float)h),
                     &sf, &nameBrush);

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
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, (float)size * 0.38f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(0xFFFFFFFF));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(init, 1, &font, Gdiplus::RectF((float)x, (float)y, (float)size, (float)size),
                 &sf, &white);
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
        return 0;

    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlID == IDC_ROOMLIST) {
            mis->itemHeight = kRoomRowH;
        } else if (mis->CtlID == IDC_MSGLIST) {
            mis->itemHeight = mis->itemID < self->messages_.size()
                ? self->compute_message_height(mis->itemID) : 80;
        }
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID == IDC_ROOMLIST)
            self->draw_room_item(dis);
        else if (dis->CtlID == IDC_MSGLIST)
            self->draw_message_item(dis);
        return TRUE;
    }

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

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool MainWindow::register_class(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWindow::wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
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

    // Room list — fixed-height owner-drawn
    hRoomList_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 240, 600,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ROOMLIST)), hInst_, nullptr);

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

    // Message list — variable-height owner-drawn, not selectable
    hMsgList_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOSEL,
        240, kRoomHeaderH, 784, 700 - kRoomHeaderH,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MSGLIST)), hInst_, nullptr);

    // Multi-line compose input
    hInput_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        240, 700, 684, 60,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INPUT)), hInst_, nullptr);

    hSend_ = CreateWindowExW(
        0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        924, 700, 100, 60,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEND)), hInst_, nullptr);

    hStatus_ = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"Not logged in",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
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
    hRecoveryKeyEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        400, kRoomHeaderH + 4, 480, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_KEY)),
        hInst_, nullptr);
    hRecoveryVerify_ = CreateWindowExW(
        0, L"BUTTON", L"Verify",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
        900, kRoomHeaderH + 4, 80, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_VERIFY)),
        hInst_, nullptr);
    hRecoveryDismiss_ = CreateWindowExW(
        0, L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        985, kRoomHeaderH + 4, 24, 22,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECOVERY_DISMISS)),
        hInst_, nullptr);
    ShowWindow(hRecoveryBanner_,  SW_HIDE);
    ShowWindow(hRecoveryLabel_,   SW_HIDE);
    ShowWindow(hRecoveryKeyEdit_, SW_HIDE);
    ShowWindow(hRecoveryVerify_,  SW_HIDE);
    ShowWindow(hRecoveryDismiss_, SW_HIDE);

    SetWindowSubclass(hInput_, input_subclass_proc, 0,
                      reinterpret_cast<DWORD_PTR>(this));

    login_view_ = std::make_unique<LoginView>(hInst_, hwnd, client_);
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    ShowWindow(login_view_->hwnd(), SW_HIDE);

    start_login();
}

void MainWindow::on_destroy() {
    if (hInput_) RemoveWindowSubclass(hInput_, input_subclass_proc, 0);
    client_.stop_sync();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h) {
    constexpr int ROOM_W   = 240;
    constexpr int SEND_W   = 100;
    constexpr int INPUT_H  = 60;
    constexpr int STATUS_H = 22;

    // Status bar always sized.
    SendMessageW(hStatus_, WM_SIZE, 0, 0);

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
                     ROOM_W, msg_area_y, w - ROOM_W, BANNER_H, SWP_NOZORDER);
        SetWindowPos(hRecoveryLabel_, nullptr,
                     ROOM_W + 12, msg_area_y + 8, LABEL_W, 18, SWP_NOZORDER);
        // Edit fills the gap between label and the right-anchored buttons.
        int edit_x = ROOM_W + 12 + LABEL_W + 8;
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
    SetWindowPos(hRoomHeader_, nullptr, ROOM_W, 0, w - ROOM_W, kRoomHeaderH, SWP_NOZORDER);
    SetWindowPos(hMsgList_,  nullptr, ROOM_W, msg_area_y, w - ROOM_W, msg_area_h, SWP_NOZORDER);
    SetWindowPos(hInput_,    nullptr, ROOM_W, msg_h, w - ROOM_W - SEND_W, INPUT_H, SWP_NOZORDER);
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
    ShowWindow(hRoomHeader_,      SW_HIDE);
    ShowWindow(hUserStrip_,       SW_HIDE);
    ShowWindow(hMsgList_,         SW_HIDE);
    ShowWindow(hInput_,           SW_HIDE);
    ShowWindow(hSend_,            SW_HIDE);
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
    ShowWindow(hMsgList_,  SW_SHOW);
    ShowWindow(hInput_,    SW_SHOW);
    ShowWindow(hSend_,     SW_SHOW);

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
    if (res) client_.paginate_back(current_room_id_, 50);
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
    // diff, or a message edit), update it in place instead of duplicating.
    if (!ev.event_id.empty()) {
        for (size_t i = 0; i < messages_.size(); ++i) {
            if (messages_[i].event_id == ev.event_id) {
                messages_[i].body              = ev.body;
                messages_[i].sender_name       = ev.sender_name;
                messages_[i].sender_avatar_url = ev.sender_avatar_url;
                RECT rc;
                if (SendMessageW(hMsgList_, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&rc) != LB_ERR)
                    InvalidateRect(hMsgList_, &rc, FALSE);
                return;
            }
        }
    }

    MessageData msg;
    msg.event_id          = ev.event_id;
    msg.body              = ev.body;
    msg.sender            = ev.sender;
    msg.sender_name       = ev.sender_name;
    msg.sender_avatar_url = ev.sender_avatar_url;
    msg.timestamp         = ev.timestamp;
    msg.is_own            = !my_user_id_.empty() && ev.sender == my_user_id_;
    msg.type              = ev.type;
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

    int bubble_max_w = std::min(kMaxBubbleWidth, (int)(avail_w * 0.70f));
    int text_w       = bubble_max_w - 2 * kBubblePadX;

    Gdiplus::RectF bound;
    HDC hdc = GetDC(hMsgList_);
    {
        // Inner scope: Graphics is destroyed before ReleaseDC below.
        Gdiplus::Graphics g(hdc);
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font bodyFont(&ff, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        auto wbody = utf8_to_wstr(msg.body);
        Gdiplus::StringFormat sf;
        Gdiplus::RectF layout(0, 0, (float)text_w, 4096.0f);
        g.MeasureString(wbody.c_str(), -1, &bodyFont, layout, &sf, &bound);
    }
    ReleaseDC(hMsgList_, hdc);

    int name_h  = msg.is_own ? 0 : 20;
    int body_h  = (int)bound.Height + 2 * kBubblePadY;
    int row_h   = kMsgRowPad + name_h + std::max(body_h, kMsgAvatarSize) + kMsgRowPad;
    return std::max(row_h, 48);
}

// ---------------------------------------------------------------------------
// Drawing: room list
// ---------------------------------------------------------------------------

void MainWindow::draw_room_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemID >= rooms_.size()) return;
    const auto& room = rooms_[dis->itemID];

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const RECT& rc = dis->rcItem;
    int x0 = rc.left, y0 = rc.top;
    int w  = rc.right  - rc.left;
    int h  = rc.bottom - rc.top;

    // Background
    bool sel = (dis->itemState & ODS_SELECTED) != 0;
    Gdiplus::SolidBrush bgBrush(sel ? Gdiplus::Color(0xFFDCEAFF) : Gdiplus::Color(0xFFFFFFFF));
    g.FillRectangle(&bgBrush, x0, y0, w, h);

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
    if (room.unread_count > 0) {
        wcount = std::to_wstring(room.unread_count);
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font badgeFont(&ff, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::StringFormat sf;
        Gdiplus::RectF bounds;
        g.MeasureString(wcount.c_str(), -1, &badgeFont, Gdiplus::PointF{}, &sf, &bounds);
        pill_w = std::max(20.0f, bounds.Width + 12.0f);
    }

    // Room name + preview text
    int tx     = ax + kRoomAvatarSize + 10;
    int text_w = rc.right - tx - (int)pill_w - 10;

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font nameFont   (&ff, 10.0f, Gdiplus::FontStyleBold,    Gdiplus::UnitPoint);
    Gdiplus::Font previewFont(&ff,  9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush nameBrush   (Gdiplus::Color(0xFF1A1A2E));
    Gdiplus::SolidBrush previewBrush(Gdiplus::Color(0xFF888888));

    Gdiplus::StringFormat sf;
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    auto wname    = utf8_to_wstr(room.name);
    auto wpreview = utf8_to_wstr(room.last_message_body);

    g.DrawString(wname.c_str(), -1, &nameFont,
                 Gdiplus::RectF((float)tx, (float)(y0 + 10), (float)text_w, 20.0f),
                 &sf, &nameBrush);
    g.DrawString(wpreview.c_str(), -1, &previewFont,
                 Gdiplus::RectF((float)tx, (float)(y0 + 33), (float)text_w, 18.0f),
                 &sf, &previewBrush);

    // Unread badge pill
    if (pill_w > 0.0f) {
        constexpr float pill_h = 18.0f;
        float pill_x = (float)(rc.right - 8) - pill_w;
        float pill_y = (float)(y0 + (h - (int)pill_h) / 2);

        Gdiplus::SolidBrush badgeBrush(Gdiplus::Color(0xFF0084FF));
        fill_rounded_rect(g, badgeBrush, pill_x, pill_y, pill_w, pill_h, 9.0f);

        Gdiplus::Font badgeFont(&ff, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush wBrush(Gdiplus::Color(0xFFFFFFFF));
        Gdiplus::StringFormat center;
        center.SetAlignment(Gdiplus::StringAlignmentCenter);
        center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(wcount.c_str(), -1, &badgeFont,
                     Gdiplus::RectF(pill_x, pill_y, pill_w, pill_h), &center, &wBrush);
    }

    // Separator line
    Gdiplus::Pen sep(Gdiplus::Color(0xFFEEEEEE), 1.0f);
    g.DrawLine(&sep, (float)x0, (float)(rc.bottom - 1), (float)rc.right, (float)(rc.bottom - 1));
}

// ---------------------------------------------------------------------------
// Drawing: message bubbles
// ---------------------------------------------------------------------------

void MainWindow::draw_message_item(DRAWITEMSTRUCT* dis) {
    if (dis->itemID >= messages_.size()) return;
    const auto& msg = messages_[dis->itemID];

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const RECT& rc = dis->rcItem;
    int x0 = rc.left, y0 = rc.top;
    int w  = rc.right  - rc.left;
    int h  = rc.bottom - rc.top;

    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(0xFFF5F5F5));
    g.FillRectangle(&bgBrush, x0, y0, w, h);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font bodyFont(&ff, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

    int bubble_max_w = std::min(kMaxBubbleWidth, (int)(w * 0.70f));
    int text_max_w   = bubble_max_w - 2 * kBubblePadX;

    auto wbody = utf8_to_wstr(msg.body);
    Gdiplus::StringFormat sfWrap;
    Gdiplus::RectF layout(0, 0, (float)text_max_w, 4096.0f);
    Gdiplus::RectF bound;
    g.MeasureString(wbody.c_str(), -1, &bodyFont, layout, &sfWrap, &bound);

    int body_h  = (int)bound.Height + 2 * kBubblePadY;
    int bubble_w = std::min((int)bound.Width + 2 * kBubblePadX + 4, bubble_max_w);
    int y_cur   = y0 + kMsgRowPad;

    if (msg.is_own) {
        int bx = rc.right - 8 - bubble_w;
        int by = y_cur;

        Gdiplus::SolidBrush bubbleBrush(Gdiplus::Color(0xFF0084FF));
        fill_rounded_rect(g, bubbleBrush, (float)bx, (float)by,
                          (float)bubble_w, (float)body_h, (float)kBubbleRadius);

        Gdiplus::SolidBrush textBrush(Gdiplus::Color(0xFFFFFFFF));
        g.DrawString(wbody.c_str(), -1, &bodyFont,
                     Gdiplus::RectF((float)(bx + kBubblePadX), (float)(by + kBubblePadY),
                                    (float)(bubble_w - 2*kBubblePadX),
                                    (float)(body_h  - 2*kBubblePadY)),
                     &sfWrap, &textBrush);
    } else {
        int ax = x0 + 8;

        // Sender name
        const std::string& disp = msg.sender_name.empty() ? msg.sender : msg.sender_name;
        Gdiplus::Font nameFont(&ff, 9.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush nameBrush(Gdiplus::Color(0xFF555555));
        Gdiplus::StringFormat sfNoWrap;
        sfNoWrap.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        sfNoWrap.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        g.DrawString(utf8_to_wstr(disp).c_str(), -1, &nameFont,
                     Gdiplus::RectF((float)(ax + kMsgAvatarSize + 8), (float)y_cur,
                                    300.0f, 18.0f),
                     &sfNoWrap, &nameBrush);
        y_cur += 20;

        // Avatar
        Gdiplus::Bitmap* bmp = get_user_avatar(msg.sender_avatar_url);
        if (bmp)
            draw_circle_bitmap(g, bmp, ax, y_cur, kMsgAvatarSize);
        else
            draw_initials_circle(g, disp, ax, y_cur, kMsgAvatarSize);

        // Gray bubble
        int bx = ax + kMsgAvatarSize + 8;
        int by = y_cur;

        Gdiplus::SolidBrush bubbleBrush(Gdiplus::Color(0xFFE4E6EB));
        fill_rounded_rect(g, bubbleBrush, (float)bx, (float)by,
                          (float)bubble_w, (float)body_h, (float)kBubbleRadius);

        Gdiplus::SolidBrush textBrush(Gdiplus::Color(0xFF1A1A2E));
        g.DrawString(wbody.c_str(), -1, &bodyFont,
                     Gdiplus::RectF((float)(bx + kBubblePadX), (float)(by + kBubblePadY),
                                    (float)(bubble_w - 2*kBubblePadX),
                                    (float)(body_h  - 2*kBubblePadY)),
                     &sfWrap, &textBrush);
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

} // namespace win32
