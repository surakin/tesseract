#include "MainWindow.h"
#include "Win32Notifier.h"
#include "Win32TrayIcon.h"
#include "LoginView.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"

#include <thread>

#include <tesseract/account_session.h>
#include <tesseract/emoji.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>

#include "views/AccountPicker.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <filesystem>
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
// MediaBytesPayload is no longer posted by this shell — media fetches now flow
// through ShellBase::ensure_*_() → post_to_ui_ → on_media_bytes_ready_().
// The struct is kept as a forward stub so any stale queued messages can be
// safely drained in the WM_TESSERACT_MEDIA_BYTES handler.
struct MediaBytesPayload {
    int          kind;       // unused in drain handler
    std::string  cache_key;
    std::vector<std::uint8_t> bytes;
};
struct VideoBytesPayload {
    std::string                 source_json;  // original request key
    std::vector<std::uint8_t>   bytes;
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
        bool has_id = !self->my_display_name_.empty() && !self->my_user_id_.empty();
        auto wname = utf8_to_wstr(shown);
        RECT name_rc{ tx, has_id ? y0 + 7 : y0, tx + text_w,
                      has_id ? y0 + 27 : y0 + h };
        win32::text::draw(hdc, name_rc, wname.c_str(), -1,
            win32::text::Style{
                .family = L"Segoe UI",
                .size   = 10.5f,
                .weight = win32::text::Weight::Bold,
                .color  = pal.text_primary,
                .valign = has_id ? win32::text::VAlign::Top
                                 : win32::text::VAlign::Center,
                .trim   = win32::text::Trim::EllipsisChar,
            });
        if (has_id) {
            auto wid = utf8_to_wstr(self->my_user_id_);
            RECT id_rc{ tx, y0 + 28, tx + text_w, y0 + h - 5 };
            win32::text::draw(hdc, id_rc, wid.c_str(), -1,
                win32::text::Style{
                    .family = L"Segoe UI",
                    .size   = 9.0f,
                    .color  = pal.text_secondary,
                    .trim   = win32::text::Trim::EllipsisChar,
                });
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        self->open_account_picker();
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MainWindow::IDM_ADD_ACCOUNT, L"Add Account…");
        std::wstring logout_label = L"Log Out";
        if (!self->my_display_name_.empty()) {
            logout_label += L" ";
            logout_label += utf8_to_wstr(self->my_display_name_);
        }
        AppendMenuW(menu, MF_STRING, MainWindow::IDM_LOGOUT, logout_label.c_str());
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (x == -1 && y == -1) {
            RECT rc; GetWindowRect(hwnd, &rc);
            x = rc.left + (rc.right - rc.left) / 2;
            y = rc.top  + (rc.bottom - rc.top) / 2;
        }
        UINT pick = TrackPopupMenu(menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD,
            x, y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (pick)
            PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(pick, 0), 0);
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
// EventHandlerBase UI-thread hook overrides (Win32)
// ---------------------------------------------------------------------------
// All marshalling is now handled by EventHandlerBase::post_to_ui_ which posts
// WM_TESSERACT_POST_TO_UI with a heap-allocated std::function<void()>. These
// methods run directly on the UI thread.

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    PostedTimelineReset payload{ std::move(room_id), std::move(snapshot) };
    on_tesseract_timeline_reset(&payload);
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    PostedMessageEvent payload{ std::move(room_id), index, std::move(ev) };
    on_tesseract_message_inserted(&payload);
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    PostedMessageEvent payload{ std::move(room_id), index, std::move(ev) };
    on_tesseract_message_updated(&payload);
}

void MainWindow::handle_message_removed_ui_(
    std::string room_id, std::size_t index)
{
    PostedMessageEvent payload{ std::move(room_id), index, nullptr };
    on_tesseract_message_removed(&payload);
}

void MainWindow::handle_sync_error_ui_(
    std::string context, std::string user_id,
    std::string description, bool soft_logout)
{
    if (context == "sync_reconnect") {
        on_reconnect(user_id);
    } else if (context == "sync_auth_error") {
        on_auth_error(user_id, soft_logout);
    } else {
        MessageBoxA(hwnd_, description.c_str(), "Sync error", MB_ICONWARNING);
    }
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    on_backup_progress(&progress);
}

void MainWindow::handle_image_packs_updated_ui_()
{
    refresh_sticker_picker();
    refresh_emoji_picker();
}

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!verif_surface_ || !verif_surface_->hwnd()) return;
    if (!is_verified && !verification_banner_dismissed_) {
        if (!verif_banner_visible_) {
            if (verif_shared_) verif_shared_->set_state(
                tesseract::views::VerificationBanner::State::Prompt);
            ShowWindow(verif_surface_->hwnd(), SW_SHOW);
            verif_banner_visible_ = true;
            on_size(0, 0);  // re-trigger layout
        }
    } else {
        if (verif_banner_visible_) {
            ShowWindow(verif_surface_->hwnd(), SW_HIDE);
            verif_banner_visible_ = false;
            on_size(0, 0);
        }
    }
}

void MainWindow::handle_verification_request_ui_(
    std::string flow_id, std::string /*user_id*/,
    std::string /*device_id*/, bool incoming)
{
    active_verification_flow_id_ = std::move(flow_id);
    if (!verif_shared_) return;
    if (incoming) {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    } else {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_) client_->start_sas(active_verification_flow_id_);
    }
    if (verif_surface_) verif_surface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!verif_shared_) return;
    verif_shared_->set_emojis(emojis);
    if (verif_surface_ && verif_surface_->hwnd()) {
        SetWindowPos(verif_surface_->hwnd(), nullptr,
                     0, 0, 0, 124,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        verif_surface_->relayout();
        on_size(0, 0);
    }
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    if (!verif_shared_) return;
    verif_shared_->set_state(
        tesseract::views::VerificationBanner::State::Done);
    if (verif_surface_) verif_surface_->relayout();
    if (hwnd_) SetTimer(hwnd_, kVerifDoneTimerId, 1500, nullptr);
}

void MainWindow::handle_verification_cancelled_ui_(
    std::string /*flow_id*/, std::string reason)
{
    if (!verif_shared_) return;
    verif_shared_->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    verif_shared_->set_cancel_reason(std::move(reason));
    if (verif_surface_) verif_surface_->relayout();
}

void MainWindow::handle_account_prefs_updated_ui_(
    std::string /*user_id*/, std::string json)
{
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty() &&
        pending_restore_room_.empty() &&
        current_room_id_.empty())
    {
        pending_restore_room_ = prefs.last_room;
    }
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id,
    std::string room_name, std::string sender,
    std::string body, bool is_mention,
    std::vector<uint8_t> /*avatar_bytes*/)
{
    NotificationPayload p{ std::move(room_id), std::move(room_name),
                           std::move(sender), std::move(body),
                           std::move(user_id), is_mention };
    on_tesseract_notify(&p);
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
}

void MainWindow::update_typing_bar_(const std::string& text)
{
    if (!hTypingBar_) return;
    if (text.empty()) {
        SetWindowTextW(hTypingBar_, L"");
        return;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return;
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                         static_cast<int>(text.size()), wide.data(), len);
    SetWindowTextW(hTypingBar_, wide.c_str());
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
    auto bytes = client_->fetch_avatar_bytes(room_id);
    Gdiplus::Bitmap* bmp = bitmap_from_bytes(bytes);
    avatar_cache_[room_id] = bmp;
    return bmp;
}

Gdiplus::Bitmap* MainWindow::get_user_avatar(const std::string& mxc_url) {
    if (mxc_url.empty()) return nullptr;
    auto it = user_avatar_cache_.find(mxc_url);
    if (it != user_avatar_cache_.end()) return it->second;
    auto bytes = client_->fetch_media_bytes(mxc_url);
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

    case WM_CLOSE:
        if (self->tray_ && self->tray_->is_available() && !self->quitting_) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);  // → WM_DESTROY

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
            self->logout_active_account();
        if (LOWORD(wParam) == IDM_ADD_ACCOUNT)
            self->begin_add_account();
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
    case WM_TESSERACT_RECOVER_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_recover_done(wParam != 0, std::move(*p));
        delete p;
        return 0;
    }
    case WM_TESSERACT_NOTIFY_CLICK: {
        auto* payload = reinterpret_cast<win32::NotifyClickPayload*>(lParam);
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        // Switch to the account that owns this notification before navigating.
        for (int i = 0; i < static_cast<int>(self->accounts_.size()); ++i) {
            if (self->accounts_[i]->user_id == payload->user_id) {
                self->switch_active_account(i);
                break;
            }
        }
        self->navigate_to_room(payload->room_id);
        delete payload;
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
            if (!self->anim_cache_.has(p->cache_key) &&
                self->msg_surface_)
            {
                if (auto img = self->msg_surface_->factory().decode_image(p->bytes))
                    self->tk_images_.emplace(p->cache_key, std::move(img));
            }
            if (self->sticker_picker_shared_)
                self->sticker_picker_shared_->invalidate_image_cache();
            if (self->emoji_picker_shared_)
                self->emoji_picker_shared_->invalidate_image_cache();
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
    case WM_TESSERACT_POST_TO_UI: {
        // ShellBase::post_to_ui_ posts a heap-allocated std::function here.
        // Execute it on the UI thread and free it.
        auto* fn = reinterpret_cast<std::function<void()>*>(lParam);
        (*fn)();
        delete fn;
        return 0;
    }
    case WM_TESSERACT_MEDIA_BYTES: {
        // No longer posted by this shell — media now flows through
        // ShellBase::ensure_*_() → post_to_ui_ → on_media_bytes_ready_().
        // Keep the case to safely drain any stale messages during shutdown.
        auto* p = reinterpret_cast<MediaBytesPayload*>(lParam);
        delete p;
        return 0;
    }
    case WM_TESSERACT_VIDEO_BYTES: {
        auto* p = reinterpret_cast<VideoBytesPayload*>(lParam);
        if (self->vid_viewer_ && !p->bytes.empty())
            self->vid_viewer_->load_bytes(p->bytes.data(), p->bytes.size());
        delete p;
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (self->vid_viewer_ && self->vid_viewer_->is_open()) {
                self->vid_viewer_->close();
                return 0;
            }
            if (self->img_viewer_ && self->img_viewer_->is_open()) {
                self->img_viewer_->close();
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_TIMER:
        if (wParam == kSearchDebounceTimer) {
            KillTimer(hwnd, kSearchDebounceTimer);
            if (self->room_list_view_)
                self->room_list_view_->set_search_text(self->pending_search_text_);
            self->refresh_room_list();
            return 0;
        }
        if (wParam == kAnimTimerId) { self->on_anim_tick(); return 0; }
        if (wParam == kScrollDebounceTimerId) {
            KillTimer(hwnd, kScrollDebounceTimerId);
            if (self->room_list_view_ && self->client_) {
                auto ids = self->room_list_view_->visible_room_ids();
                self->client_->stop_background_backfill();
                self->client_->start_background_backfill(ids);
            }
            return 0;
        }
        if (wParam == kVerifDoneTimerId) {
            KillTimer(hwnd, kVerifDoneTimerId);
            if (self->verif_shared_ && self->verif_shared_->on_done)
                self->verif_shared_->on_done();
            return 0;
        }
        if (wParam == kSyncStatusDebounceTimerId) {
            KillTimer(hwnd, kSyncStatusDebounceTimerId);
            self->sync_status_debounce_timer_id_ = 0;
            using RLS = tesseract::RoomListState;
            if (self->hStatus_
             && (self->last_room_list_state_ == RLS::Init
              || self->last_room_list_state_ == RLS::SettingUp))
            {
                self->sync_progress_shown_ = true;
                SetWindowTextW(self->hStatus_, L"Syncing rooms…");
            }
            return 0;
        }
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
    for (auto& s : accounts_) if (s && s->client) s->client->stop_sync();
    if (pending_login_client_) pending_login_client_->stop_sync();
    // login_view_ calls cancel_oauth() + joins its worker on destruction.
    // Tear it down while the client pointers are still alive.
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
        room_list_view_->on_scroll = [this] {
            KillTimer(hwnd_, kScrollDebounceTimerId);
            SetTimer(hwnd_, kScrollDebounceTimerId, 300, nullptr);
        };
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
        room_list_view_->on_search_clear = [this] {
            KillTimer(hwnd_, kSearchDebounceTimer);
            pending_search_text_.clear();
            room_search_field_->set_text("");
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
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
                if (auto* f = anim_cache_.current_frame(mxc)) return f;
                auto it = tk_images_.find(mxc);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        message_list_view_->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key) {
                if (current_room_id_.empty()) return;
                client_->send_reaction(current_room_id_, event_id, key);
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
        message_list_view_->on_receipt_needed = [this](const std::string& eid) {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        if (auto player = msg_surface_->host().make_audio_player()) {
            message_list_view_->set_audio_player(std::move(player));
        }
        message_list_view_->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t> {
                return client_->fetch_source_bytes(source_json);
            });
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
            auto res = client_->send_message(current_room_id_, trimmed);
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
            auto res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
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
            client_->send_reply(current_room_id_, reply_event_id, body);
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        };
        compose_surface_->set_root(std::move(bar));

        compose_text_area_ = compose_surface_->host().make_text_area();
        compose_text_area_->set_placeholder("Message…");
        compose_text_area_->set_on_changed([this](const std::string& s) {
            handle_compose_text_changed_(s);
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
            const auto limit = client_->media_upload_limit();
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
            auto res = client_->send_file(current_room_id_, bytes, mime,
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
            client_->send_edit(current_room_id_, event_id, new_body);
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

    hTypingBar_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0,
        hwnd, nullptr, hInst_, nullptr);
    apply_default_font(hTypingBar_);

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

    // Verification banner — shared widget on a tk::win32::Surface. Initially
    // hidden; shown by handle_verification_state_ui_ when is_verified=false.
    verif_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::VerificationBanner>();
        verif_shared_ = banner.get();
        verif_shared_->on_verify = [this] {
            if (client_) client_->request_self_verification();
        };
        verif_shared_->on_accept = [this] {
            if (client_) {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_match = [this] {
            if (client_) client_->confirm_sas(active_verification_flow_id_);
            if (verif_shared_) verif_shared_->set_state(
                tesseract::views::VerificationBanner::State::Confirming);
            if (verif_surface_) verif_surface_->relayout();
        };
        verif_shared_->on_mismatch = [this] {
            if (client_) client_->cancel_verification(active_verification_flow_id_);
        };
        verif_shared_->on_cancel = [this] {
            if (client_) client_->cancel_verification(active_verification_flow_id_);
        };
        verif_shared_->on_dismiss = [this] {
            verification_banner_dismissed_ = true;
            ShowWindow(verif_surface_->hwnd(), SW_HIDE);
            verif_banner_visible_ = false;
            on_size(0, 0);
        };
        verif_shared_->on_done = [this] {
            ShowWindow(verif_surface_->hwnd(), SW_HIDE);
            verif_banner_visible_ = false;
            on_size(0, 0);
        };
        verif_surface_->set_root(std::move(banner));
    }
    if (HWND vb = verif_surface_->hwnd()) {
        SetWindowPos(vb, nullptr, 240, kRoomHeaderH, 784, 48,
                      SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(vb, SW_HIDE);
    }

    // Image/sticker lightbox overlay — WS_CHILD Surface that covers the
    // entire content area when open. Created hidden; shown/hidden by
    // on_image_clicked / ImageViewerOverlay::on_close.
    img_viewer_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto viewer = std::make_unique<tesseract::views::ImageViewerOverlay>();
        img_viewer_ = viewer.get();
        img_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                if (auto* f = anim_cache_.current_frame(url)) return f;
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        img_viewer_->on_close = [this] {
            if (img_viewer_surface_ && img_viewer_surface_->hwnd())
                ShowWindow(img_viewer_surface_->hwnd(), SW_HIDE);
        };
        img_viewer_surface_->set_root(std::move(viewer));
    }
    if (HWND iv = img_viewer_surface_->hwnd())
        ShowWindow(iv, SW_HIDE);

    message_list_view_->on_image_clicked =
        [this](const tesseract::views::MessageListView::ImageHit& hit) {
            if (!img_viewer_ || !img_viewer_surface_) return;
            img_viewer_->open(hit.media_url, hit.body,
                               hit.natural_w, hit.natural_h);
            RECT cr; GetClientRect(hwnd_, &cr);
            constexpr int STATUS_H = 24;
            if (HWND iv = img_viewer_surface_->hwnd()) {
                SetWindowPos(iv, HWND_TOP,
                              0, 0, cr.right, cr.bottom - STATUS_H,
                              SWP_NOACTIVATE);
                ShowWindow(iv, SW_SHOWNOACTIVATE);
                SetFocus(iv);
                InvalidateRect(iv, nullptr, FALSE);
            }
        };

    // Video lightbox overlay — WS_CHILD Surface covering the full content area.
    vid_viewer_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    {
        auto vid_viewer = std::make_unique<tesseract::views::VideoViewerOverlay>();
        vid_viewer_ = vid_viewer.get();
        vid_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        vid_viewer_->set_video_player(msg_surface_->host().make_video_player());
        vid_viewer_->set_repaint_requester([this] {
            if (vid_viewer_surface_) vid_viewer_surface_->relayout();
        });
        vid_viewer_->on_close = [this] {
            if (vid_viewer_surface_ && vid_viewer_surface_->hwnd())
                ShowWindow(vid_viewer_surface_->hwnd(), SW_HIDE);
        };
        vid_viewer_surface_->set_root(std::move(vid_viewer));
    }
    if (HWND vv = vid_viewer_surface_->hwnd())
        ShowWindow(vv, SW_HIDE);

    message_list_view_->on_video_clicked =
        [this](const tesseract::views::MessageListView::VideoHit& hit) {
            if (!vid_viewer_ || !vid_viewer_surface_) return;
            vid_viewer_->open(hit.source_json, hit.thumbnail_url, hit.mime_type,
                              hit.duration_ms, hit.natural_w, hit.natural_h,
                              hit.autoplay, hit.loop, hit.no_audio, hit.hide_controls);
            RECT cr; GetClientRect(hwnd_, &cr);
            constexpr int STATUS_H = 24;
            if (HWND vv = vid_viewer_surface_->hwnd()) {
                SetWindowPos(vv, HWND_TOP,
                              0, 0, cr.right, cr.bottom - STATUS_H,
                              SWP_NOACTIVATE);
                ShowWindow(vv, SW_SHOWNOACTIVATE);
                SetFocus(vv);
                InvalidateRect(vv, nullptr, FALSE);
            }
            // Async byte fetch via PostMessage.
            HWND target = hwnd_;
            std::string src = hit.source_json;
            run_async_([this, target, src = std::move(src)]() mutable {
                auto bytes = client_->fetch_source_bytes(src);
                auto* p = new VideoBytesPayload{ src, std::move(bytes) };
                if (!PostMessageW(target, WM_TESSERACT_VIDEO_BYTES, 0,
                                  reinterpret_cast<LPARAM>(p)))
                    delete p;
            });
        };

    message_list_view_->set_video_player_factory(
        [this]() { return msg_surface_->host().make_video_player(); });
    message_list_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready) {
            run_async_([this, src, on_ready = std::move(on_ready)]() mutable {
                auto bytes = client_->fetch_source_bytes(src);
                post_to_ui_([on_ready = std::move(on_ready), bytes = std::move(bytes)]() mutable {
                    on_ready(std::move(bytes));
                });
            });
        });

    login_view_ = std::make_unique<LoginView>(hInst_, hwnd);
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    login_view_->set_on_cancel([this]() { on_login_cancelled(); });
    ShowWindow(login_view_->hwnd(), SW_HIDE);

    start_login();
}

void MainWindow::on_destroy() {
    if (anim_timer_running_ && hwnd_) {
        KillTimer(hwnd_, kAnimTimerId);
        anim_timer_running_ = false;
    }
    // Drain background workers BEFORE tearing any client down.  Each
    // worker calls `client_->fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    shutting_down_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                              [this]{ return workers_in_flight_ == 0; });
    }
    for (auto& s : accounts_) if (s && s->client) s->client->stop_sync();
    if (pending_login_client_) pending_login_client_->stop_sync();
}

// run_async_ is implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h) {
    constexpr int ROOM_W   = 240;
    constexpr int SEP_W    = 1;
    constexpr int CHAT_X   = ROOM_W + SEP_W;
    constexpr int STATUS_H  = 24;
    constexpr int TYPING_H  = 20;

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
    int msg_h = content_h - compose_h - TYPING_H;

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
    if (verif_banner_visible_ && verif_surface_ && verif_surface_->hwnd()) {
        using VBState = tesseract::views::VerificationBanner::State;
        int verif_h = (verif_shared_
                       && verif_shared_->state() == VBState::ShowEmojis) ? 124 : 48;
        SetWindowPos(verif_surface_->hwnd(), nullptr,
                      CHAT_X, msg_area_y, w - CHAT_X, verif_h,
                      SWP_NOZORDER | SWP_NOACTIVATE);
        verif_surface_->relayout();
        msg_area_y += verif_h;
        msg_area_h -= verif_h;
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
    if (hTypingBar_) {
        SetWindowPos(hTypingBar_, nullptr,
                      CHAT_X, msg_h, w - CHAT_X, TYPING_H,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (compose_surface_ && compose_surface_->hwnd()) {
        SetWindowPos(compose_surface_->hwnd(), nullptr,
                      CHAT_X, msg_h + TYPING_H, w - CHAT_X, compose_h,
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Image viewer overlay — keep it sized to the full content area
    // whenever it is visible so it tracks window resizes.
    if (img_viewer_surface_ && img_viewer_surface_->hwnd()
        && IsWindowVisible(img_viewer_surface_->hwnd())) {
        SetWindowPos(img_viewer_surface_->hwnd(), HWND_TOP,
                      0, 0, w, h - STATUS_H, SWP_NOACTIVATE);
    }
    // Video viewer overlay — same full-area tracking.
    if (vid_viewer_surface_ && vid_viewer_surface_->hwnd()
        && IsWindowVisible(vid_viewer_surface_->hwnd())) {
        SetWindowPos(vid_viewer_surface_->hwnd(), HWND_TOP,
                      0, 0, w, h - STATUS_H, SWP_NOACTIVATE);
    }

    SendMessageW(hStatus_, WM_SIZE, 0, 0);
}

// ---------------------------------------------------------------------------
// Login / reconnect
// ---------------------------------------------------------------------------

void MainWindow::start_login() {
    tesseract::SessionStore::migrate_legacy_layout();
    auto index = tesseract::SessionStore::load_index();

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Restoring session…"));

    for (const auto& uid : index.user_ids) {
        auto json = tesseract::SessionStore::load_account(uid);
        if (!json) continue;

        auto sess = std::make_unique<tesseract::AccountSession>();
        sess->client = std::make_unique<tesseract::Client>();
        sess->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());

        auto res = sess->client->restore_session(*json);
        if (!res) {
            tesseract::SessionStore::clear_account(uid);
            continue;
        }
        sess->user_id      = sess->client->get_user_id();
        sess->display_name = sess->client->get_display_name();
        sess->avatar_url   = sess->client->get_avatar_url();
        sess->last_room    =
            tesseract::Prefs::parse(sess->client->load_prefs_json()).last_room;

        auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
        bridge->set_user_id(sess->user_id);
        sess->bridge = std::move(bridge);
        sess->client->start_sync(sess->bridge.get());
        sess->sync_started = true;

        // Per-account notifier: click switches to this account then navigates.
        const std::string uid = sess->user_id;
        sess->notifier = std::make_unique<win32::Win32Notifier>(hwnd_, uid);

        accounts_.push_back(std::move(sess));
    }

    if (accounts_.empty()) {
        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_) {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
        }
        show_login_view();
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Not logged in"));
        return;
    }

    int active_idx = 0;
    if (!index.active_user_id.empty()) {
        for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
            if (accounts_[i]->user_id == index.active_user_id) {
                active_idx = i;
                break;
            }
        }
    }
    switch_active_account(active_idx);
}

void MainWindow::on_login_succeeded() {
    // The pending client ran OAuth into a temp directory.
    // Drop it (releases SQLite handles), rename the temp dir to its final
    // per-account location, then reopen a fresh client at the final path.
    if (!pending_login_client_) return;

    std::string user_id   = pending_login_client_->get_user_id();
    std::string json      = pending_login_client_->export_session();
    pending_login_client_.reset();   // closes SQLite in the temp dir

    namespace fs = std::filesystem;
    fs::path final_dir = tesseract::SessionStore::account_dir(user_id);
    std::error_code ec;
    if (!pending_login_temp_dir_.empty() && fs::exists(pending_login_temp_dir_)) {
        fs::create_directories(final_dir.parent_path(), ec);
        fs::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec) {
            // Rename failed (e.g. cross-device); leave temp dir and bail.
            pending_login_temp_dir_.clear();
            if (login_view_) {
                login_view_->set_status_message(L"Failed to save session.");
                login_view_->reset();
            }
            return;
        }
        pending_login_temp_dir_.clear();
    }

    auto sess = std::make_unique<tesseract::AccountSession>();
    sess->client = std::make_unique<tesseract::Client>();
    sess->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    if (!sess->client->restore_session(json)) {
        tesseract::SessionStore::clear_account(user_id);
        if (login_view_) login_view_->reset();
        return;
    }
    sess->user_id      = sess->client->get_user_id();
    sess->display_name = sess->client->get_display_name();
    sess->avatar_url   = sess->client->get_avatar_url();
    sess->last_room    =
        tesseract::Prefs::parse(sess->client->load_prefs_json()).last_room;

    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(sess->user_id);
    sess->bridge = std::move(bridge);
    sess->client->start_sync(sess->bridge.get());
    sess->sync_started = true;

    // Per-account notifier: click switches to this account then navigates.
    const std::string new_uid = sess->user_id;
    sess->notifier = std::make_unique<win32::Win32Notifier>(hwnd_, new_uid);

    int new_idx = static_cast<int>(accounts_.size());
    accounts_.push_back(std::move(sess));

    // Persist updated index.
    auto idx = tesseract::SessionStore::load_index();
    idx.user_ids.push_back(user_id);
    idx.active_user_id = user_id;
    tesseract::SessionStore::save_index(idx);
    tesseract::SessionStore::save_account(user_id, json);

    switch_active_account(new_idx);
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

void MainWindow::on_reconnect(const std::string& user_id) {
    int idx = -1;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
        if (accounts_[i]->user_id == user_id) { idx = i; break; }
    }
    if (idx < 0) return;

    auto& sess = accounts_[idx];
    sess->client->stop_sync();

    auto json = tesseract::SessionStore::load_account(user_id);
    if (json && sess->client->restore_session(*json)) {
        auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
        bridge->set_user_id(user_id);
        sess->bridge = std::move(bridge);
        if (idx == active_account_index_)
            event_handler_ = sess->bridge.get();
        sess->client->start_sync(sess->bridge.get());
        if (idx == active_account_index_)
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Reconnected"));
    } else {
        if (idx == active_account_index_)
            logout_active_account();
    }
}

void MainWindow::on_auth_error(const std::string& user_id, bool soft_logout) {
    int idx = -1;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
        if (accounts_[i]->user_id == user_id) { idx = i; break; }
    }
    if (idx < 0) return;

    if (soft_logout) {
        auto json = tesseract::SessionStore::load_account(user_id);
        if (json && accounts_[idx]->client->restore_session(*json)) {
            auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
            bridge->set_user_id(user_id);
            accounts_[idx]->bridge = std::move(bridge);
            if (idx == active_account_index_) {
                event_handler_ = accounts_[idx]->bridge.get();
                my_user_id_       = accounts_[idx]->client->get_user_id();
                my_display_name_  = accounts_[idx]->client->get_display_name();
                my_avatar_url_    = accounts_[idx]->client->get_avatar_url();
                populate_user_strip();
                SendMessageW(hStatus_, SB_SETTEXTW, 0,
                             reinterpret_cast<LPARAM>(L"Reconnected"));
                maybe_show_recovery_banner();
            }
            accounts_[idx]->client->start_sync(accounts_[idx]->bridge.get());
            return;
        }
    }
    if (idx == active_account_index_) {
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Session expired; please log in again."));
        logout_active_account();
    } else {
        accounts_[idx]->client->stop_sync();
        tesseract::SessionStore::clear_account(user_id);
        accounts_.erase(accounts_.begin() + idx);
        if (idx < active_account_index_) --active_account_index_;
        auto index = tesseract::SessionStore::load_index();
        index.user_ids.erase(
            std::remove(index.user_ids.begin(), index.user_ids.end(), user_id),
            index.user_ids.end());
        if (index.active_user_id == user_id && active_account_index_ >= 0)
            index.active_user_id = accounts_[active_account_index_]->user_id;
        tesseract::SessionStore::save_index(index);
    }
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

void MainWindow::on_send_clicked() {
    if (compose_shared_) compose_shared_->trigger_send();
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void MainWindow::on_tesseract_notify(const NotificationPayload* p)
{
    // Find which account fired this notification by user_id.
    for (auto& sess : accounts_) {
        if (sess->user_id != p->user_id) continue;
        // Suppress if the window is focused and the active room is the source room
        // for the active account.
        if (GetForegroundWindow() == hwnd_
                && active_account_index_ >= 0
                && accounts_[active_account_index_]->user_id == p->user_id
                && current_room_id_ == p->room_id)
            return;
        if (sess->notifier)
            sess->notifier->notify({ p->room_id, p->room_name,
                                     p->sender, p->body, p->is_mention });
        return;
    }
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty()) return;
    // Select the room in the list view so the sidebar highlights it, then
    // route through on_room_selected which handles subscription + prefs save.
    if (room_list_view_) room_list_view_->set_selected_room(room_id);
    on_room_selected(room_id);
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

    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id)
        client_->unsubscribe_room(current_room_id_);

    current_room_id_ = room_id;
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = room_id;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (compose_shared_) {
        compose_shared_->clear_reply();
        compose_shared_->clear_editing();
    }
    if (compose_text_area_) compose_text_area_->set_text("");
    if (compose_shared_)    compose_shared_->set_current_text({});
    update_typing_bar_({});

    for (const auto& r : rooms_) {
        if (r.id == current_room_id_) {
            current_room_info_ = r;
            update_room_header(r);
            break;
        }
    }
    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the Win32 message pump stays responsive.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    HWND hwnd = hwnd_;
    std::string sub_room = current_room_id_;
    tesseract::Client* cl = client_;
    run_async_([this, sub_room, hwnd, cl, visible_ids = std::move(visible_ids)]{
        auto res = cl->subscribe_room(sub_room);
        bool reached = false;
        if (res) {
            auto pr = cl->paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            cl->start_background_backfill(visible_ids);
        }
        auto* p = new std::string(sub_room);
        PostMessageW(hwnd, WM_TESSERACT_SUBSCRIBE_DONE,
                      static_cast<WPARAM>(reached), reinterpret_cast<LPARAM>(p));
    });
}

void MainWindow::request_more_history(const std::string& room_id) {
    if (room_id.empty()) return;
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    HWND hwnd = hwnd_;
    tesseract::Client* cl = client_;
    run_async_([this, room_id, hwnd, cl]{
        auto pr = cl->paginate_back_with_status(room_id, kPaginationBatch);
        auto* p = new std::string(room_id);
        PostMessageW(hwnd, WM_TESSERACT_PAGINATE_DONE,
                      static_cast<WPARAM>(pr.ok && pr.reached_start),
                      reinterpret_cast<LPARAM>(p));
    });
}

void MainWindow::on_tesseract_paginate_done(std::string* room_id,
                                              bool reached_start) {
    if (!room_id) return;
    push_paginate_result_(*room_id, reached_start);
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
        ensure_reply_details_(ev->in_reply_to_id);
        rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
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
    ensure_reply_details_(payload->event->in_reply_to_id);
    message_list_view_->insert_message(payload->index,
                                         tesseract::views::make_row_data(*payload->event, my_user_id_));
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_message_updated(PostedMessageEvent* payload) {
    if (!payload || !payload->event) return;
    if (payload->room_id != current_room_id_) return;
    if (payload->event->type == tesseract::EventType::Unhandled) return;
    if (!message_list_view_) return;

    ensure_row_media(*payload->event);
    ensure_reply_details_(payload->event->in_reply_to_id);
    message_list_view_->update_message(payload->index,
                                         tesseract::views::make_row_data(*payload->event, my_user_id_));
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_message_removed(PostedMessageEvent* payload) {
    if (!payload) return;
    if (payload->room_id != current_room_id_) return;
    if (!message_list_view_) return;
    message_list_view_->remove_message(payload->index);
    if (msg_surface_) msg_surface_->relayout();
}

void MainWindow::on_tesseract_rooms(RoomsPayload* payload) {
    // push_rooms_ updates per_account_rooms_, rooms_, and calls on_rooms_updated_().
    push_rooms_(std::move(payload->user_id), std::move(payload->rooms));
}

void MainWindow::on_rooms_updated_() {
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
    push_paginate_result_(*room_id, reached_start);
}

void MainWindow::refresh_room_list() {
    if (!room_list_view_) return;

    std::vector<tesseract::RoomInfo> filtered;
    if (space_stack_.empty()) {
        if (hSpaceNavBack_)  ShowWindow(hSpaceNavBack_,  SW_HIDE);
        if (hSpaceNavLabel_) ShowWindow(hSpaceNavLabel_, SW_HIDE);
        if (!pending_search_text_.empty()) {
            for (const auto& r : rooms_) ensure_room_avatar_(r);
            room_list_view_->set_rooms(rooms_);
            if (!current_room_id_.empty())
                room_list_view_->set_selected_room(current_room_id_);
            if (hwnd_) { RECT rc; GetClientRect(hwnd_, &rc); on_size(rc.right, rc.bottom); }
            if (room_surface_) room_surface_->relayout();
            return;
        }
        // Build the root list, excluding rooms that are children of any space
        // (they appear only when drilled in).
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
            if (r.is_space)
                for (const auto& id : client_->space_children(r.id))
                    in_space.insert(id);
        for (const auto& r : rooms_) if (!r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
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
        auto child_ids = client_->space_children(space_stack_.back());
        for (const auto& r : rooms_) {
            if (std::find(child_ids.begin(), child_ids.end(), r.id)
                != child_ids.end()) {
                filtered.push_back(r);
            }
        }
    }
    for (const auto& r : filtered) ensure_room_avatar_(r);

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
// ensure_room_avatar_, ensure_user_avatar_, ensure_media_image_, and
// ensure_reply_details_ are implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Animated media — multi-frame WIC decode + 60 Hz WM_TIMER tick
// ---------------------------------------------------------------------------

void MainWindow::try_load_animation(const std::string& url,
                                      std::span<const std::uint8_t> bytes)
{
    if (url.empty() || bytes.empty()) return;
    if (anim_cache_.has(url)) return;
    auto frames = tk::win32::decode_animation(bytes);
    if (frames.size() < 2) return;

    std::vector<std::unique_ptr<tk::Image>> imgs;
    std::vector<int> delays;
    imgs.reserve(frames.size());
    delays.reserve(frames.size());
    for (auto& af : frames) {
        imgs.push_back(std::move(af.image));
        delays.push_back(af.delay_ms);
    }
    anim_cache_.store(url, std::move(imgs), std::move(delays),
                      static_cast<std::int64_t>(GetTickCount64()));
    // Drop any static-cache leftover from a prior probe.
    tk_images_.erase(url);

    if (!anim_timer_running_ && hwnd_) {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerHz, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::on_anim_tick() {
    if (anim_cache_.empty()) {
        if (anim_timer_running_ && hwnd_) {
            KillTimer(hwnd_, kAnimTimerId);
            anim_timer_running_ = false;
        }
        return;
    }
    const std::int64_t now = static_cast<std::int64_t>(GetTickCount64());
    if (!anim_cache_.advance(now)) return;
    if (msg_surface_ && msg_surface_->hwnd())
        InvalidateRect(msg_surface_->hwnd(), nullptr, FALSE);
    if (sticker_picker_surface_ && sticker_picker_surface_->hwnd() &&
        hStickerPicker_ && IsWindowVisible(hStickerPicker_))
    {
        if (sticker_picker_shared_) sticker_picker_shared_->invalidate_image_cache();
        InvalidateRect(sticker_picker_surface_->hwnd(), nullptr, FALSE);
    }
    if (emoji_picker_surface_ && emoji_picker_surface_->hwnd() &&
        hEmojiPicker_ && IsWindowVisible(hEmojiPicker_))
    {
        if (emoji_picker_shared_) emoji_picker_shared_->invalidate_image_cache();
        InvalidateRect(emoji_picker_surface_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::request_sticker_image(const std::string& cache_key) {
    if (cache_key.empty()) return;
    if (tk_images_.count(cache_key) || anim_cache_.has(cache_key)) return;
    if (!sticker_fetches_in_flight_.insert(cache_key).second) return;

    HWND target = hwnd_;
    run_async_([this, target, cache_key]() {
        auto bytes = client_->fetch_source_bytes(cache_key);
        auto* p    = new StickerBytesPayload{ cache_key, std::move(bytes) };
        if (!PostMessageW(target, WM_TESSERACT_STICKER_BYTES, 0,
                          reinterpret_cast<LPARAM>(p)))
        {
            delete p;   // window already gone; drop the payload.
        }
    });
}

// ---------------------------------------------------------------------------
// ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::post_to_ui_(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(hwnd_, WM_TESSERACT_POST_TO_UI, 0,
                      reinterpret_cast<LPARAM>(p))) {
        delete p;  // window already gone; drop the closure
    }
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                        MediaKind kind,
                                        std::vector<uint8_t> bytes) {
    // Called on the UI thread (via post_to_ui_ → WM_TESSERACT_POST_TO_UI).
    // Decode the bytes, store in the appropriate tk::Image cache, and repaint
    // the relevant surface.
    if (bytes.empty()) return;

    HWND invalidate_hwnd = nullptr;
    switch (kind) {
    case MediaKind::RoomAvatar:
        if (room_surface_) {
            if (auto img = room_surface_->factory().decode_image(bytes))
                tk_avatars_.emplace(cache_key, std::move(img));
            invalidate_hwnd = room_surface_->hwnd();
        }
        // Also invalidate the room header — it uses the GDI+ avatar_cache_
        // which is a separate code-path, but a relayout of room_surface
        // covers the tk-based room list redraw that callers expect.
        if (hRoomHeader_) InvalidateRect(hRoomHeader_, nullptr, FALSE);
        break;
    case MediaKind::UserAvatar:
        if (msg_surface_) {
            if (auto img = msg_surface_->factory().decode_image(bytes))
                tk_avatars_.emplace(cache_key, std::move(img));
            invalidate_hwnd = msg_surface_->hwnd();
        }
        break;
    case MediaKind::MediaImage:
        if (msg_surface_) {
            try_load_animation(cache_key, bytes);
            if (!anim_cache_.has(cache_key)) {
                if (auto img = msg_surface_->factory().decode_image(bytes))
                    tk_images_.emplace(cache_key, std::move(img));
            }
            invalidate_hwnd = msg_surface_->hwnd();
        }
        break;
    }
    if (invalidate_hwnd)
        InvalidateRect(invalidate_hwnd, nullptr, FALSE);
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                            const std::string& /*video_url*/) {
    // Win32 has no GStreamer/AVFoundation pipeline for first-frame extraction.
    // Record the event so the dedup set doesn't retry on every redraw; the
    // MessageListView will show its play-button placeholder over a grey card.
    video_thumb_in_flight_.insert(event_id);
}

// ---------------------------------------------------------------------------
//  Event → MessageRowData + append into the shared MessageListView
// ---------------------------------------------------------------------------

void MainWindow::ensure_row_media(const tesseract::Event& ev) {
    // Delegate to the ShellBase implementation which calls ensure_user_avatar_,
    // ensure_media_image_, generate_video_thumbnail_, etc.
    ensure_row_media_(ev);
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
    if (!client_) return;
    if (recovery_banner_dismissed_) return;
    if (!client_->needs_recovery())  return;
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
    run_async_([this, target, key]() {
        auto res = client_->recover(key);
        WPARAM ok = res.ok ? 1 : 0;
        auto*  p  = new std::wstring(widen_utf8(res.message));
        PostMessageW(target, WM_TESSERACT_RECOVER_DONE,
                     ok, reinterpret_cast<LPARAM>(p));
    });
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
        && !client_->needs_recovery())
    {
        if (recovery_surface_ && recovery_surface_->hwnd())
            ShowWindow(recovery_surface_->hwnd(), SW_HIDE);
        recovery_banner_visible_ = false;
        RECT rc; GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
    }

    last_backup_state_  = progress->state;
    last_imported_keys_ = progress->imported_keys;
    refresh_sync_status();
}

void MainWindow::on_room_list_state(tesseract::RoomListState state) {
    push_room_list_state_(state);
    refresh_sync_status();
}

void MainWindow::refresh_sync_status() {
    if (!hStatus_) return;
    using RLS = tesseract::RoomListState;
    using BS  = tesseract::BackupState;

    const bool room_busy    = (last_room_list_state_ == RLS::Init
                            || last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy    = (last_backup_state_ == BS::Downloading);

    if (room_busy) {
        // Debounce 300 ms so already-warm sessions that flash through
        // Init→Running don't churn the status bar.
        if (!sync_progress_shown_ && sync_status_debounce_timer_id_ == 0) {
            sync_status_debounce_timer_id_ = SetTimer(
                hwnd_, kSyncStatusDebounceTimerId, 300, nullptr);
        } else if (sync_progress_shown_) {
            SetWindowTextW(hStatus_, L"Syncing rooms…");
        }
        return;
    }

    if (sync_status_debounce_timer_id_ != 0) {
        KillTimer(hwnd_, kSyncStatusDebounceTimerId);
        sync_status_debounce_timer_id_ = 0;
    }

    if (reconnecting) {
        sync_progress_shown_ = true;
        SetWindowTextW(hStatus_, L"Reconnecting…");
        return;
    }
    if (keys_busy) {
        sync_progress_shown_ = true;
        wchar_t buf[96];
        swprintf_s(buf, L"Downloading encryption keys (%llu)…",
                   static_cast<unsigned long long>(last_imported_keys_));
        SetWindowTextW(hStatus_, buf);
        return;
    }
    if (sync_progress_shown_) {
        sync_progress_shown_ = false;
        SetWindowTextW(hStatus_, L"Connected");
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
    if (client_ && !my_avatar_url_.empty()) {
        auto bytes = client_->fetch_media_bytes(my_avatar_url_);
        if (!bytes.empty()) user_avatar_bmp_ = bitmap_from_bytes(bytes);
    }
    ShowWindow(hUserStrip_, SW_SHOW);
    InvalidateRect(hUserStrip_, nullptr, FALSE);
    RECT rc; GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

// ---------------------------------------------------------------------------
// Multi-account: switch / add / logout / cancel / picker
// ---------------------------------------------------------------------------

void MainWindow::switch_active_account(int new_idx) {
    if (new_idx < 0 || new_idx >= static_cast<int>(accounts_.size())) return;
    active_account_index_ = new_idx;
    auto& sess = accounts_[new_idx];

    client_        = sess->client.get();
    event_handler_ = sess->bridge.get();

    my_user_id_       = sess->user_id;
    my_display_name_  = sess->display_name;
    my_avatar_url_    = sess->avatar_url;
    pending_restore_room_ = sess->last_room;

    current_room_id_.clear();
    current_room_info_ = tesseract::RoomInfo{};
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();

    // Swap in cached rooms snapshot if available.
    auto it = per_account_rooms_.find(sess->user_id);
    if (it != per_account_rooms_.end())
        rooms_ = it->second;
    else
        rooms_.clear();

    if (room_list_view_)    room_list_view_->set_rooms({});
    if (message_list_view_) message_list_view_->set_messages({});
    if (room_surface_)  room_surface_->relayout();
    if (msg_surface_)   msg_surface_->relayout();

    recovery_banner_visible_   = false;
    recovery_banner_dismissed_ = false;
    if (recovery_surface_ && recovery_surface_->hwnd())
        ShowWindow(recovery_surface_->hwnd(), SW_HIDE);

    populate_user_strip();
    refresh_room_list();

    // Update active_user_id on disk.
    auto idx = tesseract::SessionStore::load_index();
    idx.active_user_id = sess->user_id;
    tesseract::SessionStore::save_index(idx);

    show_main_content();
    SendMessageW(hStatus_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Connected"));
    maybe_show_recovery_banner();

    if (!tray_) {
        tray_ = std::make_unique<Win32TrayIcon>(
            hInst_,
            [this]{ ShowWindow(hwnd_, SW_SHOW);
                     if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
                     SetForegroundWindow(hwnd_); },
            [this]{ if (IsWindowVisible(hwnd_)) { ShowWindow(hwnd_, SW_HIDE); }
                     else { ShowWindow(hwnd_, SW_SHOW);
                            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
                            SetForegroundWindow(hwnd_); } },
            [this]{ quitting_ = true; DestroyWindow(hwnd_); });
    }
}

void MainWindow::begin_add_account() {
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;

    // New client runs OAuth into a unique temp dir.
    std::string ts = std::to_string(
        static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    pending_login_temp_dir_ =
        tesseract::SessionStore::account_dir("pending-" + ts);

    pending_login_client_ = std::make_unique<tesseract::Client>();
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());

    if (login_view_) {
        login_view_->set_client(pending_login_client_.get());
        login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
        login_view_->reset();
    }
    show_login_view();
}

void MainWindow::on_login_cancelled() {
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
        pending_login_temp_dir_.clear();
    }
    pending_login_is_add_account_ = false;

    if (add_account_return_idx_ >= 0 &&
        add_account_return_idx_ < static_cast<int>(accounts_.size()))
    {
        switch_active_account(add_account_return_idx_);
    } else {
        show_login_view();
    }
    add_account_return_idx_ = -1;
}

void MainWindow::logout_active_account() {
    if (active_account_index_ < 0 ||
        active_account_index_ >= static_cast<int>(accounts_.size()))
        return;

    std::string uid = accounts_[active_account_index_]->user_id;
    accounts_[active_account_index_]->client->logout();
    accounts_[active_account_index_]->client->stop_sync();
    accounts_.erase(accounts_.begin() + active_account_index_);

    tesseract::SessionStore::clear_account(uid);
    auto index = tesseract::SessionStore::load_index();
    index.user_ids.erase(
        std::remove(index.user_ids.begin(), index.user_ids.end(), uid),
        index.user_ids.end());

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

    if (accounts_.empty()) {
        client_        = nullptr;
        event_handler_ = nullptr;
        active_account_index_ = -1;
        index.active_user_id.clear();
        tesseract::SessionStore::save_index(index);

        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_) {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
        }
        RECT rc; GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
        show_login_view();
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Signed out"));
    } else {
        int next = std::min(active_account_index_,
                            static_cast<int>(accounts_.size()) - 1);
        active_account_index_ = -1;  // force re-bind
        index.active_user_id = accounts_[next]->user_id;
        tesseract::SessionStore::save_index(index);
        switch_active_account(next);
        SendMessageW(hStatus_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Signed out"));
    }
}

void MainWindow::rebuild_account_picker() {
    if (!hAccountPicker_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = DefWindowProcW;
            wc.hInstance     = hInst_;
            wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = L"TesseractAccountPicker";
            RegisterClassExW(&wc);
            registered = true;
        }
        constexpr int kPickerW = 260;
        constexpr int kRowH    = 56;
        int           kPickerH = kRowH * static_cast<int>(accounts_.size());
        hAccountPicker_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"TesseractAccountPicker", L"",
            WS_POPUP | WS_BORDER,
            0, 0, kPickerW, kPickerH,
            hwnd_, nullptr, hInst_, nullptr);
        if (!hAccountPicker_) return;

        account_picker_surface_ =
            std::make_unique<tk::win32::Surface>(hInst_, hAccountPicker_,
                                                  tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        account_picker_->on_select = [this](const std::string& uid) {
            if (hAccountPicker_) ShowWindow(hAccountPicker_, SW_HIDE);
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                if (accounts_[i]->user_id == uid) { switch_active_account(i); break; }
            }
        };
        account_picker_surface_->set_root(std::move(picker));
        if (HWND s = account_picker_surface_->hwnd()) {
            constexpr int kPickerW = 260;
            constexpr int kRowH    = 56;
            int kPickerH = kRowH * static_cast<int>(accounts_.size());
            SetWindowPos(s, nullptr, 0, 0, kPickerW, kPickerH,
                          SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Rebuild entries.
    std::vector<tesseract::views::AccountEntry> entries;
    for (const auto& s : accounts_) {
        entries.push_back({ s->user_id, s->display_name, s->avatar_url,
                             s->user_id == my_user_id_ });
    }
    if (account_picker_) {
        account_picker_->set_entries(std::move(entries));
        if (account_picker_surface_) account_picker_surface_->relayout();
    }
}

void MainWindow::open_account_picker() {
    if (accounts_.size() < 2) return;
    rebuild_account_picker();
    if (!hAccountPicker_) return;

    constexpr int kPickerW = 260;
    constexpr int kRowH    = 56;
    int kPickerH = kRowH * static_cast<int>(accounts_.size());

    RECT sr{}; GetWindowRect(hUserStrip_, &sr);
    int x = sr.left;
    int y = sr.top - kPickerH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        if (x + kPickerW > mi.rcWork.right)
            x = mi.rcWork.right - kPickerW - 4;
        if (y < mi.rcWork.top) y = sr.bottom + 4;
    }
    SetWindowPos(hAccountPicker_, HWND_TOPMOST,
                 x, y, kPickerW, kPickerH, SWP_NOACTIVATE);
    ShowWindow(hAccountPicker_, SW_SHOWNOACTIVATE);
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
        WS_POPUP,
        0, 0, kEmojiPickW, kEmojiPickH,
        hwnd_, nullptr, hInst_, nullptr);
    if (!hEmojiPicker_) return;

    emoji_picker_surface_ =
        std::make_unique<tk::win32::Surface>(hInst_, hEmojiPicker_,
                                              tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(client_);
    emoji_picker_shared_->on_selected =
        [this](const std::string& glyph) { insert_emoji_at_cursor(glyph); };
    emoji_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto sit = tk_images_.find(cache_key);
            if (sit != tk_images_.end()) return sit->second.get();
            const_cast<MainWindow*>(this)->request_sticker_image(cache_key);
            return nullptr;
        });
    emoji_picker_surface_->set_root(std::move(shared));

    if (HWND s = emoji_picker_surface_->hwnd()) {
        SetWindowPos(s, nullptr, 0, 0, kEmojiPickW, kEmojiPickH,
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
            client_->send_reaction(current_room_id_, ev, glyph);
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
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img) {
            if (!current_room_id_.empty()) {
                const std::string body =
                    img.body.empty() ? img.shortcode : img.body;
                client_->send_sticker(current_room_id_, body,
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
            if (auto* f = anim_cache_.current_frame(cache_key)) return f;
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

void MainWindow::refresh_emoji_picker() {
    if (emoji_picker_shared_) {
        emoji_picker_shared_->refresh_emoticon_packs();
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
}

} // namespace win32
