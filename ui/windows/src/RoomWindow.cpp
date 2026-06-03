#include "RoomWindow.h"
#include "MainWindow.h"
#include "TextRenderer.h"
#include "Theme.h"

#include "views/PopoutRoomWidget.h"

#include <algorithm>
#include <string>

namespace win32
{

bool RoomWindow::class_registered_ = false;

// ---------------------------------------------------------------------------

static std::wstring utf8_to_wstr(const std::string& s)
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
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string wstr_to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n,
                        nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------

RoomWindow::RoomWindow(MainWindow* parent, const std::string& room_id)
    : tesseract::RoomWindowBase(parent, room_id), parent_(parent)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!class_registered_)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc_;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        class_registered_ = true;
    }

    hwnd_ = CreateWindowExW(
        0, kClassName,
        utf8_to_wstr(room_id).c_str(), // title filled in by set_room
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr,
        nullptr, hInst, this);

    if (!hwnd_)
    {
        return;
    }

    // Create the D2D surface that fills the entire client area.
    surface_ =
        std::make_unique<tk::win32::Surface>(hInst, hwnd_, tk::Theme::light());

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>();
    room_view_  = room_widget->room_view();
    img_viewer_ = room_widget->image_viewer();
    vid_viewer_ = room_widget->video_viewer();
    surface_->set_root(std::move(room_widget));

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
    }

    // ── Image / video save dialogs ────────────────────────────────────────
    img_viewer_->on_save =
        [this](std::string source_url, std::string filename_hint)
    {
        std::wstring suggested(filename_hint.begin(), filename_hint.end());
        if (suggested.empty())
            suggested = L"image";
        std::wstring path = parent_->show_save_dialog_(
            suggested,
            L"Images\0*.jpg;*.jpeg;*.png;*.gif;*.webp\0All files\0*.*\0\0");
        if (!path.empty())
            save_source_to_file_(std::move(source_url),
                                  wstr_to_utf8(path));
    };
    vid_viewer_->on_save =
        [this](std::string source_json, std::string mime_type)
    {
        std::wstring suggested = L"video";
        if (mime_type == "video/mp4")
            suggested = L"video.mp4";
        else if (mime_type == "video/webm")
            suggested = L"video.webm";
        std::wstring path = parent_->show_save_dialog_(
            suggested,
            L"Videos\0*.mp4;*.webm;*.mkv\0All files\0*.*\0\0");
        if (!path.empty())
            save_source_to_file_(std::move(source_json),
                                  wstr_to_utf8(path));
    };

    // ── Surface-bound providers (need this shell's own surface_) ─────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }
    room_view_->set_post_delayed(
        [this](int ms, std::function<void()> fn)
        {
            if (surface_)
            {
                surface_->host().post_delayed(ms, std::move(fn));
            }
        });
    room_view_->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    room_view_->on_set_clipboard = [this](std::string_view t)
    {
        if (surface_)
            surface_->host().set_clipboard_text(t);
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

    // ── NativeTextArea overlay ────────────────────────────────────────────
    text_area_ = surface_->host().make_text_area();
    text_area_->set_placeholder("Message\xe2\x80\xa6");
    text_area_->set_on_changed(
        [this](const std::string& s)
        {
            bool typing = !s.empty();
            if (typing != compose_typing_active_)
            {
                compose_typing_active_ = typing;
                send_typing_notice_(typing);
            }
            room_view_->set_current_text(s);
            if (mention_controller_)
            {
                mention_controller_->on_text_changed(
                    s, text_area_->cursor_byte_pos());
            }
        });
    text_area_->set_on_submit(
        [this]
        {
            if (mention_controller_ && mention_controller_->on_submit())
            {
                return;
            }
            if (room_view_)
            {
                room_view_->compose_bar()->trigger_send();
            }
        });
    text_area_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        { return mention_controller_ && mention_controller_->on_nav(nk); });

    // @mention popup + controller (mirrors the main window).
    {
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
        mention_popup_hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, 0, 0,
            int(tesseract::views::MentionPopup::kWidth),
            int(tesseract::views::MentionPopup::kRowHeight), nullptr, nullptr,
            inst, nullptr);
        mention_popup_surface_ = std::make_unique<tk::win32::Surface>(
            inst, mention_popup_hwnd_, surface_->theme());
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
        hooks.room_id = [this] { return room_id_; };
        hooks.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        hooks.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        wire_mention_shell_hooks_(mention_popup_widget_, hooks);
        mention_controller_ =
            std::make_unique<tesseract::views::MentionController>(
                text_area_.get(), shell_client_(), mention_popup_widget_,
                std::move(hooks));
    }
    text_area_->set_on_height_changed(
        [this](float h)
        {
            if (room_view_)
            {
                room_view_->set_text_area_natural_height(h);
            }
            if (surface_)
            {
                surface_->relayout();
            }
        });

    surface_->set_on_layout(
        [this]
        {
            if (room_view_ && text_area_)
            {
                text_area_->set_rect(room_view_->compose_text_area_rect());
            }
        });

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    finish_init_();
}

RoomWindow::~RoomWindow()
{
    // If the HWND is still alive (e.g. programmatic close that bypassed
    // WM_DESTROY), destroy it now. WM_DESTROY sets hwnd_ = nullptr to prevent
    // re-entry.
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------

void RoomWindow::bring_to_front()
{
    if (hwnd_)
    {
        if (IsIconic(hwnd_))
        {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        SetForegroundWindow(hwnd_);
    }
}

void RoomWindow::close_window()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
    }
}

void RoomWindow::request_relayout()
{
    if (surface_)
    {
        surface_->relayout();
    }
}

void RoomWindow::update_window_title_(const std::string& name)
{
    if (hwnd_)
    {
        SetWindowTextW(hwnd_, utf8_to_wstr(name).c_str());
    }
}

void RoomWindow::apply_theme(const tk::Theme& t)
{
    // The global win32 native palette/mode is already set by the main
    // shell's apply_theme_ui_() (which calls this). Here we just re-skin
    // this pop-out's own chrome: dark caption + control theme + repaint,
    // then push the tk theme into the surface.
    if (hwnd_)
    {
        win32::theme::apply_window_attributes(hwnd_);
        win32::theme::apply_control_theme(hwnd_);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    if (surface_)
    {
        surface_->set_theme(t);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
    }
}

// ---------------------------------------------------------------------------

void RoomWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_hwnd_ || !surface_)
    {
        return;
    }
    int w = int(tesseract::views::MentionPopup::kWidth);
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);
    POINT pt{LONG(cursor_local.x), LONG(cursor_local.y)};
    ClientToScreen(surface_->hwnd(), &pt);
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
}

void RoomWindow::hide_mention_popup_()
{
    if (mention_popup_hwnd_)
    {
        ShowWindow(mention_popup_hwnd_, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------

void RoomWindow::surface_repaint_()
{
    if (surface_)
    {
        InvalidateRect(surface_->hwnd(), nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK RoomWindow::wnd_proc_(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    RoomWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RoomWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<RoomWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self)
    {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self->handle_msg_(hwnd, msg, wParam, lParam);
}

LRESULT RoomWindow::handle_msg_(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
{
    switch (msg)
    {
    case WM_DPICHANGED:
    {
        win32::text::on_dpi_changed(LOWORD(wParam));
        theme::on_dpi_changed();
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && surface_)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (HWND sh = surface_->hwnd())
            {
                SetWindowPos(sh, nullptr, 0, 0, rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            surface_->relayout();
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // surface child covers the entire client area

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (vid_viewer_ && vid_viewer_->is_open())
            {
                vid_viewer_->close();
                vid_viewer_->set_visible(false);
                if (surface_)
                    surface_->relayout();
                return 0;
            }
            if (img_viewer_ && img_viewer_->is_open())
            {
                img_viewer_->close();
                img_viewer_->set_visible(false);
                if (surface_)
                    surface_->relayout();
                return 0;
            }
        }
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (room_view_ && room_view_->message_list()->has_selection())
            {
                room_view_->message_list()->copy_selection();
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        hwnd_ = nullptr;
        schedule_self_close_();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
