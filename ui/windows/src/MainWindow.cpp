#include "MainWindow.h"
#include "RoomWindow.h"
#include "views/BrandView.h"
#include "Win32Notifier.h"
#include "Win32ScreenLock.h"
#include "Win32TrayIcon.h"
#include "LoginView.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"
#include "app/SlashCommands.h"

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
#include <dwmapi.h>
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
struct VideoBytesPayload
{
    std::string source_json; // original request key
    std::vector<std::uint8_t> bytes;
};
struct FileBytesPayload
{
    std::string dest_path;
    std::vector<uint8_t> bytes;
};
} // namespace

// ---------------------------------------------------------------------------
// Status-bar WndProc — flat custom-painted strip replacing the comctl32
// STATUSCLASSNAMEW (which carries a 9x-style size grip and chunky borders).
// Stores the latest text via WM_SETTEXT / SB_SETTEXTW so existing callers
// using either message continue to work.
// ---------------------------------------------------------------------------

LRESULT CALLBACK status_bar_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCREATE:
        SetPropW(hwnd, L"TesseractStatusText", nullptr);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NCDESTROY:
    {
        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        delete p;
        RemovePropW(hwnd, L"TesseractStatusText");
        return DefWindowProcW(hwnd, msg, wParam, lParam);
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

        auto* p =
            static_cast<std::wstring*>(GetPropW(hwnd, L"TesseractStatusText"));
        if (p && !p->empty())
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, pal.text_secondary);
            HFONT old =
                (HFONT)SelectObject(hdc, theme::font(theme::FontRole::Small));
            RECT text_rc = {rc.left + 10, rc.top, rc.right - 10, rc.bottom};
            DrawTextW(hdc, p->c_str(), -1, &text_rc,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            SelectObject(hdc, old);
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
    }
    if (main_app_surface_)
    {
        main_app_surface_->set_theme(current_theme_);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(current_theme_);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(current_theme_);
    }
    if (slash_popup_surface_)
    {
        slash_popup_surface_->set_theme(current_theme_);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(current_theme_);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(current_theme_);
    }
    if (join_room_surface_)
    {
        join_room_surface_->set_theme(current_theme_);
    }
    if (account_picker_surface_)
    {
        account_picker_surface_->set_theme(current_theme_);
    }
    if (settings_surface_)
    {
        settings_surface_->set_theme(current_theme_);
    }
    if (login_view_)
    {
        login_view_->set_theme(current_theme_);
    }

    // Pop-out room windows track the theme too.
    apply_theme_to_secondary_windows_(current_theme_);
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

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    tesseract::EventList snapshot)
{
    PostedTimelineReset payload{std::move(room_id), std::move(snapshot)};
    on_tesseract_timeline_reset(&payload);
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    PostedMessageEvent payload{std::move(room_id), index, std::move(ev)};
    on_tesseract_message_inserted(&payload);
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    PostedMessageEvent payload{std::move(room_id), index, std::move(ev)};
    on_tesseract_message_updated(&payload);
}

void MainWindow::handle_message_removed_ui_(std::string room_id,
                                            std::size_t index)
{
    PostedMessageEvent payload{std::move(room_id), index, nullptr};
    on_tesseract_message_removed(&payload);
}

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                       std::string description,
                                       bool soft_logout)
{
    if (context == "sync_reconnect")
    {
        on_reconnect(user_id);
    }
    else if (context == "sync_auth_error")
    {
        on_auth_error(user_id, soft_logout);
    }
    else
    {
        MessageBoxW(hwnd_, utf8_to_wstr(description).c_str(), L"Sync error",
                    MB_ICONWARNING);
    }
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
    if (!is_verified && !verification_banner_dismissed_)
    {
        if (!verif_banner_visible_)
        {
            if (verif_shared_)
            {
                verif_shared_->set_state(
                    tesseract::views::VerificationBanner::State::Prompt);
            }
            // Verification takes priority — hide the recovery banner if it
            // appeared before the verification state callback arrived.
            // But if recovery is actively in progress (Verifying/Importing), let
            // it finish rather than interrupting with the verification banner.
            if (recovery_banner_visible_ && recovery_shared_)
            {
                auto rs = recovery_shared_->state();
                if (rs == tesseract::views::RecoveryBanner::State::Form ||
                    rs == tesseract::views::RecoveryBanner::State::Failed)
                {
                    if (recovery_key_chosen_)
                    {
                        return;
                    }
                    main_app_->show_recovery_banner(false);
                    recovery_banner_visible_ = false;
                }
                else
                {
                    return;
                }
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

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
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
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            self->on_size(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

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
        return 0;
    }

    case WM_COMMAND:
        // Compose bar Send / Emoji + recovery banner clicks go through
        // the shared widgets' callbacks now — no WM_COMMAND wiring. The
        // emoji-picker search field is a NativeTextField overlay handled
        // by its set_on_changed lambda. Only the logout / add-account
        // items posted by show_user_context_menu_ remain.
        if (LOWORD(wParam) == IDM_SETTINGS)
        {
            self->open_settings_();
        }
        if (LOWORD(wParam) == IDM_LOGOUT)
        {
            self->logout_active_account();
        }
        if (LOWORD(wParam) == IDM_ADD_ACCOUNT)
        {
            self->begin_add_account();
        }
        if (LOWORD(wParam) == IDM_QUIT)
        {
            self->quitting_ = true;
            DestroyWindow(hwnd);
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
        // colours on the EDIT controls + recovery STATIC labels.
        const auto& pal = theme::palette();
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        // The recovery banner and compose bar are now tk::win32::Surfaces —
        // they paint their own backgrounds; no WM_CTLCOLOR tinting needed.
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
    case WM_TESSERACT_SUBSCRIBE_DONE:
    {
        auto* p = reinterpret_cast<std::string*>(lParam);
        self->on_tesseract_subscribe_done(p, wParam != 0);
        delete p;
        return 0;
    }
    case WM_TESSERACT_JUMP_DONE:
    {
        auto* p = reinterpret_cast<JumpDonePayload*>(lParam);
        self->on_tesseract_jump_done(p);
        delete p;
        return 0;
    }
    case WM_TESSERACT_RECOVER_DONE:
    {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_recover_done(wParam != 0, std::move(*p));
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
        for (int i = 0; i < static_cast<int>(self->accounts_.size()); ++i)
        {
            if (self->accounts_[i]->user_id == payload->user_id)
            {
                self->switch_active_account(i);
                break;
            }
        }
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
    case WM_TESSERACT_VIDEO_BYTES:
    {
        auto* p = reinterpret_cast<VideoBytesPayload*>(lParam);
        if (self->vid_viewer_ && !p->bytes.empty())
        {
            self->vid_viewer_->load_bytes(p->bytes.data(), p->bytes.size());
        }
        delete p;
        return 0;
    }
    case WM_TESSERACT_FILE_BYTES:
    {
        auto* p = reinterpret_cast<FileBytesPayload*>(lParam);
        if (p && !p->bytes.empty())
        {
            std::ofstream f(p->dest_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(p->bytes.data()),
                    static_cast<std::streamsize>(p->bytes.size()));
        }
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
            if (self->vid_viewer_ && self->vid_viewer_->is_open())
            {
                self->vid_viewer_->close();
                return 0;
            }
            if (self->img_viewer_ && self->img_viewer_->is_open())
            {
                self->img_viewer_->close();
                return 0;
            }
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
        if (wParam == kScrollDebounceTimerId)
        {
            KillTimer(hwnd, kScrollDebounceTimerId);
            if (self->room_list_view_ && self->client_)
            {
                auto ids = self->room_list_view_->visible_room_ids();
                self->client_->stop_background_backfill();
                self->client_->start_background_backfill(ids);
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

MainWindow::MainWindow(HINSTANCE hInst) : hInst_(hInst)
{
    set_screen_lock_(std::make_unique<win32::Win32ScreenLock>(hInst));
}

MainWindow::~MainWindow()
{
    for (auto& s : accounts_)
    {
        if (s && s->client)
        {
            s->client->stop_sync();
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->stop_sync();
    }
    // login_view_ calls cancel_oauth() + joins its worker on destruction.
    // Tear it down while the client pointers are still alive.
    login_view_.reset();
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
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::on_create(HWND hwnd)
{

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
        auto mainAppRoot = std::make_unique<tesseract::views::MainAppWidget>();
        main_app_ = mainAppRoot.get();

        main_app_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hwnd, tk::Theme::light());
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
        recovery_shared_ = main_app_->recovery_banner();
        verif_shared_ = main_app_->verif_banner();
        img_viewer_ = main_app_->image_viewer();
        vid_viewer_ = main_app_->video_viewer();

        // Space nav callback.
        main_app_->on_space_back = [this]
        {
            on_space_back();
        };

        tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

        // TabBar callbacks.
        main_app_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            tab_select_room(room_id);
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
            POINT sp{static_cast<LONG>(p.x), static_cast<LONG>(p.y)};
            if (main_app_surface_ && main_app_surface_->hwnd())
            {
                ClientToScreen(main_app_surface_->hwnd(), &sp);
            }
            show_user_context_menu_(sp.x, sp.y);
        };

        room_list_view_->on_room_selected = [this](const std::string& room_id)
        {
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
            if (room_search_field_)
            {
                room_search_field_->set_text("");
            }
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
        room_list_view_->on_join_room_requested = [this]
        {
            open_join_room_dialog();
        };

        // ── RoomView shortcode wiring (avatar/image/preview via helper) ────
        room_view_->set_shortcode_provider(
            [this](const std::string& mxc) -> std::string
            {
                return shortcode_for_mxc_(mxc);
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
                return client_->fetch_source_bytes(source_json);
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
                room_text_area_ ? room_text_area_->mention_draft()
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
            auto res = tesseract::dispatch_compose_send(
                *client_, current_room_id_, msg.body, msg.formatted_body);
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
                MessageBoxW(hwnd_, utf8_to_wstr(res.message).c_str(),
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
        room_view_->on_reply_focus = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_focused(true);
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
        room_view_->on_link_clicked = [](const std::string& url)
        {
            tesseract::Client::open_in_browser(url);
        };
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
        room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
        {
            if (!main_app_surface_)
            {
                return;
            }
            if (!hTopicTooltip_)
            {
                hTopicTooltip_ = CreateWindowExW(
                    WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(hTopicTooltip_, TTM_SETMAXTIPWIDTH, 0, 400);
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
                ti.hwnd = hwnd_;
                ti.uId = 1;
                ti.lpszText = const_cast<LPWSTR>(L"");
                SendMessageW(hTopicTooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
            int wlen =
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            topic_tooltip_text_.resize(static_cast<std::size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1,
                                topic_tooltip_text_.data(), wlen);
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = hwnd_;
            ti.uId = 1;
            ti.lpszText = topic_tooltip_text_.data();
            SendMessageW(hTopicTooltip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            POINT pt{static_cast<LONG>(anchor.x),
                     static_cast<LONG>(anchor.y + anchor.h)};
            ClientToScreen(main_app_surface_->hwnd(), &pt);
            SendMessageW(
                hTopicTooltip_, TTM_TRACKPOSITION, 0,
                MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y)));
            SendMessageW(hTopicTooltip_, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
        };
        room_view_->on_hide_tooltip = [this]
        {
            if (!hTopicTooltip_)
            {
                return;
            }
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = hwnd_;
            ti.uId = 1;
            SendMessageW(hTopicTooltip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
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
            run_async_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        room_view_->on_jump_to_date_requested = [this]
        {
            openJumpToDateDialog();
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
            [this](const std::string& body, const std::string& formatted)
        {
            on_thread_send_requested(body, formatted);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_thread_send_reply =
            [this](const std::string& reply_id,
                   const std::string& body,
                   const std::string& formatted)
        {
            on_thread_send_reply_requested(reply_id, body, formatted);
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
                    post_to_ui_(
                        [this, members = std::move(members)]() mutable
                        {
                            if (main_app_)
                            {
                                for (const auto& m : members)
                                    ensure_user_avatar_(m.avatar_url);
                                main_app_->room_view()->set_room_members(
                                    std::move(members));
                            }
                        });
                });
        };
        room_view_->on_save_topic = [this](std::string room_id, std::string topic)
        {
            auto* c = client_;
            run_async_(
                [c, room_id = std::move(room_id), topic = std::move(topic)]()
                {
                    c->set_room_topic(room_id, topic);
                });
        };
        room_view_->on_leave_room = [this](std::string room_id)
        {
            auto* c = client_;
            if (!c) return;
            run_async_(
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
        setup_dm_callbacks();
        room_view_->on_ignore_user = [this](std::string user_id)
        {
            auto* c = client_;
            run_async_(
                [c, user_id = std::move(user_id)]()
                {
                    c->ignore_user(user_id);
                });
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
                if (!room_view_ || !client_)
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
                POINT sp{static_cast<LONG>(p.x), static_cast<LONG>(p.y)};
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
        main_app_surface_->set_on_file_drop(
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename)
            {
                if (!room_view_)
                {
                    return;
                }
                const auto limit = client_->media_upload_limit();
                if (limit > 0 && bytes.size() > limit)
                {
                    if (hStatus_)
                    {
                        SetWindowTextW(hStatus_, L"File exceeds server limit");
                    }
                    return;
                }
                if (bytes.empty()) return;
                auto* cb = room_view_->compose_bar();
                if (mime == "image/gif" || mime == "image/webp")
                {
                    // Show first frame immediately; detect animation in background.
                    cb->set_pending_image(bytes, mime, filename,
                                         /*is_animated=*/false);
                    auto gen = cb->pending_gen();
                    extract_media_info_(gen, std::move(bytes), std::move(mime));
                }
                else if (mime.rfind("image/", 0) == 0)
                {
                    cb->set_pending_image(std::move(bytes), std::move(mime),
                                         std::move(filename), /*is_animated=*/false);
                }
                else if (mime.rfind("video/", 0) == 0)
                {
                    cb->set_pending_video(bytes, mime, filename);
                    auto gen = cb->pending_gen();
                    extract_media_info_(gen, std::move(bytes), std::move(mime));
                }
                else if (mime.rfind("audio/", 0) == 0)
                {
                    cb->set_pending_audio(bytes, mime, filename);
                    auto gen = cb->pending_gen();
                    extract_media_info_(gen, std::move(bytes), std::move(mime));
                }
                else
                {
                    cb->set_pending_file(std::move(bytes), std::move(mime),
                                         std::move(filename));
                }
            });

        // ── Native overlays ──────────────────────────────────────────────────
        room_search_field_ = main_app_surface_->host().make_text_field();
        room_search_field_->set_placeholder("Search");
        room_search_field_->set_visible(false);
        room_search_field_->set_on_changed(
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

        room_text_area_ = main_app_surface_->host().make_text_area();
        room_text_area_->set_placeholder("Message…");
        room_text_area_->set_on_changed(
            [this](const std::string& s)
            {
                handle_compose_text_changed_(s);
                if (room_view_)
                {
                    room_view_->set_current_text(s);
                }

                // ── Slash-command popup ─────────────────────────────────────────
                int cursor = room_text_area_->cursor_byte_pos();

                {
                    auto m = slash_engine_.find_prefix(s, cursor);
                    if (m.has_value())
                    {
                        auto items = slash_engine_.lookup(m->prefix);
                        if (items.empty())
                        {
                            hide_slash_popup_();
                        }
                        else
                        {
                            // Only hide other popovers if they are actually
                            // visible — these hide functions reset
                            // set_on_popup_nav(nullptr), and calling them
                            // unconditionally on every text-change tick kills
                            // the slash popup's nav handler.
                            if (shortcode_popup_visible_())
                                hide_shortcode_popup_();
                            if (mention_popup_visible_())
                                hide_mention_popup_();
                            show_slash_popup_(items,
                                              room_text_area_->cursor_rect());
                            // Reinstall the nav handler unconditionally on
                            // every tick: hide_*_popup_ calls above (when they
                            // did fire) wipe it, and installing
                            // unconditionally avoids the "first keystroke
                            // kills nav" bug.
                            room_text_area_->set_on_popup_nav(
                                [this](tk::NativeTextArea::NavKey nk) -> bool
                                {
                                    if (!slash_popup_visible_())
                                    {
                                        return false;
                                    }
                                    int cur =
                                        slash_popup_widget_->selected_index();
                                    int n = slash_popup_widget_->visible_rows();
                                    if (n <= 0)
                                    {
                                        return true;
                                    }
                                    int next = cur;
                                    switch (nk)
                                    {
                                    case tk::NativeTextArea::NavKey::Up:
                                        next = std::max(0, cur - 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Down:
                                        next = std::min(n - 1, cur + 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Tab:
                                    {
                                        int sel =
                                            slash_popup_widget_->selected_index();
                                        if (sel >= 0 &&
                                            sel < slash_popup_widget_
                                                      ->visible_rows() &&
                                            slash_popup_widget_->on_accepted)
                                        {
                                            slash_popup_widget_->on_accepted(
                                                slash_popup_widget_
                                                    ->suggestion_at(sel));
                                        }
                                        else
                                        {
                                            hide_slash_popup_();
                                        }
                                        return true;
                                    }
                                    case tk::NativeTextArea::NavKey::ShiftTab:
                                        return false;
                                    case tk::NativeTextArea::NavKey::Escape:
                                        hide_slash_popup_();
                                        return true;
                                    }
                                    slash_popup_widget_->set_selected_index(
                                        next);
                                    slash_popup_surface_->host()
                                        .request_repaint();
                                    return true;
                                });
                        }
                        return;
                    }
                    if (slash_popup_visible_())
                    {
                        hide_slash_popup_();
                    }
                }

                // ── Shortcode detection ─────────────────────────────────────────

                auto complete = shortcode_engine_.find_complete(s, cursor);
                if (complete)
                {
                    auto hits = shortcode_engine_.lookup(complete->prefix,
                                                         cached_emoticons_, 1);
                    std::string r =
                        (!hits.empty() && !hits.front().glyph.empty())
                            ? hits.front().glyph
                            : ":" + complete->prefix + ":";
                    room_text_area_->replace_range(complete->start,
                                                   complete->end, r);
                    hide_shortcode_popup_();
                    hide_mention_popup_();
                    return;
                }

                auto prefix_match = shortcode_engine_.find_prefix(s, cursor);
                if (prefix_match && prefix_match->prefix.size() >= 2)
                {
                    hide_mention_popup_();
                    shortcode_current_suggestions_ = shortcode_engine_.lookup(
                        prefix_match->prefix, cached_emoticons_);
                    if (!shortcode_current_suggestions_.empty())
                    {
                        shortcode_active_match_ = *prefix_match;
                        for (const auto& sugg : shortcode_current_suggestions_)
                        {
                            if (!sugg.emoticon.url.empty())
                            {
                                ensure_media_image_(sugg.emoticon.url, 28, 28);
                            }
                        }
                        bool was_visible = shortcode_popup_visible_();
                        show_shortcode_popup_(shortcode_current_suggestions_,
                                              room_text_area_->cursor_rect());
                        if (!was_visible)
                        {
                            room_text_area_->set_on_popup_nav(
                                [this](tk::NativeTextArea::NavKey nk) -> bool
                                {
                                    if (!shortcode_popup_visible_())
                                    {
                                        return false;
                                    }
                                    int cur = shortcode_popup_widget_
                                                  ->selected_index();
                                    int n =
                                        shortcode_popup_widget_->visible_rows();
                                    if (n <= 0)
                                    {
                                        return true;
                                    }
                                    int next = cur;
                                    switch (nk)
                                    {
                                    case tk::NativeTextArea::NavKey::Up:
                                        next = std::max(0, cur - 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Down:
                                        next = std::min(n - 1, cur + 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Tab:
                                    {
                                        int sel = shortcode_popup_widget_
                                                      ->selected_index();
                                        if (sel >= 0 &&
                                            sel <
                                                (int)
                                                    shortcode_current_suggestions_
                                                        .size())
                                        {
                                            auto& s =
                                                shortcode_current_suggestions_
                                                    [sel];
                                            std::string r =
                                                s.glyph.empty()
                                                    ? ":" + s.shortcode + ":"
                                                    : s.glyph;
                                            room_text_area_->replace_range(
                                                shortcode_active_match_.start,
                                                shortcode_active_match_.end, r);
                                        }
                                        hide_shortcode_popup_();
                                        return true;
                                    }
                                    case tk::NativeTextArea::NavKey::ShiftTab:
                                        return false;
                                    case tk::NativeTextArea::NavKey::Escape:
                                        hide_shortcode_popup_();
                                        return true;
                                    }
                                    shortcode_popup_widget_->set_selected_index(
                                        next);
                                    shortcode_popup_surface_->host()
                                        .request_repaint();
                                    return true;
                                });
                        }
                        return;
                    }
                }
                // ── @mention popup ──────────────────────────────────────────
                if (mention_controller_ &&
                    mention_controller_->on_text_changed(
                        s, room_text_area_->cursor_byte_pos()))
                {
                    return;
                }
                hide_slash_popup_();
                hide_shortcode_popup_();
                hide_mention_popup_();
                // ── End shortcode / slash detection ────────────────────────────
            });
        room_text_area_->set_on_submit(
            [this]
            {
                if (slash_popup_visible_())
                {
                    int sel = slash_popup_widget_->selected_index();
                    if (sel >= 0 && sel < slash_popup_widget_->visible_rows() &&
                        slash_popup_widget_->on_accepted)
                    {
                        slash_popup_widget_->on_accepted(
                            slash_popup_widget_->suggestion_at(sel));
                        return;
                    }
                    hide_slash_popup_();
                }
                if (mention_controller_ && mention_controller_->on_submit())
                {
                    return;
                }
                if (shortcode_popup_visible_())
                {
                    int sel = shortcode_popup_widget_->selected_index();
                    if (sel >= 0 &&
                        sel < (int)shortcode_current_suggestions_.size())
                    {
                        auto& s = shortcode_current_suggestions_[sel];
                        std::string r =
                            s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
                        room_text_area_->replace_range(
                            shortcode_active_match_.start,
                            shortcode_active_match_.end, r);
                        hide_shortcode_popup_();
                        return;
                    }
                    hide_shortcode_popup_();
                }
                on_send_clicked();
            });
        room_text_area_->set_on_edit_last(
            [this]
            {
                return room_view_ && room_view_->edit_last_own();
            });
        room_text_area_->set_on_height_changed(
            [this](float h)
            {
                if (room_view_)
                {
                    room_view_->set_text_area_natural_height(h);
                }
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            });
        room_text_area_->set_on_image_paste(
            [this](std::vector<std::uint8_t> bytes, std::string mime)
            {
                if (room_view_)
                {
                    room_view_->compose_bar()->set_pending_image(
                        std::move(bytes), std::move(mime));
                }
            });

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
            hooks.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            hooks.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            mention_controller_ =
                std::make_unique<tesseract::views::MentionController>(
                    room_text_area_.get(), client_, mention_popup_widget_,
                    std::move(hooks));
        }

        topic_text_area_ = main_app_surface_->host().make_text_area();
        topic_text_area_->set_on_changed(
            [this](const std::string& t)
            {
                if (main_app_)
                    main_app_->room_view()->set_topic_edit_text(t);
            });
        topic_text_area_->set_visible(false);

        recovery_key_field_ = main_app_surface_->host().make_text_field();
        recovery_key_field_->set_placeholder("Recovery key or passphrase");
        recovery_key_field_->set_password(true);
        recovery_key_field_->set_on_changed(
            [this](const std::string& k)
            {
                if (recovery_shared_)
                {
                    recovery_shared_->set_current_key(k);
                }
            });
        recovery_key_field_->set_on_submit(
            [this]
            {
                on_recovery_verify_clicked();
            });

        // ── RecoveryBanner callbacks ─────────────────────────────────────────
        recovery_shared_->on_verify = [this](const std::string& /*key*/)
        {
            on_recovery_verify_clicked();
        };
        recovery_shared_->on_dismiss = [this]
        {
            on_recovery_dismiss_clicked();
        };

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
            recovery_key_chosen_ = true;
            if (!recovery_banner_dismissed_ && main_app_ && recovery_shared_)
            {
                recovery_shared_->set_state(
                    tesseract::views::RecoveryBanner::State::Form);
                recovery_shared_->set_current_key("");
                if (recovery_key_field_)
                {
                    recovery_key_field_->set_text("");
                    recovery_key_field_->set_enabled(true);
                }
                main_app_->show_recovery_banner(true);
                recovery_banner_visible_ = true;
            }
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
            HWND target = hwnd_;
            run_async_(
                [this, target, source_url = std::move(source_url), path]()
                {
                    auto bytes = client_->fetch_source_bytes(source_url);
                    auto* p = new FileBytesPayload{
                        wstr_to_utf8(path.c_str()), std::move(bytes)};
                    if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                                      reinterpret_cast<LPARAM>(p)))
                        delete p;
                });
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
            ensure_media_image_(src_tok, tesseract::visual::kMaxInlineImageWidth,
                                tesseract::visual::kMaxInlineImageHeight);
        };

        // Avatar click → open the lightbox with the original avatar mxc.
        // Overrides the thumbnail-only wiring from
        // ShellBase::wire_main_app_widget_ so ensure_media_image_ fetches
        // the native-resolution bytes into tk_images_; the viewer's
        // image_provider prefers that over the resized tk_avatars_ entry.
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
            ensure_media_image_(url, tesseract::visual::kMaxInlineImageWidth,
                                tesseract::visual::kMaxInlineImageHeight);
        };

        vid_viewer_->on_save =
            [this](std::string source_json, std::string mime_type)
        {
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            std::wstring suggested(("video" + ext).begin(),
                                   ("video" + ext).end());
            std::wstring path = show_save_dialog_(
                suggested,
                L"Videos\0*.mp4;*.webm;*.mkv\0All files\0*.*\0\0");
            if (path.empty())
                return;
            HWND target = hwnd_;
            run_async_(
                [this, target, source_json = std::move(source_json), path]()
                {
                    auto bytes = client_->fetch_source_bytes(source_json);
                    auto* p = new FileBytesPayload{
                        wstr_to_utf8(path.c_str()), std::move(bytes)};
                    if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                                      reinterpret_cast<LPARAM>(p)))
                        delete p;
                });
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
                              hit.autoplay, hit.loop, hit.no_audio,
                              hit.hide_controls);
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
            // Async byte fetch via PostMessage.
            HWND target = hwnd_;
            std::string src = src_tok;
            run_async_(
                [this, target, src = std::move(src)]() mutable
                {
                    auto bytes = client_->fetch_source_bytes(src);
                    auto* p = new VideoBytesPayload{src, std::move(bytes)};
                    if (!PostMessageW(target, WM_TESSERACT_VIDEO_BYTES, 0,
                                      reinterpret_cast<LPARAM>(p)))
                    {
                        delete p;
                    }
                });
        };

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
            HWND target = hwnd_;
            std::string url = hit.source ? hit.source->fetch_token() : std::string{};
            run_async_(
                [this, target, url, path]()
                {
                    auto bytes = client_->fetch_source_bytes(url);
                    auto* p = new FileBytesPayload{
                        wstr_to_utf8(path.c_str()), std::move(bytes)};
                    if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                                      reinterpret_cast<LPARAM>(p)))
                        delete p;
                });
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
                run_async_(
                    [this, src, on_ready = std::move(on_ready)]() mutable
                    {
                        auto bytes = client_->fetch_source_bytes(src);
                        post_to_ui_(
                            [on_ready = std::move(on_ready),
                             bytes = std::move(bytes)]() mutable
                            {
                                on_ready(std::move(bytes));
                            });
                    });
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
                                  (vid_viewer_ && vid_viewer_->is_open());

                // Compose text area. Hidden while a viewer is open or while
                // voice recording is active (rect is empty in that case).
                if (room_text_area_)
                {
                    const tk::Rect ta = main_app_->compose_text_area_rect();
                    const bool show_ta = !hide && !ta.empty();
                    room_text_area_->set_visible(show_ta);
                    if (show_ta)
                        room_text_area_->set_rect(ta);
                }

                // Topic edit text area.
                if (topic_text_area_)
                {
                    const tk::Rect tr =
                        main_app_->room_view()->topic_edit_rect();
                    const bool show_t = !hide && !tr.empty();
                    const bool was_visible = topic_text_area_->visible();
                    topic_text_area_->set_visible(show_t);
                    if (show_t)
                    {
                        topic_text_area_->set_rect(tr);
                        if (!was_visible)
                            topic_text_area_->set_text(
                                main_app_->room_view()
                                    ->topic_edit_initial_text());
                    }
                }

                // Room search field.
                if (room_search_field_)
                {
                    bool srch = !hide && main_app_->room_search_field_visible();
                    room_search_field_->set_visible(srch);
                    if (srch)
                    {
                        tk::Rect r = main_app_->room_search_field_rect();
                        r.x += 2;
                        r.y += 2;
                        r.w -= 4;
                        r.h -= 4;
                        room_search_field_->set_rect(r);
                    }
                }

                // Recovery key field.
                if (recovery_key_field_)
                {
                    bool rec = !hide && main_app_->recovery_key_field_visible();
                    recovery_key_field_->set_visible(rec);
                    if (rec)
                    {
                        recovery_key_field_->set_rect(
                            main_app_->recovery_key_field_rect());
                    }
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
        auto view = std::make_unique<tesseract::views::SettingsView>();
        settings_view_ = view.get();
        settings_view_->on_close = [this]
        {
            close_settings_();
        };
        settings_view_->on_logout = [this]
        {
            close_settings_();
            logout_active_account();
        };
        settings_view_->on_theme_changed =
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
        settings_view_->on_inactive_period_changed = [this](int days)
        {
            auto& s = tesseract::Settings::instance();
            s.inactive_room_threshold_days = days;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_view_->on_send_presence_changed = [this](bool enabled)
        {
            handle_send_presence_toggle_(enabled);
        };
        settings_view_->on_tab_changed = [this] { settings_surface_->relayout(); };
        settings_view_->on_clear_caches = [this]
        {
            clear_all_caches_([this](uint64_t local, uint64_t sdk)
            {
                if (settings_view_)
                    settings_view_->set_cache_sizes(local, sdk);
            });
        };
        settings_surface_->set_root(std::move(view));
        settings_surface_->set_on_layout(
            [this]
            {
                if (settings_name_field_ && settings_view_)
                {
                    const tk::Rect r = settings_view_->name_field_rect();
                    settings_name_field_->set_visible(!r.empty());
                    if (!r.empty())
                        settings_name_field_->set_rect(r);
                }
            });
        settings_surface_->set_theme(current_theme_);
        if (settings_surface_->hwnd())
        {
            ShowWindow(settings_surface_->hwnd(), SW_HIDE);
        }
    }

    apply_current_theme_();

    branding_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hwnd, tk::Theme::light());
    branding_surface_->set_root(std::make_unique<tesseract::views::BrandView>());

    {
        RECT wrc{};
        GetWindowRect(hwnd_, &wrc);
        picker_track_pos_ = {wrc.left, wrc.top};
    }

    start_login();
}

void MainWindow::on_destroy()
{
    if (anim_timer_running_ && hwnd_)
    {
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
                             [this]
                             {
                                 return workers_in_flight_ == 0;
                             });
    }
    for (auto& s : accounts_)
    {
        if (s && s->client)
        {
            s->client->stop_sync();
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->stop_sync();
    }
}

// run_async_ is implemented in tesseract::ShellBase.

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::on_size(int w, int h)
{
    constexpr int STATUS_H = 24;

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
    tesseract::SessionStore::migrate_legacy_layout();
    auto index = tesseract::SessionStore::load_index();

    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Restoring session…"));

    for (const auto& uid : index.user_ids)
    {
        auto json = tesseract::SessionStore::load_account(uid);
        if (!json)
        {
            continue;
        }

        auto sess = std::make_unique<tesseract::AccountSession>();
        sess->client = std::make_unique<tesseract::Client>();
        sess->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());

        auto res = sess->client->restore_session(*json);
        if (!res)
        {
            tesseract::SessionStore::clear_account(uid);
            continue;
        }
        sess->user_id = sess->client->get_user_id();
        sess->display_name = sess->client->get_display_name();
        sess->avatar_url = sess->client->get_avatar_url();
        sess->last_room =
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

    if (accounts_.empty())
    {
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_)
        {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_on_begin_oauth(
                [this]
                {
                    if (!pending_login_temp_dir_.empty())
                    {
                        return;
                    }
                    auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    pending_login_temp_dir_ =
                        tesseract::SessionStore::account_dir(
                            "pending-" + std::to_string(ms));
                    pending_login_client_->set_data_dir(
                        (pending_login_temp_dir_ / "matrix-store").string());
                });
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
        }
        show_login_view();
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Not logged in"));
        return;
    }

    int active_idx = 0;
    if (!index.active_user_id.empty())
    {
        for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
        {
            if (accounts_[i]->user_id == index.active_user_id)
            {
                active_idx = i;
                break;
            }
        }
    }
    switch_active_account(active_idx);

    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](auto fn) { post_to_ui_(std::move(fn)); },
        [this](auto fn) { run_async_(std::move(fn)); },
        [this](auto cb)
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

            post_to_ui_([cb = std::move(cb), bytes = std::move(bytes),
                         mime]() mutable { cb(std::move(bytes), mime); });
        });
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
    }

    settings_name_field_ = settings_surface_->host().make_text_field();
    settings_name_field_->set_text(my_display_name_);
    settings_name_field_->set_placeholder("Display name");
    settings_name_field_->set_visible(false);

    settings_name_field_->set_on_submit(
        [this]
        {
            if (!settings_controller_) return;
            settings_controller_->set_display_name(
                settings_name_field_->text());
            settings_view_->set_name_busy(true);
            settings_surface_->relayout();
        });

    settings_controller_->on_name_changed = [this](std::string name)
    {
        settings_view_->set_display_name_text(name);
        if (settings_name_field_) settings_name_field_->set_text(name);
        settings_surface_->relayout();
    };
    settings_controller_->on_name_result = [this](bool ok, std::string error)
    {
        settings_view_->set_name_busy(false);
        if (!ok) settings_view_->set_name_error(std::move(error));
        settings_surface_->relayout();
    };
    settings_controller_->on_avatar_changed = [this](std::string mxc)
    {
        my_avatar_url_ = mxc;
        if (active_account_index_ >= 0 &&
            active_account_index_ < static_cast<int>(accounts_.size()))
        {
            accounts_[active_account_index_]->avatar_url = my_avatar_url_;
        }
        settings_view_->set_avatar_url(mxc);
        settings_surface_->relayout();
        populate_user_strip();
    };
}

void MainWindow::on_login_succeeded()
{
    // The pending client ran OAuth into a temp directory.
    // Drop it (releases SQLite handles), rename the temp dir to its final
    // per-account location, then reopen a fresh client at the final path.
    if (!pending_login_client_)
    {
        return;
    }

    std::string user_id = pending_login_client_->get_user_id();

    // Reject if this account is already signed in.
    for (const auto& a : accounts_)
    {
        if (a->user_id == user_id)
        {
            pending_login_client_.reset();
            if (login_view_)
            {
                login_view_->set_client(nullptr);
            }
            std::error_code ec;
            std::filesystem::remove_all(pending_login_temp_dir_, ec);
            pending_login_temp_dir_.clear();
            if (login_view_)
            {
                login_view_->set_status_message(
                    L"Already signed in as " +
                    std::wstring(user_id.begin(), user_id.end()));
            }
            pending_login_is_add_account_ = false;
            if (add_account_return_idx_ >= 0 &&
                add_account_return_idx_ < static_cast<int>(accounts_.size()))
            {
                switch_active_account(add_account_return_idx_);
            }
            add_account_return_idx_ = -1;
            return;
        }
    }

    std::string json = pending_login_client_->export_session();
    pending_login_client_.reset(); // closes SQLite in the temp dir
    // Null out the dangling pointer before any reset() calls below.
    if (login_view_)
    {
        login_view_->set_client(nullptr);
    }

    namespace fs = std::filesystem;
    fs::path final_dir = tesseract::SessionStore::account_dir(user_id);
    std::error_code ec;
    if (!pending_login_temp_dir_.empty() && fs::exists(pending_login_temp_dir_))
    {
        fs::create_directories(final_dir.parent_path(), ec);
        fs::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec)
        {
            // Rename failed (e.g. cross-device); leave temp dir and bail.
            pending_login_temp_dir_.clear();
            if (login_view_)
            {
                pending_login_client_ = std::make_unique<tesseract::Client>();
                login_view_->set_client(pending_login_client_.get());
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
    if (!sess->client->restore_session(json))
    {
        tesseract::SessionStore::clear_account(user_id);
        if (login_view_)
        {
            pending_login_client_ = std::make_unique<tesseract::Client>();
            login_view_->set_client(pending_login_client_.get());
            login_view_->reset();
        }
        return;
    }
    sess->user_id = sess->client->get_user_id();
    sess->display_name = sess->client->get_display_name();
    sess->avatar_url = sess->client->get_avatar_url();
    sess->last_room =
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

    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](auto fn) { post_to_ui_(std::move(fn)); },
        [this](auto fn) { run_async_(std::move(fn)); },
        [this](auto cb)
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

            post_to_ui_([cb = std::move(cb), bytes = std::move(bytes),
                         mime]() mutable { cb(std::move(bytes), mime); });
        });
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
    }

    settings_name_field_ = settings_surface_->host().make_text_field();
    settings_name_field_->set_text(my_display_name_);
    settings_name_field_->set_placeholder("Display name");
    settings_name_field_->set_visible(false);

    settings_name_field_->set_on_submit(
        [this]
        {
            if (!settings_controller_) return;
            settings_controller_->set_display_name(
                settings_name_field_->text());
            settings_view_->set_name_busy(true);
            settings_surface_->relayout();
        });

    settings_controller_->on_name_changed = [this](std::string name)
    {
        settings_view_->set_display_name_text(name);
        if (settings_name_field_) settings_name_field_->set_text(name);
        settings_surface_->relayout();
    };
    settings_controller_->on_name_result = [this](bool ok, std::string error)
    {
        settings_view_->set_name_busy(false);
        if (!ok) settings_view_->set_name_error(std::move(error));
        settings_surface_->relayout();
    };
    settings_controller_->on_avatar_changed = [this](std::string mxc)
    {
        my_avatar_url_ = mxc;
        if (active_account_index_ >= 0 &&
            active_account_index_ < static_cast<int>(accounts_.size()))
        {
            accounts_[active_account_index_]->avatar_url = my_avatar_url_;
        }
        settings_view_->set_avatar_url(mxc);
        settings_surface_->relayout();
        populate_user_strip();
    };
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
    settings_view_->set_theme_pref(tesseract::Settings::instance().theme_pref);
    settings_view_->set_notifications_enabled(
        tesseract::Settings::instance().notifications_enabled);
    settings_view_->set_hide_content_enabled(
        tesseract::Settings::instance().notification_hide_content);
    settings_view_->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    settings_view_->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    settings_view_->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    settings_view_->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
    settings_view_->set_send_presence_pref(
        tesseract::Settings::instance().send_presence);

    if (settings_controller_ && settings_name_field_)
    {
        settings_name_field_->set_text(my_display_name_);
        settings_surface_->relayout();
    }
    else
    {
        settings_surface_->relayout();
    }

    compute_cache_sizes_([this](uint64_t local, uint64_t sdk)
    {
        if (settings_view_)
            settings_view_->set_cache_sizes(local, sdk);
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

    RECT rc;
    GetClientRect(hwnd_, &rc);
    on_size(rc.right, rc.bottom);
}

void MainWindow::close_settings_()
{
    settings_visible_ = false;
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

void MainWindow::on_reconnect(const std::string& user_id)
{
    int idx = -1;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
    {
        if (accounts_[i]->user_id == user_id)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        return;
    }

    auto& sess = accounts_[idx];
    sess->client->stop_sync();

    auto json = tesseract::SessionStore::load_account(user_id);
    if (json && sess->client->restore_session(*json))
    {
        auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
        bridge->set_user_id(user_id);
        sess->bridge = std::move(bridge);
        if (idx == active_account_index_)
        {
            event_handler_ = sess->bridge.get();
        }
        sess->client->start_sync(sess->bridge.get());
        if (idx == active_account_index_)
        {
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Reconnected"));
        }
    }
    else
    {
        if (idx == active_account_index_)
        {
            logout_active_account();
        }
    }
}

void MainWindow::on_auth_error(const std::string& user_id, bool soft_logout)
{
    int idx = -1;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
    {
        if (accounts_[i]->user_id == user_id)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        return;
    }

    if (soft_logout)
    {
        auto json = tesseract::SessionStore::load_account(user_id);
        if (json && accounts_[idx]->client->restore_session(*json))
        {
            auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
            bridge->set_user_id(user_id);
            accounts_[idx]->bridge = std::move(bridge);
            if (idx == active_account_index_)
            {
                event_handler_ = accounts_[idx]->bridge.get();
                my_user_id_ = accounts_[idx]->client->get_user_id();
                my_display_name_ = accounts_[idx]->client->get_display_name();
                my_avatar_url_ = accounts_[idx]->client->get_avatar_url();
                populate_user_strip();
                SendMessageW(hStatus_, SB_SETTEXTW, 0,
                             reinterpret_cast<LPARAM>(L"Reconnected"));
                maybe_show_recovery_banner();
            }
            accounts_[idx]->client->start_sync(accounts_[idx]->bridge.get());
            return;
        }
    }
    if (idx == active_account_index_)
    {
        SendMessageW(
            hStatus_, SB_SETTEXTW, 0,
            reinterpret_cast<LPARAM>(L"Session expired; please log in again."));
        logout_active_account();
    }
    else
    {
        accounts_[idx]->client->stop_sync();
        tesseract::SessionStore::clear_account(user_id);
        accounts_.erase(accounts_.begin() + idx);
        if (idx < active_account_index_)
        {
            --active_account_index_;
        }
        auto index = tesseract::SessionStore::load_index();
        index.user_ids.erase(
            std::remove(index.user_ids.begin(), index.user_ids.end(), user_id),
            index.user_ids.end());
        if (index.active_user_id == user_id && active_account_index_ >= 0)
        {
            index.active_user_id = accounts_[active_account_index_]->user_id;
        }
        tesseract::SessionStore::save_index(index);
    }
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

    for (auto& sess : accounts_)
    {
        if (sess->user_id != p->user_id)
        {
            continue;
        }
        // Already watching this exact room — suppress silently.
        if (win_focused && active_account_index_ >= 0 &&
            accounts_[active_account_index_]->user_id == p->user_id &&
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

    for (const auto& r : rooms_)
    {
        if (r.id == room_id && r.is_space)
        {
            space_stack_.push_back(room_id);
            refresh_room_list();
            return;
        }
    }

    hide_slash_popup_();
    hide_shortcode_popup_();
    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

    current_room_id_ = room_id;
    clear_focused_state_(room_id);
    KillTimer(hwnd_, kMarkReadTimerId);
    SetTimer(hwnd_, kMarkReadTimerId,
             static_cast<UINT>(
                 tesseract::Settings::instance().mark_as_read_delay_ms),
             nullptr);
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
    }
    if (room_text_area_)
    {
        room_text_area_->set_text("");
    }
    if (room_text_area_)
    {
        room_text_area_->set_focused(true);
    }
    if (room_view_)
    {
        room_view_->set_current_text({});
    }
    update_typing_bar_({}, false);

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
    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the Win32 message pump stays responsive.
    {
        auto& state = pagination_[current_room_id_];
        if (state.in_flight)
            return;
        state.in_flight = true;
    }
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    HWND hwnd = hwnd_;
    std::string sub_room = current_room_id_;
    tesseract::Client* cl = client_;
    run_async_(
        [this, sub_room, hwnd, cl, visible_ids = std::move(visible_ids)]
        {
            auto res = cl->subscribe_room(sub_room);
            bool reached = false;
            if (res)
            {
                auto pr =
                    cl->paginate_back_with_status(sub_room, kPaginationBatch);
                reached = pr.ok && pr.reached_start;
                cl->start_background_backfill(visible_ids);
            }
            auto* p = new std::string(sub_room);
            PostMessageW(hwnd, WM_TESSERACT_SUBSCRIBE_DONE,
                         static_cast<WPARAM>(reached),
                         reinterpret_cast<LPARAM>(p));
        });
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

void MainWindow::on_tesseract_timeline_reset(PostedTimelineReset* payload)
{
    if (!payload)
    {
        return;
    }

    if (payload->room_id == current_room_id_ && room_view_)
    {
        auto rows = build_rows_(payload->snapshot);
        // A genuine switch, OR a re-population of an emptied view (e.g.
        // logout → login → same room): both warrant the display gate.
        const auto* ml = room_view_->message_list();
        const bool room_switch = view_displayed_room_id_ != payload->room_id ||
                                 (ml && ml->messages().empty());
        view_displayed_room_id_ = payload->room_id;
        room_view_->set_messages(std::move(rows), room_switch);
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
        const auto& pstate = pagination_[payload->room_id];
        if (room_switch && pstate.is_focused && room_view_->message_list())
        {
            room_view_->message_list()->begin_focused_gate(
                pstate.focus_event_id);
        }
        room_view_->set_historical_mode(pstate.is_focused);
        if (pstate.is_focused)
        {
            room_view_->scroll_to_event_id(pstate.focus_event_id);
        }
    }

    dispatch_timeline_reset_secondary_(payload->room_id, payload->snapshot);
}

void MainWindow::on_tesseract_message_inserted(PostedMessageEvent* payload)
{
    if (!payload || !payload->event)
    {
        return;
    }
    if (payload->event->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (payload->room_id == current_room_id_ && room_view_)
    {
        ensure_row_media(*payload->event);
        if (!payload->event->in_reply_to_id.empty())
        {
            ensure_reply_details_(payload->event->event_id);
        }
        room_view_->insert_message(
            payload->index,
            tesseract::views::make_row_data(*payload->event, my_user_id_));
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }

    dispatch_message_inserted_secondary_(payload->room_id, payload->index,
                                         *payload->event);
}

void MainWindow::on_tesseract_message_updated(PostedMessageEvent* payload)
{
    if (!payload || !payload->event)
    {
        return;
    }
    if (payload->event->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (payload->room_id == current_room_id_ && room_view_)
    {
        ensure_row_media(*payload->event);
        if (!payload->event->in_reply_to_id.empty())
        {
            ensure_reply_details_(payload->event->event_id);
        }
        room_view_->update_message(
            payload->index,
            tesseract::views::make_row_data(*payload->event, my_user_id_));
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }

    dispatch_message_updated_secondary_(payload->room_id, payload->index,
                                        *payload->event);
}

void MainWindow::on_tesseract_message_removed(PostedMessageEvent* payload)
{
    if (!payload)
    {
        return;
    }

    if (payload->room_id == current_room_id_ && room_view_)
    {
        room_view_->remove_message(payload->index);
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }

    dispatch_message_removed_secondary_(payload->room_id, payload->index);
}

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
    else if (!pending_restore_room_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                on_room_selected(target);
                break;
            }
        }
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
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
}

void MainWindow::on_tesseract_subscribe_done(std::string* room_id,
                                             bool reached_start)
{
    if (!room_id || *room_id != current_room_id_)
    {
        return;
    }
    push_paginate_result_(*room_id, reached_start);
}

void MainWindow::on_tesseract_jump_done(JumpDonePayload* p)
{
    if (!p)
    {
        return;
    }
    if (!p->ok)
    {
        std::wstring msg =
            L"Jump to date failed: " + utf8_to_wstr(p->error_msg);
        MessageBoxW(hwnd_, msg.c_str(), L"Jump to Date",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    begin_focused_subscription_(p->room_id, p->event_id);
    const std::string room_id = p->room_id;
    const std::string event_id = p->event_id;
    run_async_(
        [this, room_id, event_id]
        {
            client_->subscribe_room_at(room_id, event_id);
        });
}

namespace
{

struct JumpPickerState
{
    HWND hCal;
    bool done;
    bool accepted;
    SYSTEMTIME result;
};

LRESULT CALLBACK jump_to_date_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam)
{
    auto* ctx = reinterpret_cast<JumpPickerState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_COMMAND:
        if (!ctx)
        {
            break;
        }
        if (LOWORD(wParam) == IDOK)
        {
            MonthCal_GetCurSel(ctx->hCal, &ctx->result);
            ctx->result.wHour = ctx->result.wMinute = ctx->result.wSecond =
                ctx->result.wMilliseconds = 0;
            ctx->accepted = true;
            ctx->done = true;
            DestroyWindow(hwnd);
        }
        else if (LOWORD(wParam) == IDCANCEL)
        {
            ctx->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (ctx)
        {
            ctx->done = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // anonymous namespace

void MainWindow::openJumpToDateDialog()
{
    if (current_room_id_.empty())
    {
        return;
    }

    // Register the picker window class once.
    static bool s_registered = false;
    if (!s_registered)
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_DATE_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = jump_to_date_proc;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        wc.lpszClassName = L"TesseractJumpToDate";
        RegisterClassExW(&wc);
        s_registered = true;
    }

    JumpPickerState ctx{nullptr, false, false, {}};

    // Compute popup position (centred on the main window).
    const int kBtnH = 28, kBtnW = 90, kGap = 8;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);

    // Create the host popup (placeholder size; resized after MonthCal).
    HWND hPicker =
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"TesseractJumpToDate",
                        L"Jump to Date", WS_POPUP | WS_CAPTION | WS_SYSMENU, 0,
                        0, 200, 200, hwnd_, nullptr, hInst_, &ctx);
    if (!hPicker)
    {
        return;
    }

    // Create MonthCal control to get its minimum size.
    HWND hCal = CreateWindowExW(0, MONTHCAL_CLASS, L"",
                                WS_CHILD | WS_VISIBLE | MCS_NOTODAY, kGap, kGap,
                                180, 150, hPicker, nullptr, hInst_, nullptr);
    if (!hCal)
    {
        DestroyWindow(hPicker);
        return;
    }
    ctx.hCal = hCal;

    // Query minimum size and resize popup accordingly.
    RECT calMin{};
    MonthCal_GetMinReqRect(hCal, &calMin);
    const int calW = calMin.right - calMin.left;
    const int calH = calMin.bottom - calMin.top;

    RECT adjRect{0, 0, calW + kGap * 2, calH + kGap * 3 + kBtnH};
    AdjustWindowRectEx(&adjRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME);
    const int wndW = adjRect.right - adjRect.left;
    const int wndH = adjRect.bottom - adjRect.top;
    const int cx = (wr.left + wr.right - wndW) / 2;
    const int cy = (wr.top + wr.bottom - wndH) / 2;

    SetWindowPos(hPicker, nullptr, cx, cy, wndW, wndH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hCal, nullptr, kGap, kGap, calW, calH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Set min/max date on the MonthCal: 1970-01-01 … today.
    SYSTEMTIME range[2]{};
    range[0].wYear = 1970;
    range[0].wMonth = 1;
    range[0].wDay = 1;
    GetSystemTime(&range[1]); // UTC today
    range[1].wHour = 0;
    range[1].wMinute = 0;
    range[1].wSecond = 0;
    range[1].wMilliseconds = 0;
    MonthCal_SetRange(hCal, GDTR_MIN | GDTR_MAX, range);

    // Buttons: OK and Cancel.
    const int btnTop = kGap + calH + kGap;
    const int totalBtns = kBtnW * 2 + kGap;
    const int btnLeft = (calW + kGap * 2 - totalBtns) / 2;
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  btnLeft, btnTop, kBtnW, kBtnH, hPicker,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst_,
                  nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  btnLeft + kBtnW + kGap, btnTop, kBtnW, kBtnH, hPicker,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                  hInst_, nullptr);

    ShowWindow(hPicker, SW_SHOW);

    // Run a nested modal message loop.
    // RAII guard: re-enable the parent on all exit paths.
    struct EnableGuard
    {
        HWND hwnd;
        ~EnableGuard()
        {
            EnableWindow(hwnd, TRUE);
        }
    } guard{hwnd_};
    EnableWindow(hwnd_, FALSE);
    MSG m{};
    BOOL ret;
    while (!ctx.done && (ret = GetMessageW(&m, nullptr, 0, 0)) != 0)
    {
        if (ret == -1)
        {
            break; // GetMessage error — abort loop
        }
        if (!IsDialogMessageW(hPicker, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    SetForegroundWindow(hwnd_);

    if (!ctx.accepted)
    {
        return;
    }

    // Convert selected UTC date to Unix ms via FILETIME.
    FILETIME ft{};
    SystemTimeToFileTime(&ctx.result, &ft);
    ULARGE_INTEGER ui;
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    // FILETIME epoch (1601-01-01) to Unix epoch (1970-01-01): 116444736000000000 * 100ns
    constexpr ULONGLONG kEpochDiff = 116444736000000000ULL;
    const uint64_t ts_ms = (ui.QuadPart - kEpochDiff) / 10000ULL;

    const std::string room_id = current_room_id_;
    HWND main_hwnd = hwnd_;
    tesseract::Client* cl = client_;
    run_async_(
        [cl, room_id, ts_ms, main_hwnd]
        {
            auto res = cl->timestamp_to_event(room_id, ts_ms, "f");
            auto* p = new JumpDonePayload{
                res.ok, room_id, res.ok ? res.message : std::string{},
                !res.ok ? res.message : std::string{}};
            PostMessageW(main_hwnd, WM_TESSERACT_JUMP_DONE, 0,
                         reinterpret_cast<LPARAM>(p));
        });
}

void MainWindow::refresh_room_list()
{
    if (!room_list_view_)
    {
        return;
    }

    std::vector<tesseract::RoomInfo> filtered;
    if (space_stack_.empty())
    {
        // Hide the space nav bar via the widget tree.
        if (main_app_)
        {
            main_app_->set_space_nav(false);
        }

        if (!pending_search_text_.empty())
        {
            for (const auto& r : rooms_)
            {
                ensure_room_avatar_(r);
            }
            room_list_view_->set_rooms(rooms_);
            if (!current_room_id_.empty())
            {
                room_list_view_->set_selected_room(current_room_id_);
            }
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
            return;
        }
        // Build the root list, excluding rooms that are children of any space
        // (they appear only when drilled in).
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
        {
            if (r.is_space)
            {
                auto sc_it = space_children_cache_.find(r.id);
                if (sc_it != space_children_cache_.end())
                {
                    for (const auto& id : sc_it->second)
                    {
                        in_space.insert(id);
                    }
                }
            }
        }
        for (const auto& r : rooms_)
        {
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        for (const auto& r : rooms_)
        {
            if (r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        apply_space_child_counts_(filtered);
    }
    else
    {
        // Show the navigation bar with the current space's name and avatar.
        std::string space_name;
        std::string space_avatar;
        for (const auto& r : rooms_)
        {
            if (r.id == space_stack_.back())
            {
                space_name = r.name;
                space_avatar = r.avatar_url;
                ensure_room_avatar_(r);
                break;
            }
        }
        if (main_app_)
        {
            main_app_->set_space_nav(true, space_name, space_avatar);
        }

        static const std::vector<std::string> kNoChildren;
        const auto sc_it = space_children_cache_.find(space_stack_.back());
        const auto& child_ids =
            sc_it != space_children_cache_.end() ? sc_it->second : kNoChildren;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
            {
                filtered.push_back(r);
            }
        }
    }
    for (const auto& r : filtered)
    {
        ensure_room_avatar_(r);
    }

    room_list_view_->set_rooms(filtered);
    if (!current_room_id_.empty())
    {
        room_list_view_->set_selected_room(current_room_id_);
    }

    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_space_back()
{
    if (!space_stack_.empty())
    {
        space_stack_.pop_back();
    }
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
    if (url.empty() || bytes.empty())
    {
        return;
    }
    if (anim_cache_.has(url))
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
    anim_cache_.store(url, std::move(imgs), std::move(delays),
                      static_cast<std::int64_t>(GetTickCount64()));
    // Drop any static-cache leftover from a prior probe.
    tk_images_.erase(url);

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
        if (auto img = main_app_surface_->factory().decode_image(bytes))
        {
            tk_avatars_.emplace(cache_key, std::move(img));
        }
        main_app_surface_->relayout();
        break;
    case MediaKind::UserAvatar:
        if (auto img = main_app_surface_->factory().decode_image(bytes))
        {
            tk_avatars_.emplace(cache_key, std::move(img));
        }
        if (hAccountPicker_ && IsWindowVisible(hAccountPicker_) &&
            account_picker_surface_)
        {
            account_picker_surface_->relayout();
        }
        break;
    case MediaKind::MediaImage:
        try_load_animation(cache_key, bytes);
        if (!anim_cache_.has(cache_key))
        {
            if (auto img = main_app_surface_->factory().decode_image(bytes))
            {
                tk_images_.emplace(cache_key, std::move(img));
            }
            else
            {
                media_disk_cache_.evict(cache_key);
            }
        }
        if (room_view_)
        {
            room_view_->notify_image_ready(cache_key);
        }
        main_app_surface_->relayout();
        if (shortcode_popup_visible_() && shortcode_popup_surface_)
        {
            shortcode_popup_surface_->relayout();
        }
        break;
    case MediaKind::Tile:
        if (tk_images_.count(cache_key))
        {
            return;
        }
        if (auto img = main_app_surface_->factory().decode_image(bytes))
        {
            tk_images_.emplace(cache_key, std::move(img));
            if (room_view_)
            {
                room_view_->message_list()->invalidate_data();
            }
            main_app_surface_->relayout();
        }
        break;
    }
    if (invalidate_hwnd)
    {
        InvalidateRect(invalidate_hwnd, nullptr, FALSE);
    }
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

void MainWindow::extract_media_info_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     std::string mime)
{
    run_async_([this, pending_gen, bytes = std::move(bytes), mime = std::move(mime)]() mutable
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
            MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
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
                                    cur_type->Release();
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
            MFShutdown();
        }
        // ── Audio: duration only via Media Foundation ──────────────────────
        else if (mime.rfind("audio/", 0) == 0)
        {
            MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
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
            MFShutdown();
        }

        CoUninitialize();

        // Post result back to the UI thread; resolve compose_bar() at call
        // time to avoid dangling pointer if the view was freed.
        post_to_ui_([this, info = std::move(info)]() mutable
        {
            if (room_view_)
                room_view_->compose_bar()->update_pending_attachment(info);
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
    if (tk_images_.count(key) || !main_app_surface_)
    {
        return;
    }
    auto img =
        main_app_surface_->factory().create_image_rgba(rgba.data(), w, h);
    if (!img)
    {
        return;
    }
    tk_images_.emplace(key, std::move(img));
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

// ---------------------------------------------------------------------------
// Recovery banner (Step 6) — inline key entry, no modal dialog.
// ---------------------------------------------------------------------------

namespace
{
std::wstring widen_utf8(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

} // namespace

void MainWindow::maybe_show_recovery_banner()
{
    if (!client_)
    {
        return;
    }
    if (recovery_banner_dismissed_)
    {
        return;
    }
    if (!client_->needs_recovery())
    {
        return;
    }
    if (recovery_banner_visible_)
    {
        return;
    }
    if (verif_banner_visible_)
    {
        return;
    }
    if (!main_app_)
    {
        return;
    }

    if (recovery_shared_)
    {
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Form);
        recovery_shared_->set_current_key("");
    }
    if (recovery_key_field_)
    {
        recovery_key_field_->set_text("");
        recovery_key_field_->set_enabled(true);
    }
    main_app_->show_recovery_banner(true);
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    recovery_banner_visible_ = true;
    recovery_in_flight_ = false;
}

void MainWindow::on_recovery_verify_clicked()
{
    std::string key;
    if (recovery_key_field_)
    {
        key = recovery_key_field_->text();
    }
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
    {
        if (recovery_shared_)
        {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            recovery_shared_->set_failure_message(
                "Please enter a recovery key or passphrase.");
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (recovery_shared_)
    {
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    }
    if (recovery_key_field_)
    {
        recovery_key_field_->set_enabled(false);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    recovery_in_flight_ = true;

    HWND target = hwnd_;
    run_async_(
        [this, target, key]()
        {
            auto res = client_->recover(key);
            WPARAM ok = res.ok ? 1 : 0;
            auto* p = new std::wstring(widen_utf8(res.message));
            PostMessageW(target, WM_TESSERACT_RECOVER_DONE, ok,
                         reinterpret_cast<LPARAM>(p));
        });
}

void MainWindow::on_recover_done(bool ok, std::wstring msg)
{
    if (ok)
    {
        if (recovery_shared_)
        {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Importing);
        }
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
        return;
    }
    if (recovery_shared_)
    {
        recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Failed);
        // wstring → utf8 for the failure detail.
        int n = WideCharToMultiByte(CP_UTF8, 0, msg.data(),
                                    static_cast<int>(msg.size()), nullptr, 0,
                                    nullptr, nullptr);
        std::string utf8(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg.data(),
                            static_cast<int>(msg.size()), utf8.data(), n,
                            nullptr, nullptr);
        recovery_shared_->set_failure_message(utf8);
    }
    if (recovery_key_field_)
    {
        recovery_key_field_->set_enabled(true);
        recovery_key_field_->set_focused(true);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    recovery_in_flight_ = false;
}

void MainWindow::on_recovery_dismiss_clicked()
{
    recovery_banner_dismissed_ = true;
    recovery_key_chosen_ = false;
    if (main_app_)
    {
        main_app_->show_recovery_banner(false);
    }
    recovery_banner_visible_ = false;
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_backup_progress(tesseract::BackupProgress* progress)
{
    maybe_show_recovery_banner();

    if (recovery_banner_visible_ && recovery_shared_ &&
        recovery_shared_->state() ==
            tesseract::views::RecoveryBanner::State::Importing &&
        progress->state == tesseract::BackupState::Downloading &&
        progress->imported_keys > 0)
    {
        recovery_shared_->set_import_progress(progress->imported_keys);
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }
    if (progress->state == tesseract::BackupState::Enabled &&
        !client_->needs_recovery())
    {
        if (main_app_)
        {
            main_app_->show_recovery_banner(false);
        }
        recovery_banner_visible_ = false;
        recovery_key_chosen_ = false;
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
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
    if (sync_progress_shown_)
    {
        sync_progress_shown_ = false;
        SetWindowTextW(hStatus_, L"Connected");
    }
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

void MainWindow::switch_active_account(int new_idx)
{
    if (new_idx < 0 || new_idx >= static_cast<int>(accounts_.size()))
    {
        return;
    }
    const int old_idx = active_account_index_;
    reset_server_info_();
    active_account_index_ = new_idx;
    auto& sess = accounts_[new_idx];

    client_ = sess->client.get();
    event_handler_ = sess->bridge.get();

    my_user_id_ = sess->user_id;
    my_display_name_ = sess->display_name;
    my_avatar_url_ = sess->avatar_url;
    pending_restore_room_ = sess->last_room;

    if (settings_controller_)
        settings_controller_->set_client(client_);

    current_room_id_.clear();
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();

    // Swap in cached rooms snapshot if available.
    auto it = per_account_rooms_.find(sess->user_id);
    if (it != per_account_rooms_.end())
    {
        rooms_ = it->second;
    }
    else
    {
        rooms_.clear();
    }

    // Restore the invite snapshot for the incoming account (parallel to rooms_).
    auto inv_it = per_account_invites_.find(sess->user_id);
    invites_ = (inv_it != per_account_invites_.end())
                   ? inv_it->second
                   : std::vector<tesseract::InviteInfo>{};
    on_invites_updated_();

    // Dismiss any stale InviteCard from the previous account.
    current_invite_room_id_.clear();
    current_invite_inviter_id_.clear();
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

    // Save banner state for the outgoing account, then load for the incoming.
    if (old_idx >= 0 && old_idx < static_cast<int>(accounts_.size()))
    {
        accounts_[old_idx]->recovery_banner_dismissed      = recovery_banner_dismissed_;
        accounts_[old_idx]->recovery_key_chosen            = recovery_key_chosen_;
        accounts_[old_idx]->verification_banner_dismissed  = verification_banner_dismissed_;
    }
    recovery_banner_visible_ = false;
    recovery_banner_dismissed_     = sess->recovery_banner_dismissed;
    recovery_key_chosen_           = sess->recovery_key_chosen;
    verification_banner_dismissed_ = sess->verification_banner_dismissed;
    if (main_app_)
    {
        main_app_->show_recovery_banner(false);
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

    // Update active_user_id on disk.
    auto idx = tesseract::SessionStore::load_index();
    idx.active_user_id = sess->user_id;
    tesseract::SessionStore::save_index(idx);

    show_main_content();
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Connected"));
    handle_verification_state_ui_(!sess->unverified);
    maybe_show_recovery_banner();

    if (!tray_)
    {
        tray_ = std::make_unique<Win32TrayIcon>(
            hInst_,
            [this]
            {
                ShowWindow(hwnd_, SW_SHOW);
                if (IsIconic(hwnd_))
                {
                    ShowWindow(hwnd_, SW_RESTORE);
                }
                SetForegroundWindow(hwnd_);
            },
            [this]
            {
                if (IsWindowVisible(hwnd_))
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
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    if (login_view_)
    {
        login_view_->set_client(pending_login_client_.get());
        login_view_->set_on_begin_oauth(
            [this]
            {
                if (!pending_login_temp_dir_.empty())
                {
                    return;
                }
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                    "pending-" + std::to_string(ms));
                pending_login_client_->set_data_dir(
                    (pending_login_temp_dir_ / "matrix-store").string());
            });
        login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
        login_view_->reset();
    }
    show_login_view();
}

void MainWindow::on_login_cancelled()
{
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
        pending_login_temp_dir_.clear();
    }
    pending_login_is_add_account_ = false;

    if (add_account_return_idx_ >= 0 &&
        add_account_return_idx_ < static_cast<int>(accounts_.size()))
    {
        switch_active_account(add_account_return_idx_);
    }
    else
    {
        show_login_view();
    }
    add_account_return_idx_ = -1;
}

void MainWindow::logout_active_account()
{
    if (active_account_index_ < 0 ||
        active_account_index_ >= static_cast<int>(accounts_.size()))
    {
        return;
    }

    std::string uid = accounts_[active_account_index_]->user_id;
    notify_presence_logout_();
    accounts_[active_account_index_]->client->logout();
    accounts_[active_account_index_]->client->stop_sync();
    accounts_.erase(accounts_.begin() + active_account_index_);

    tesseract::SessionStore::clear_account(uid);
    per_account_rooms_.erase(uid);
    per_account_invites_.erase(uid);
    auto index = tesseract::SessionStore::load_index();
    index.user_ids.erase(
        std::remove(index.user_ids.begin(), index.user_ids.end(), uid),
        index.user_ids.end());

    current_room_id_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    rooms_.clear();
    invites_.clear();
    current_invite_room_id_.clear();
    current_invite_inviter_id_.clear();
    reset_server_info_();
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
        main_app_->show_recovery_banner(false);
        main_app_->show_verif_banner(false);
    }
    recovery_banner_visible_ = false;
    recovery_banner_dismissed_ = false;
    recovery_key_chosen_ = false;
    verification_banner_dismissed_ = false;
    verif_banner_visible_ = false;
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    if (accounts_.empty())
    {
        client_ = nullptr;
        event_handler_ = nullptr;
        active_account_index_ = -1;
        index.active_user_id.clear();
        tesseract::SessionStore::save_index(index);

        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        if (login_view_)
        {
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_on_begin_oauth(
                [this]
                {
                    if (!pending_login_temp_dir_.empty())
                    {
                        return;
                    }
                    auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    pending_login_temp_dir_ =
                        tesseract::SessionStore::account_dir(
                            "pending-" + std::to_string(ms));
                    pending_login_client_->set_data_dir(
                        (pending_login_temp_dir_ / "matrix-store").string());
                });
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        on_size(rc.right, rc.bottom);
        show_login_view();
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Signed out"));
    }
    else
    {
        int next = std::min(active_account_index_,
                            static_cast<int>(accounts_.size()) - 1);
        active_account_index_ = -1; // force re-bind
        index.active_user_id = accounts_[next]->user_id;
        tesseract::SessionStore::save_index(index);
        switch_active_account(next);
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Signed out"));
    }
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
        constexpr int kPickerW = 260;
        constexpr int kRowH = 56;
        int kPickerH = kRowH * static_cast<int>(accounts_.size());
        hAccountPicker_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TesseractAccountPicker", L"",
            WS_POPUP | WS_BORDER, 0, 0, kPickerW, kPickerH, hwnd_, nullptr,
            hInst_, nullptr);
        if (!hAccountPicker_)
        {
            return;
        }

        account_picker_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, hAccountPicker_, tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(make_avatar_image_provider_());
        account_picker_->on_select = [this](const std::string& uid)
        {
            if (hAccountPicker_)
            {
                ShowWindow(hAccountPicker_, SW_HIDE);
            }
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
            {
                if (accounts_[i]->user_id == uid)
                {
                    switch_active_account(i);
                    break;
                }
            }
        };
        account_picker_surface_->set_root(std::move(picker));
        if (HWND s = account_picker_surface_->hwnd())
        {
            constexpr int kPickerW = 260;
            constexpr int kRowH = 56;
            int kPickerH = kRowH * static_cast<int>(accounts_.size());
            SetWindowPos(s, nullptr, 0, 0, kPickerW, kPickerH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Rebuild entries.
    std::vector<tesseract::views::AccountEntry> entries;
    for (const auto& s : accounts_)
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
    if (accounts_.size() < 2)
    {
        return;
    }
    rebuild_account_picker();
    if (!hAccountPicker_)
    {
        return;
    }

    constexpr int kPickerW = 260;
    constexpr int kRowH = 56;
    int kPickerH = kRowH * static_cast<int>(accounts_.size());

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
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, IDM_ADD_ACCOUNT, L"Add Account…");
    std::wstring logout_label = L"Log Out";
    if (!my_display_name_.empty())
    {
        logout_label += L" ";
        logout_label += utf8_to_wstr(my_display_name_);
    }
    AppendMenuW(menu, MF_STRING, IDM_LOGOUT, logout_label.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    UINT pick = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screen_x,
                               screen_y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (pick)
    {
        PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(pick, 0), 0);
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
        0, kEmojiPickW, kEmojiPickH, hwnd_, nullptr, hInst_, nullptr);
    if (!hEmojiPicker_)
    {
        return;
    }

    emoji_picker_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hEmojiPicker_, tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
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
        SetWindowPos(s, nullptr, 0, 0, kEmojiPickW, kEmojiPickH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    emoji_picker_search_field_ =
        emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder("Search emoji");
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (emoji_picker_shared_)
            {
                emoji_picker_shared_->set_search_query(q);
            }
            if (emoji_picker_surface_)
            {
                emoji_picker_surface_->relayout();
            }
        });
    emoji_picker_surface_->set_on_layout(
        [this]
        {
            if (emoji_picker_search_field_ && emoji_picker_shared_)
            {
                tk::Rect r = emoji_picker_shared_->search_field_rect();
                r.x += 1;
                r.y += 1;
                r.w -= 2;
                r.h -= 2;
                emoji_picker_search_field_->set_rect(r);
            }
        });
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
    int x = btn_rc.left + 8;
    int y = btn_rc.top - kEmojiPickH - 4;

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

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST, x, y, kEmojiPickW, kEmojiPickH,
                 SWP_NOACTIVATE);

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_focused(true);
    }
}

// ---------------------------------------------------------------------------
// Slash-command popup — WS_POPUP HWND hosting a tk::win32::Surface that
// paints the shared tesseract::views::SlashCommandPopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_slash_popup_(
    const std::vector<tesseract::views::SlashCommandSuggestion>& items,
    tk::Rect cursor_local)
{
    int w = int(tesseract::views::SlashCommandPopup::kWidth);
    int rows = std::min((int)items.size(),
                        int(tesseract::views::SlashCommandPopup::kMaxRows));
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);

    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);

    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);

    int x = pt.x;
    int y_above = pt.y - h - 4;
    int y_below = pt.y + int(cursor_local.h) + 4;
    int y = (y_above >= mi.rcWork.top) ? y_above : y_below;
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);

    if (!slash_popup_hwnd_)
    {
        slash_popup_hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, x, y, w,
            h, nullptr, nullptr, hInst_, nullptr);
        slash_popup_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, slash_popup_hwnd_, main_app_surface_->theme());
        auto pw = std::make_unique<tesseract::views::SlashCommandPopup>();
        slash_popup_widget_ = pw.get();
        slash_popup_surface_->set_root(std::move(pw));
        slash_popup_widget_->on_accepted =
            [this](tesseract::views::SlashCommandSuggestion s)
        {
            hide_slash_popup_();
            if (!room_text_area_) return;
            if (!client_ || current_room_id_.empty())
            {
                return;
            }
            if (s.args_hint.empty())
            {
                // No args — send immediately.
                std::string body = "/" + s.name;
                (void)tesseract::dispatch_compose_send(
                    *client_, current_room_id_, body, std::string{});
                room_text_area_->set_text("");
                room_view_->set_current_text({});
            }
            else
            {
                // Needs args — autocomplete to `/name ` and leave the
                // composer open for the user to type arguments.
                std::string body = "/" + s.name + " ";
                room_text_area_->set_text(body);
            }
        };
        slash_popup_widget_->on_dismissed = [this]
        {
            hide_slash_popup_();
        };
    }

    slash_popup_widget_->set_suggestions(items);
    slash_popup_widget_->set_selected_index(0);
    SetWindowPos(slash_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // The Surface is a WS_CHILD created at a placeholder size; the STATIC
    // popup parent never forwards WM_SIZE, so stretch the child to fill the
    // popup every show (row count changes the height). Without this the
    // surface stays tiny and clicks on suggestion rows never reach it.
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
    if (room_text_area_)
    {
        room_text_area_->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Shortcode popup — WS_POPUP HWND hosting a tk::win32::Surface that paints
// the shared tesseract::views::ShortcodePopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_shortcode_popup_(
    const std::vector<tesseract::views::ShortcodeSuggestion>& suggestions,
    tk::Rect cursor_local)
{
    int w = int(tesseract::views::ShortcodePopup::kWidth);
    int rows = std::min((int)suggestions.size(),
                        int(tesseract::views::ShortcodePopup::kMaxRows));
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);

    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);

    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);

    int x = pt.x;
    int y_above = pt.y - h - 4;
    int y_below = pt.y + int(cursor_local.h) + 4;
    int y = (y_above >= mi.rcWork.top) ? y_above : y_below;
    x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);

    if (!shortcode_popup_hwnd_)
    {
        shortcode_popup_hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, x, y, w,
            h, nullptr, nullptr, hInst_, nullptr);
        shortcode_popup_surface_ = std::make_unique<tk::win32::Surface>(
            hInst_, shortcode_popup_hwnd_, main_app_surface_->theme());
        auto pw = std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = pw.get();
        shortcode_popup_surface_->set_root(std::move(pw));
        shortcode_popup_widget_->on_accepted =
            [this](tesseract::views::ShortcodeSuggestion s)
        {
            std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
            room_text_area_->replace_range(shortcode_active_match_.start,
                                           shortcode_active_match_.end,
                                           std::move(r));
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->on_dismissed = [this]
        {
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->set_image_provider(
            make_static_image_provider_());
    }

    shortcode_popup_widget_->set_suggestions(suggestions);
    SetWindowPos(shortcode_popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // The Surface is a WS_CHILD created at a placeholder size; the STATIC
    // popup parent never forwards WM_SIZE, so stretch the child to fill the
    // popup every show (row count changes the height). Without this the
    // surface stays tiny and clicks on suggestion rows never reach it.
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
    if (room_text_area_)
    {
        room_text_area_->set_on_popup_nav(nullptr);
    }
}

// ── @mention popup ─────────────────────────────────────────────────────────

void MainWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_hwnd_ || !main_app_surface_)
    {
        return;
    }
    int w = int(tesseract::views::MentionPopup::kWidth);
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);

    HWND parent = main_app_surface_->hwnd();
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(parent, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    int x = pt.x;
    int y_above = pt.y - h - 4;
    int y_below = pt.y + int(cursor_local.h) + 4;
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

    // Route keyboard nav to the controller while the popup is up (re-installed
    // each show; mutually exclusive with the shortcode popup).
    room_text_area_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        {
            return mention_controller_ && mention_controller_->on_nav(nk);
        });
}

void MainWindow::hide_mention_popup_()
{
    if (mention_popup_hwnd_)
    {
        ShowWindow(mention_popup_hwnd_, SW_HIDE);
    }
    if (room_text_area_)
    {
        room_text_area_->set_on_popup_nav(nullptr);
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
    POINT pt{static_cast<LONG>(local_rect.x), static_cast<LONG>(local_rect.y)};
    ClientToScreen(parent_hwnd, &pt);
    LONG rectW = static_cast<LONG>(local_rect.w);
    LONG rectH = static_cast<LONG>(local_rect.h);

    // Prefer above, centered on the rect; fall back to below if the
    // monitor doesn't have room. Clamp to the work area horizontally.
    int x = pt.x + rectW / 2 - kEmojiPickW / 2;
    int y = pt.y - kEmojiPickH - 4;
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
        if (x + kEmojiPickW > mi.rcWork.right)
        {
            x = mi.rcWork.right - kEmojiPickW - 4;
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y + kEmojiPickH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - kEmojiPickH - 4;
        }
    }
    (void)rectW;

    SetWindowPos(hEmojiPicker_, HWND_TOPMOST, x, y, kEmojiPickW, kEmojiPickH,
                 SWP_NOACTIVATE);

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }

    ShowWindow(hEmojiPicker_, SW_SHOWNOACTIVATE);
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_focused(true);
    }
}

void MainWindow::popup_sticker_at_rect(HWND parent_hwnd, tk::Rect local_rect)
{
    ensure_sticker_picker_created();
    if (!hStickerPicker_ || !parent_hwnd)
    {
        return;
    }

    POINT pt{static_cast<LONG>(local_rect.x), static_cast<LONG>(local_rect.y)};
    ClientToScreen(parent_hwnd, &pt);
    LONG rectW = static_cast<LONG>(local_rect.w);
    LONG rectH = static_cast<LONG>(local_rect.h);

    // Prefer above, centered on the rect; fall back to below if the
    // monitor doesn't have room. Clamp to the work area horizontally.
    int x = pt.x + rectW / 2 - kStickerPickW / 2;
    int y = pt.y - kStickerPickH - 4;
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
        if (x + kStickerPickW > mi.rcWork.right)
        {
            x = mi.rcWork.right - kStickerPickW - 4;
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y + kStickerPickH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - kStickerPickH - 4;
        }
    }
    (void)rectW;

    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }

    SetWindowPos(hStickerPicker_, HWND_TOPMOST, x, y, kStickerPickW,
                 kStickerPickH, SWP_NOACTIVATE);
    ShowWindow(hStickerPicker_, SW_SHOWNOACTIVATE);
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_focused(true);
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
        return;
    }
    // Compose mode: today's behaviour — insert `:shortcode:` text into
    // the compose field. MSC2545 rich-emoticon sending is a separate task.
    if (!room_text_area_)
    {
        return;
    }
    room_text_area_->insert_at_cursor(":" + img.shortcode + ":");
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
        0, kStickerPickW, kStickerPickH, hwnd_, nullptr, hInst_, nullptr);
    if (!hStickerPicker_)
    {
        return;
    }

    sticker_picker_surface_ = std::make_unique<tk::win32::Surface>(
        hInst_, hStickerPicker_, tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (!current_room_id_.empty())
        {
            const std::string body =
                img.body.empty() ? img.shortcode : img.body;
            if (thread_panel_ == ThreadPanel::Open &&
                !current_thread_root_.empty())
            {
                client_->send_thread_sticker(current_room_id_,
                                             current_thread_root_, body,
                                             img.url, img.info_json);
            }
            else
            {
                client_->send_sticker(current_room_id_, body, img.url,
                                      img.info_json);
            }
        }
        if (hStickerPicker_)
        {
            ShowWindow(hStickerPicker_, SW_HIDE);
        }
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
        SetWindowPos(s, nullptr, 0, 0, kStickerPickW, kStickerPickH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder("Search stickers");
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (sticker_picker_shared_)
            {
                sticker_picker_shared_->set_search_query(q);
            }
            if (sticker_picker_surface_)
            {
                sticker_picker_surface_->relayout();
            }
        });
    sticker_picker_surface_->set_on_layout(
        [this]
        {
            if (sticker_picker_search_field_ && sticker_picker_shared_)
            {
                tk::Rect r = sticker_picker_shared_->search_field_rect();
                r.x += 1;
                r.y += 1;
                r.w -= 2;
                r.h -= 2;
                sticker_picker_search_field_->set_rect(r);
            }
        });
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
    int x = btn_rc.right - kStickerPickW - 8;
    int y = btn_rc.top - kStickerPickH - 4;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (x + kStickerPickW > mi.rcWork.right)
        {
            x = mi.rcWork.right - kStickerPickW - 4;
        }
        if (y < mi.rcWork.top)
        {
            y = btn_rc.bottom + 4;
        }
        if (y + kStickerPickH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - kStickerPickH - 4;
        }
    }

    SetWindowPos(hStickerPicker_, HWND_TOPMOST, x, y, kStickerPickW,
                 kStickerPickH, SWP_NOACTIVATE);

    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }

    ShowWindow(hStickerPicker_, SW_SHOWNOACTIVATE);
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_focused(true);
    }
}

void MainWindow::refresh_sticker_picker()
{
    if (sticker_picker_shared_)
    {
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

    auto jrv = std::make_unique<tesseract::views::JoinRoomView>();
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
        run_async_(
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

    join_room_surface_->set_root(std::move(jrv));

    if (HWND s = join_room_surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, kJoinRoomPickW, kJoinRoomPickH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    join_room_alias_field_ = join_room_surface_->host().make_text_field();
    join_room_alias_field_->set_placeholder("#room:server.org");
    join_room_alias_field_->set_on_changed(
        [this](const std::string& text)
        {
            if (join_room_shared_)
            {
                join_room_shared_->set_alias_text(text);
            }
        });

    join_room_surface_->set_on_layout(
        [this]
        {
            if (join_room_alias_field_ && join_room_shared_)
            {
                join_room_alias_field_->set_rect(
                    join_room_shared_->alias_field_rect());
                join_room_alias_field_->set_visible(
                    join_room_shared_->alias_field_visible());
            }
        });
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
    }
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_text("");
    }
    if (join_room_shared_)
    {
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
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_focused(true);
    }
}

void MainWindow::refresh_emoji_picker()
{
    if (emoji_picker_shared_)
    {
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
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id)
                {
                    continue;
                }
                name = r.name;
                const std::string& av_mxc = r.effective_avatar_url();
                if (!av_mxc.empty())
                {
                    auto it = tk_avatars_.find(av_mxc);
                    if (it != tk_avatars_.end())
                    {
                        avatar = it->second.get();
                    }
                }
                break;
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

} // namespace win32
