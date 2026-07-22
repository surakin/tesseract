#include "RoomWindow.h"
#include "MainWindow.h"
#include "TextRenderer.h"
#include "Theme.h"
#include "resource.h"
#include "views/ComposePopups.h"

#include "views/PopoutRoomWidget.h"

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

    // Create the D2D surface that fills the entire client area.
    surface_ =
        std::make_unique<tk::win32::Surface>(hInst, hwnd_, tk::Theme::light());

    auto room_widget = tk::create_root_widget<tesseract::views::PopoutRoomWidget>(
        &surface_->host());
    room_view_            = room_widget->room_view();
    img_viewer_           = room_widget->image_viewer();
    vid_viewer_           = room_widget->video_viewer();
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

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
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
            fetch_source_bytes_(src, std::move(on_ready));
        });

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
    room_view_->on_file_clicked =
        [this](tesseract::views::MessageListView::FileHit hit)
    {
        std::wstring suggested(hit.file_name.begin(), hit.file_name.end());
        if (suggested.empty())
            suggested = L"download";
        std::wstring path =
            parent_->show_save_dialog_(suggested, L"All files\0*.*\0\0");
        if (path.empty())
            return;
        std::string url = hit.source ? hit.source->fetch_token() : std::string{};
        save_source_to_file_(std::move(url), wstr_to_utf8(path));
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
            shell_show_status_message_(std::move(reason));
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

    // ── Compose text area (self-owned) ────────────────────────────────────
    text_area_ = room_view_->compose_bar()->text_area();
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
            // Drive all composer popups through the shared priority dispatch
            // (gif > slash > shortcode > mention).
            tesseract::views::dispatch_compose_text_changed(
                s, text_area_->cursor_byte_pos(), gif_controller_.get(),
                slash_controller_.get(), shortcode_controller_.get(),
                mention_controller_.get());
        });
    text_area_->set_on_submit(
        [this]
        {
            if (tesseract::views::dispatch_compose_submit(
                    gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get()))
            {
                return;
            }
            if (room_view_)
            {
                room_view_->compose_bar()->trigger_send();
            }
        });
    text_area_->push_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            return tesseract::views::dispatch_compose_nav(
                nk, gif_controller_.get(), slash_controller_.get(),
                shortcode_controller_.get(), mention_controller_.get());
        });

    // @mention popup + controller (mirrors the main window).
    {
        mention_popup_ = surface_->host().make_popup_surface();
        auto pw = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = pw.get();
        if (mention_popup_)
            mention_popup_->set_root(std::move(pw));

        tesseract::views::MentionController::Hooks hooks;
        hooks.show = [this](tk::Rect cursor, int rows)
        { show_mention_popup_(cursor, rows); };
        hooks.hide = [this] { hide_mention_popup_(); };
        hooks.repaint = [this]
        {
            if (mention_popup_)
                mention_popup_->request_repaint();
        };
        hooks.room_id = [this] { return room_id_; };
        hooks.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        hooks.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        wire_mention_shell_hooks_(mention_popup_widget_, hooks);
        mention_controller_ =
            std::make_unique<tesseract::views::MentionController>(
                text_area_, shell_client_(), mention_popup_widget_,
                std::move(hooks));
    }

    // /command + :shortcode: + /gif composer popups (mirror the main window).
    {
        // ── /command autocomplete popup ───────────────────────────────────
        slash_popup_ = surface_->host().make_popup_surface();
        {
            auto pw = std::make_unique<tesseract::views::SlashCommandPopup>();
            slash_popup_widget_ = pw.get();
            if (slash_popup_)
                slash_popup_->set_root(std::move(pw));
        }
        {
            tesseract::views::SlashCommandController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_slash_popup_(cursor, rows); };
            sh.hide = [this]
            {
                if (slash_popup_)
                    slash_popup_->set_visible(false);
            };
            sh.repaint = [this]
            {
                if (slash_popup_)
                    slash_popup_->request_repaint();
            };
            sh.room_id = [this] { return room_id_; };
            sh.client = [this] { return shell_client_(); };
            sh.clear_composer = [this]
            {
                if (room_view_)
                    room_view_->clear_compose_text();
            };
            sh.on_location = [this] { send_current_location_(); };
            slash_controller_ =
                std::make_unique<tesseract::views::SlashCommandController>(
                    text_area_, slash_popup_widget_, std::move(sh));
        }

        // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────
        shortcode_popup_ = surface_->host().make_popup_surface();
        {
            auto pw = std::make_unique<tesseract::views::ShortcodePopup>();
            shortcode_popup_widget_ = pw.get();
            shortcode_popup_widget_->set_image_provider(
                [this](const std::string& url) -> const tk::Image*
                { return shell_image_(url); });
            if (shortcode_popup_)
                shortcode_popup_->set_root(std::move(pw));
        }
        {
            tesseract::views::ShortcodeController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_shortcode_popup_(cursor, rows); };
            sh.hide = [this]
            {
                if (shortcode_popup_)
                    shortcode_popup_->set_visible(false);
            };
            sh.repaint = [this]
            {
                if (shortcode_popup_)
                    shortcode_popup_->request_repaint();
            };
            sh.emoticons = [this]() { return shell_emoticons_(); };
            sh.fetch_image = [this](const std::string& url)
            { shell_ensure_media_image_(url, 28, 28); };
            shortcode_controller_ =
                std::make_unique<tesseract::views::ShortcodeController>(
                    text_area_, shortcode_popup_widget_, std::move(sh));
        }

        // ── /gif inline result strip ──────────────────────────────────────
        gif_popup_ = surface_->host().make_popup_surface();
        {
            auto pw = std::make_unique<tesseract::views::GifPopup>();
            gif_popup_widget_ = pw.get();
            // Strip cells render via the shell's shared two-stage provider. The
            // repaint refreshes THIS pop-out's surface, self-guarded by the
            // window's liveness token (the shell's fetch may outlive us).
            gif_popup_widget_->set_image_provider(
                [this](const tesseract::GifResult& result) -> const tk::Image*
                {
                    auto alive = alive_;
                    return shell_gif_strip_image_(
                        result,
                        [this, alive]
                        {
                            if (*alive && gif_popup_)
                                gif_popup_->request_repaint();
                        });
                });
            if (gif_popup_)
                gif_popup_->set_root(std::move(pw));
        }
        {
            tesseract::views::GifController::Hooks gh;
            gh.show = [this] { show_gif_popup_(); };
            gh.hide = [this] { hide_gif_popup_(); };
            gh.repaint = [this]
            {
                if (gif_popup_)
                    gif_popup_->request_repaint();
            };
            gh.room_id = [this] { return room_id_; };
            gh.client = [this] { return shell_client_(); };
            gh.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            gh.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            gh.post_delayed = [this](int ms, std::function<void()> fn)
            {
                if (surface_)
                    surface_->host().post_delayed(ms, std::move(fn));
            };
            gh.api_key = []() -> std::string
            { return tesseract::Settings::instance().gif_api_key; };
            gh.client_key = []() -> std::string { return "tesseract"; };
            gh.clear_composer = [this]
            {
                if (text_area_)
                    text_area_->set_text("");
                if (room_view_)
                    room_view_->set_current_text({});
            };
            gh.get_cached_gif_bytes =
                [this](const std::string& url) -> std::vector<std::uint8_t>
            { return shell_cached_gif_bytes_(url); };
            gif_controller_ = std::make_unique<tesseract::views::GifController>(
                text_area_, gif_popup_widget_, std::move(gh));
        }
    }
    // Auto-grow (set_on_height_changed) is wired internally by ComposeBar's
    // own constructor now — see ComposeBar::ComposeBar()'s text_area_ setup.

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
                rif->set_overlay_inset(2.0f);
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
            fpf->set_overlay_inset(2.0f);
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
        if (surface_)
            surface_->set_cursor(url.empty() ? tk::win32::Cursor::Default
                                             : tk::win32::Cursor::Pointer);
    };

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // Defensive: size the surface to the client area explicitly so it's
    // correct even if the initial WM_SIZE fired during CreateWindowExW (before
    // surface_ existed) and SW_SHOW didn't re-fire it. Idempotent with the
    // WM_SIZE handler.
    {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (surface_ && surface_->hwnd())
        {
            SetWindowPos(surface_->hwnd(), nullptr, 0, 0, rc.right, rc.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (mention_popup_)
    {
        mention_popup_->set_theme(t);
    }
}

// ---------------------------------------------------------------------------

void RoomWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_)
    {
        return;
    }
    const float w = tesseract::views::MentionPopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::MentionPopup::kRowHeight;
    mention_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    mention_popup_->set_visible(true);
}

void RoomWindow::hide_mention_popup_()
{
    if (mention_popup_)
    {
        mention_popup_->set_visible(false);
    }
}

void RoomWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    if (!slash_popup_)
    {
        return;
    }
    const float w = tesseract::views::SlashCommandPopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::SlashCommandPopup::kRowHeight;
    slash_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    slash_popup_->set_visible(true);
}

void RoomWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    if (!shortcode_popup_)
    {
        return;
    }
    const float w = tesseract::views::ShortcodePopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::ShortcodePopup::kRowHeight;
    shortcode_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    shortcode_popup_->set_visible(true);
}

void RoomWindow::show_gif_popup_()
{
    if (!gif_popup_ || !gif_popup_widget_ || !surface_ || !room_view_)
    {
        return;
    }
    // Full-width strip just above the compose bar. compose_bar_rect is in
    // layout (DIP) coords, so scale to physical pixels for this window's DPI
    // before handing it to set_rect (unlike cursor_local for mention/slash/
    // shortcode, which is already physical — cursor_rect() uses MapWindowPoints).
    const tk::Rect cb = room_view_->compose_bar_rect();
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float scale = dpi > 0.f ? dpi / 96.f : 1.f;
    const tk::Rect cb_px{cb.x * scale, cb.y * scale, cb.w * scale, cb.h * scale};
    const float h_px = sz.h * scale;
    gif_popup_->set_rect(cb_px, {cb_px.w, h_px}, tk::PopupPlacement::PreferAbove);
    gif_popup_->set_visible(true);
}

void RoomWindow::hide_gif_popup_()
{
    if (gif_popup_)
    {
        gif_popup_->set_visible(false);
    }
}

void RoomWindow::on_gif_results(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void RoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                      const std::string& message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, message);
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
    // Inline animated media in the timeline.
    surface_repaint_();
    // Visible picker: invalidate the shared view's image cache so animated
    // sticker/emoticon cells advance (surface_repaint_() above already
    // covers repainting it, since it's part of this window's own surface
    // now rather than a separate popup).
    if (room_view_)
    {
        if (room_view_->emoji_picker_visible() && room_view_->emoji_picker())
            room_view_->emoji_picker()->invalidate_image_cache();
        if (room_view_->sticker_picker_visible() && room_view_->sticker_picker())
            room_view_->sticker_picker()->invalidate_image_cache();
    }
    // Advance the /gif strip's animated cells (frames come from the shared
    // anim cache; the shell's tick fires this for every window). No
    // set_anim_cache wired for this pop-out's popup (unlike MainWindow's),
    // so a plain full repaint is used rather than update_anim_regions()'s
    // partial-redraw path — it still picks up the shared cache's already-
    // centrally-advanced current frame, just without the per-region
    // optimization.
    if (gif_popup_ && gif_popup_->visible())
    {
        gif_popup_->request_repaint();
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
            RECT wrc{};
            GetWindowRect(hwnd, &wrc);
            save_popout_geometry_(wrc.left, wrc.top,
                                  wrc.right - wrc.left, wrc.bottom - wrc.top,
                                  static_cast<int>(GetDpiForWindow(hwnd)));
        }
        return 0;

    case WM_MOVE:
    {
        RECT wrc{};
        GetWindowRect(hwnd, &wrc);
        save_popout_geometry_(wrc.left, wrc.top,
                              wrc.right - wrc.left, wrc.bottom - wrc.top,
                              static_cast<int>(GetDpiForWindow(hwnd)));
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
        return 1; // surface child covers the entire client area

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (room_view_ && room_view_->room_search_open())
            {
                room_view_->close_room_search();
                return 0;
            }
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
