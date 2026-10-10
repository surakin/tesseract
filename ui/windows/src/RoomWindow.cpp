#include "RoomWindow.h"
#include "MainWindow.h"
#include "Win32Taskbar.h"
#include "Theme.h"
#include "resource.h"
#include "app/ComposerPopups.h"

#include "views/PopoutRoomWidget.h"
#include "views/media_viewer_items.h"

#include "tk/i18n.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>
#include <tesseract/settings.h>

#include <commctrl.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
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
        // Match the main window: big icon (Alt+Tab/taskbar) + small icon
        // (titlebar/system menu), from the multi-resolution .ico.
        wc.hIcon = static_cast<HICON>(
            LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                       LR_DEFAULTCOLOR | LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
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
        RegisterClassExW(&wc);
        class_registered_ = true;
    }

    // Determine the target DPI from the monitor at the saved position before
    // the window exists, then call the DPI-aware restore helper so w/h are
    // scaled from the save-time DPI to the current monitor's DPI.
    int targetDpi = 0;
    {
        const auto& pops = tesseract::Settings::instance().popout_windows;
        auto it = std::find_if(
            pops.begin(), pops.end(),
            [&room_id](const tesseract::Settings::PopoutEntry& e)
            { return e.room_id == room_id; });
        if (it != pops.end() && it->geometry.valid && it->geometry.dpi > 0)
        {
            POINT pt{it->geometry.x, it->geometry.y};
            HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            UINT ux = 96, uy = 0;
            GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &ux, &uy);
            targetDpi = static_cast<int>(ux);
        }
    }
    const auto saved = get_saved_popout_geometry_(800, 600, targetDpi);
    hwnd_ = CreateWindowExW(
        0, kClassName,
        utf8_to_wstr(room_id).c_str(), // title filled in by set_room
        WS_OVERLAPPEDWINDOW,
        saved.valid ? saved.x : CW_USEDEFAULT,
        saved.valid ? saved.y : CW_USEDEFAULT,
        saved.valid ? saved.w : 800,
        saved.valid ? saved.h : 600,
        nullptr, nullptr, hInst, this);

    if (!hwnd_)
    {
        return;
    }
    title_bar_.on_dpi_changed(GetDpiForWindow(hwnd_));
    title_bar_.attach(hwnd_);

    parent_->taskbar().register_room_window(hwnd_);

    // Create the D2D surface that fills the entire client area.
    surface_ =
        std::make_unique<tk::win32::Surface>(hInst, hwnd_, tk::Theme::light());

    auto room_widget = tk::create_root_widget<tesseract::views::PopoutRoomWidget>(
        &surface_->host());
    room_view_            = room_widget->room_view();
    media_viewer_         = room_widget->media_viewer();
    forward_picker_widget_ = room_widget->forward_picker();
    room_media_view_widget_ = room_widget->room_media_view();
    confirm_dialog_widget_ = room_widget->confirm_dialog();
    room_widget->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    surface_->set_root(std::move(room_widget));

    // ── Shared per-room wiring, via RoomPane ──────────────────────────────
    init_pane_(&surface_->host());
    pane_->attach({
        .room_view = room_view_,
        .media_viewer = media_viewer_,
        .forward_picker = forward_picker_widget_,
        .room_media_view = room_media_view_widget_,
        .focus_forward_picker_field = [this]
        {
            if (!forward_picker_widget_)
                return;
            if (auto* f = forward_picker_widget_->search_field())
            {
                f->set_text("");
                f->set_focused(true);
            }
        },
        .hide_forward_picker_field = [this]
        {
            if (forward_picker_widget_)
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
            if (hwnd_)
            {
                SetFocus(hwnd_);
            }
        },
    });

    // ── Video player for this window's MediaViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        media_viewer_->set_video_player(std::move(player));
    }

    // Inline autoplay video/GIF in the timeline (separate from the lightbox
    // player above — MessageListView falls back to a static thumbnail unless
    // both of these are set).
    room_view_->set_video_player_factory(
        [this]() { return surface_->host().make_video_player(); });
    room_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
        {
            pane_->fetch_source_bytes_(src, std::move(on_ready));
        });

    // ── Media save dialog ─────────────────────────────────────────────────
    // One handler for every viewer kind: RoomPane builds the spec and writes
    // the file; this window only shows the dialog.
    pane_->install_media_viewer_save_(
        [this](const tesseract::views::MediaSaveSpec& spec,
               std::function<void(std::string)> done)
        {
            const std::wstring filter = MainWindow::save_filter_for(spec);
            std::wstring path = parent_->show_save_dialog_(
                utf8_to_wstr(spec.suggested_name), filter.c_str());
            if (!path.empty())
            {
                done(wstr_to_utf8(path));
            }
        });

    // Full-screen toggle acts on this pop-out window.
    const auto set_fs = [this](bool on) { set_window_fullscreen_impl_(on); };
    media_viewer_->on_request_fullscreen = set_fs;

    room_view_->on_file_clicked =
        [this](tesseract::views::MessageListView::FileHit hit)
    {
        std::wstring suggested(hit.file_name.begin(), hit.file_name.end());
        if (suggested.empty())
            suggested = L"download";
        std::wstring path =
            parent_->show_save_dialog_(suggested, MainWindow::file_filter({{tk::tr("All files"), L"*.*"}}).c_str());
        if (path.empty())
            return;
        std::string url = hit.source ? hit.source->fetch_token() : std::string{};
        pane_->save_source_to_file_(std::move(url), wstr_to_utf8(path));
    };

    // ── Surface-bound providers (need this shell's own surface_) ─────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }

    // Drag-and-drop file ingest is now tree-dispatched automatically (see
    // DropTarget::Drop -> Host::fire_file_drop -> Host::dispatch_file_drop);
    // RoomView::on_file_drop routes into this window's compose bar via the
    // provider fields wired in wire_room_view_. Pop-outs never open their
    // RoomSettingsView, so it's always invisible and never claims a drop
    // ahead of the compose bar.
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            pane_->shell_show_status_message_(std::move(reason));
        });

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
    room_view_->on_selection_started = [this]()
    {
        if (surface_)
            surface_->host().release_focus_to_canvas();
    };
    room_view_->on_show_copy_menu = [this]()
    {
        if (!room_view_)
            return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, tk::win32::utf8_to_wide(tk::tr("Copy")).c_str());
        POINT pt{};
        GetCursorPos(&pt);
        int cmd = static_cast<int>(TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            pt.x, pt.y, hwnd_, nullptr));
        DestroyMenu(menu);
        if (cmd == 1)
            room_view_->copy_active_selection();
    };

    // ── Compose text area (self-owned) + composer popups ──────────────────
    text_area_ = room_view_->compose_bar()->text_area();
    popups_ = std::make_unique<tesseract::ComposerPopups>(
        surface_->host(), text_area_, pane_.get(), room_view_);

    surface_->set_on_layout(
        [this]
        {
            // Native child controls always paint over canvas-drawn overlays,
            // so hide them while the confirm dialog covers the window —
            // otherwise the compose box/search fields would poke through on
            // top of the modal backdrop. text_area_ self-positions via
            // ComposeBar::arrange() otherwise (reached via the relayout this
            // set_on_layout callback runs after), so only a force-hide is
            // needed here — mirrors the search fields' own gating below.
            const bool confirm_open =
                confirm_dialog_widget_ && confirm_dialog_widget_->is_open();
            if (confirm_open && text_area_)
            {
                text_area_->set_visible(false);
            }
            if (confirm_open && room_view_)
            {
                // Search field self-positions via RoomSearchBar::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* bar = room_view_->room_search_bar())
                    if (auto* f = bar->search_field())
                        f->set_visible(false);
            }
            if (confirm_open && forward_picker_widget_)
            {
                // Search field self-positions via ForwardRoomPicker::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
            }
        });

    // Per-room "find in conversation" — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here (on_close is already
    // wired internally by RoomView's own constructor).
    if (room_view_)
    {
        if (auto* bar = room_view_->room_search_bar())
        {
            if (auto* rif = bar->search_field())
            {
                rif->push_popup_nav(
                    [this](tk::NavKey nk) -> bool
                    {
                        if (!room_view_ || !room_view_->room_search_open())
                            return false;
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(-1);
                            return true;
                        case tk::NavKey::Down:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(+1);
                            return true;
                        case tk::NavKey::Escape:
                            room_view_->close_room_search();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
        }
    }

    // Forward-message picker — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here.
    if (forward_picker_widget_)
    {
        if (auto* fpf = forward_picker_widget_->search_field())
        {
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    if (!forward_picker_widget_ || !forward_picker_widget_->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        forward_picker_widget_->move_selection(-1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        forward_picker_widget_->move_selection(+1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        forward_picker_widget_->close();
                        return true;
                    default:
                        return false;
                    }
                });
        }
    }

    room_view_->on_link_hovered = [this](const std::string& url)
    {
        if (!surface_) return;
        using HC = tesseract::views::MessageListView::HoverCursor;
        const HC hc = tesseract::views::MessageListView::hover_cursor_for(url);
        surface_->set_cursor(hc == HC::Text      ? tk::win32::Cursor::IBeam
                             : hc == HC::Pointer ? tk::win32::Cursor::Pointer
                                                 : tk::win32::Cursor::Default);
    };

    // Force a WM_NCCALCSIZE recompute before the window is shown, in case
    // its size/position ended up unchanged from what CreateWindowExW already
    // used (Windows never re-fires WM_NCCALCSIZE for a no-op SetWindowPos).
    // Without this the custom-titlebar-hiding logic in
    // CustomTitleBar::adjust_nccalcsize can miss its only chance to run
    // before first paint, leaving the native caption visible alongside the
    // custom one until the window is resized. See MainWindow::create() for
    // the matching fix and full explanation.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // Defensive: size the surface to the client area explicitly so it's
    // correct even if the initial WM_SIZE fired during CreateWindowExW (before
    // surface_ existed) and SW_SHOW didn't re-fire it. Idempotent with the
    // WM_SIZE handler.
    {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int titlebar_h = fs_active_ ? 0 : title_bar_.height_px();
        if (surface_ && surface_->hwnd())
        {
            SetWindowPos(surface_->hwnd(), nullptr, 0, titlebar_h, rc.right,
                         rc.bottom - titlebar_h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (surface_)
        {
            surface_->relayout();
        }
    }

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
        surface_->root()->apply_theme(t);
    }
    if (popups_)
    {
        popups_->apply_theme(t);
    }
}

void RoomWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

// ---------------------------------------------------------------------------

void RoomWindow::on_gif_results(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
{
    if (popups_)
    {
        popups_->on_gif_results(request_id, std::move(results));
    }
}

void RoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                      const std::string& message)
{
    if (popups_)
    {
        popups_->on_gif_search_failed(request_id, message);
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

void RoomWindow::repaint_anim_frame()
{
    RoomWindowBase::repaint_anim_frame();
    if (popups_)
    {
        popups_->repaint_anim_frame();
    }
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK RoomWindow::wnd_proc_(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    if (msg == WM_GETMINMAXINFO)
    {
        // Enforce the same room-content floor as MainWindow (see its
        // wnd_proc) — can fire before WM_NCCREATE sets GWLP_USERDATA.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.f : 1.f;
        mmi->ptMinTrackSize.x = static_cast<LONG>(
            std::round(tesseract::visual::kMinWindowWidth * scale));
        mmi->ptMinTrackSize.y = static_cast<LONG>(
            std::round(tesseract::visual::kMinWindowHeight * scale));
        return 0;
    }

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

void RoomWindow::set_window_fullscreen_impl_(bool on)
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
}

LRESULT RoomWindow::handle_msg_(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
{
    if (msg == parent_->taskbar().taskbar_button_created_message())
    {
        parent_->taskbar().on_taskbar_button_created(hwnd);
        return 0;
    }
    switch (msg)
    {
    // ── Custom extended title bar (see CustomTitleBar.h) ───────────────────
    case WM_NCCALCSIZE:
        if (wParam == TRUE && !fs_active_)
        {
            title_bar_.adjust_nccalcsize(hwnd, wParam, lParam);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCHITTEST:
    {
        if (auto dwm = CustomTitleBar::dwm_hittest_hook(hwnd, msg, wParam,
                                                        lParam))
            return *dwm;
        const LRESULT ht = title_bar_.handle_nchittest(hwnd, lParam);
        if (ht != HTNOWHERE)
            return ht;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCMOUSEMOVE:
        title_bar_.handle_ncmousemove(hwnd, wParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCMOUSELEAVE:
        title_bar_.handle_ncmouseleave(hwnd);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCLBUTTONDOWN:
    {
        if (auto dwm = CustomTitleBar::dwm_hittest_hook(hwnd, msg, wParam,
                                                        lParam))
            return *dwm;
        if (title_bar_.handle_nclbuttondown(hwnd, wParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCLBUTTONUP:
    {
        if (auto dwm = CustomTitleBar::dwm_hittest_hook(hwnd, msg, wParam,
                                                        lParam))
            return *dwm;
        if (title_bar_.handle_nclbuttonup(hwnd, wParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCRBUTTONDOWN:
        if (auto dwm = CustomTitleBar::dwm_hittest_hook(hwnd, msg, wParam,
                                                        lParam))
            return *dwm;
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCRBUTTONUP:
    {
        if (auto dwm = CustomTitleBar::dwm_hittest_hook(hwnd, msg, wParam,
                                                        lParam))
            return *dwm;
        if (title_bar_.show_system_menu(hwnd, wParam, lParam))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_ACTIVATE:
    {
        const bool active = LOWORD(wParam) != WA_INACTIVE;
        if (is_active_ != active)
        {
            is_active_ = active;
            RECT title_rc{};
            GetClientRect(hwnd, &title_rc);
            title_rc.bottom = title_bar_.height_px();
            InvalidateRect(hwnd, &title_rc, TRUE);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_DPICHANGED:
    {
        theme::on_dpi_changed();
        title_bar_.on_dpi_changed(LOWORD(wParam));
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // This is an independent top-level window the user can drag to a
        // different-DPI monitor on its own — handle its own scale change
        // directly rather than relying on the main shell to propagate one
        // (which only knows its own monitor's scale).
        apply_scale_change(static_cast<float>(LOWORD(wParam)) / 96.0f);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && surface_)
        {
            // See CustomTitleBar::invalidate_strip's comment: the button
            // rects are right-anchored, so a live resize drag would
            // otherwise leave the old button position ghosted mid-strip
            // until the drag ends.
            title_bar_.invalidate_strip(hwnd);

            RECT rc;
            GetClientRect(hwnd, &rc);
            const int titlebar_h = fs_active_ ? 0 : title_bar_.height_px();
            if (HWND sh = surface_->hwnd())
            {
                // SetWindowPos() below synchronously dispatches WM_SIZE to
                // the child surface whenever its size actually changes,
                // which already triggers a full relayout + synchronous
                // repaint (Host::on_resize()). Only relayout here ourselves
                // when the target size matches what the surface already
                // has, so WM_SIZE won't fire and nobody else will do it —
                // otherwise every tick of a live resize drag would redo the
                // same measure+arrange+paint pass twice.
                RECT before{};
                GetClientRect(sh, &before);
                const int target_h = rc.bottom - titlebar_h;
                const bool needs_relayout =
                    before.right == rc.right && before.bottom == target_h;
                SetWindowPos(sh, nullptr, 0, titlebar_h, rc.right, target_h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (needs_relayout) surface_->relayout();
            }

            // A popup's screen position is captured once when it opens and
            // never recomputed — leaving one open across a resize would
            // strand it away from whatever control anchored it. Close
            // everything instead (see MainWindow::on_size for the same fix
            // in the main window).
            surface_->host().dismiss_active_popup();
            if (room_view_) room_view_->dismiss_popups();
            if (popups_)
            {
                popups_->hide_all();
            }

            // surface_ now flushes its own repaint synchronously on resize
            // (Host::on_resize()'s UpdateWindow() call), so its
            // DirectComposition-presented content is never a stale frame
            // behind. The title bar (painted directly on hwnd, via
            // WM_ERASEBKGND) is still on the classic invalidate-and-wait
            // path, which left it lagging a frame or two behind the now-
            // faster surface — visible as a flicker/seam right as a resize
            // drag ends. Flush it synchronously too so both land in the
            // same frame.
            UpdateWindow(hwnd);
            RECT wrc{};
            GetWindowRect(hwnd, &wrc);
            save_popout_geometry_(wrc.left, wrc.top,
                                  wrc.right - wrc.left, wrc.bottom - wrc.top,
                                  static_cast<int>(GetDpiForWindow(hwnd)));
        }
        return 0;

    case WM_MOVE:
    {
        if (!IsIconic(hwnd))
        {
            RECT wrc{};
            GetWindowRect(hwnd, &wrc);
            save_popout_geometry_(wrc.left, wrc.top,
                                  wrc.right - wrc.left, wrc.bottom - wrc.top,
                                  static_cast<int>(GetDpiForWindow(hwnd)));
        }
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        // The surface child covers everything below the title bar strip;
        // the strip itself is real client area now (see CustomTitleBar.h)
        // that no child HWND paints over, so it needs a real fill here.
        //
        // The fill and title_bar_.paint() are two separate GDI draws —
        // DWM's compositor can sample this window's redirection surface
        // between them (confirmed live, via debugger, on MainWindow's
        // identical pattern), showing the plain background for one
        // composited frame before the title bar content lands even though
        // no message is dispatched between the two calls. Double-buffer:
        // paint both into an off-screen DC first and blit the finished
        // frame across in one atomic call.
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp =
            CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);
        const auto& pal = theme::palette();
        FillRect(mem_dc, &rc, theme::brush(pal.window_bg));
        if (!fs_active_)
            title_bar_.paint(mem_dc, hwnd, rc, is_active_);
        BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
              mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
        return 1;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (room_view_ && room_view_->room_search_open())
            {
                room_view_->close_room_search();
                return 0;
            }
            if (close_media_viewer_if_open_())
            {
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        parent_->taskbar().unregister_window(hwnd);
        hwnd_ = nullptr;
        schedule_self_close_();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
