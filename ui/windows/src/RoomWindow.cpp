#include "RoomWindow.h"
#include "MainWindow.h"
#include "TextRenderer.h"

#include "views/RoomView.h"
#include <tesseract/client.h>

#include <string>

namespace win32 {

bool RoomWindow::class_registered_ = false;

// ---------------------------------------------------------------------------

static std::wstring utf8_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ---------------------------------------------------------------------------

RoomWindow::RoomWindow(MainWindow* parent, const std::string& room_id)
    : tesseract::RoomWindowBase(parent, room_id)
    , parent_(parent)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!class_registered_) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = wnd_proc_;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        class_registered_ = true;
    }

    hwnd_ = CreateWindowExW(
        0, kClassName,
        utf8_to_wstr(room_id).c_str(),   // title filled in by set_room
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInst, this);

    if (!hwnd_) return;

    // Create the D2D surface that fills the entire client area.
    surface_ = std::make_unique<tk::win32::Surface>(hInst, hwnd_, tk::Theme::light());

    auto room_root = std::make_unique<tesseract::views::RoomView>();
    room_view_ = room_root.get();
    surface_->set_root(std::move(room_root));

    // ── RoomView providers (share caches with the main shell) ────────────
    room_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = shell_->tk_avatars_.find(mxc);
            return it == shell_->tk_avatars_.end() ? nullptr : it->second.get();
        });
    room_view_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            if (auto* f = shell_->anim_cache_.current_frame(mxc)) return f;
            auto it = shell_->tk_images_.find(mxc);
            return it == shell_->tk_images_.end() ? nullptr : it->second.get();
        });
    room_view_->set_preview_provider(
        [this](const std::string& url) -> const tesseract::views::UrlPreviewData* {
            // URL previews are cached in the main shell's url_preview_data_.
            // RoomWindowBase is a friend of ShellBase; access via parent_.
            auto it = parent_->url_preview_data_.find(url);
            return it == parent_->url_preview_data_.end() ? nullptr : &it->second;
        });
    if (auto player = surface_->host().make_audio_player())
        room_view_->set_audio_player(std::move(player));
    room_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return shell_->client_->fetch_source_bytes(source_json);
        });

    // ── Repaint / layout ─────────────────────────────────────────────────
    room_view_->set_repaint_requester([this] {
        if (surface_) InvalidateRect(surface_->hwnd(), nullptr, FALSE);
    });
    room_view_->on_layout_changed = [this] {
        if (surface_) surface_->relayout();
    };

    // ── Compose callbacks ────────────────────────────────────────────────
    room_view_->on_send = [this](const std::string& body) {
        std::string trimmed = body;
        auto l = trimmed.find_first_not_of(" \t\n\r");
        auto r = trimmed.find_last_not_of (" \t\n\r");
        if (l == std::string::npos) return;
        trimmed = trimmed.substr(l, r - l + 1);
        if (trimmed.empty()) return;
        send_message_(trimmed);
        if (text_area_) text_area_->set_text("");
        room_view_->set_current_text({});
    };
    room_view_->on_send_reply = [this](const std::string& reply_id,
                                        const std::string& body) {
        if (body.empty()) return;
        send_reply_(reply_id, body);
        if (text_area_) text_area_->set_text("");
        room_view_->set_current_text({});
    };
    room_view_->on_send_edit = [this](const std::string& event_id,
                                       const std::string& new_body) {
        if (new_body.empty()) return;
        send_edit_(event_id, new_body);
        if (text_area_) text_area_->set_text("");
        room_view_->set_current_text({});
    };
    room_view_->on_edit_cancelled = [this] {
        if (text_area_) text_area_->set_text("");
        room_view_->set_current_text({});
    };
    room_view_->on_edit_prefill = [this](const std::string& body) {
        if (text_area_) text_area_->set_text(body);
    };
    room_view_->on_reply_focus = [this] {
        if (text_area_) text_area_->set_focused(true);
    };
    room_view_->on_delete_requested = [this](const std::string& event_id) {
        delete_event_(event_id);
    };
    room_view_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            toggle_reaction_(event_id, key);
        };
    room_view_->on_receipt_needed = [this](const std::string& event_id) {
        send_receipt_(event_id);
    };
    room_view_->on_link_clicked = [](const std::string& url) {
        tesseract::Client::open_in_browser(url);
    };
    room_view_->on_near_top = [this] {
        request_pagination_back_();
    };

    // ── NativeTextArea overlay ────────────────────────────────────────────
    text_area_ = surface_->host().make_text_area();
    text_area_->set_placeholder("Message\xe2\x80\xa6");
    text_area_->set_on_changed([this](const std::string& s) {
        bool typing = !s.empty();
        if (typing != compose_typing_active_) {
            compose_typing_active_ = typing;
            if (shell_->client_)
                shell_->client_->send_typing_notice(room_id_, typing);
        }
        room_view_->set_current_text(s);
    });
    text_area_->set_on_submit([this] {
        if (room_view_) room_view_->compose_bar()->trigger_send();
    });
    text_area_->set_on_height_changed([this](float h) {
        if (room_view_) room_view_->set_text_area_natural_height(h);
        if (surface_) surface_->relayout();
    });

    surface_->set_on_layout([this] {
        if (room_view_ && text_area_)
            text_area_->set_rect(room_view_->compose_text_area_rect());
    });

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    finish_init_();
}

RoomWindow::~RoomWindow() {
    // If the HWND is still alive (e.g. programmatic close that bypassed
    // WM_DESTROY), destroy it now. WM_DESTROY sets hwnd_ = nullptr to prevent
    // re-entry.
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------

void RoomWindow::bring_to_front() {
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }
}

void RoomWindow::close_window() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void RoomWindow::request_relayout() {
    if (surface_) surface_->relayout();
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK RoomWindow::wnd_proc_(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam) {
    RoomWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RoomWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RoomWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handle_msg_(hwnd, msg, wParam, lParam);
}

LRESULT RoomWindow::handle_msg_(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && surface_) {
            RECT rc; GetClientRect(hwnd, &rc);
            if (HWND sh = surface_->hwnd())
                SetWindowPos(sh, nullptr, 0, 0,
                             rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            surface_->relayout();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // surface child covers the entire client area

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        hwnd_ = nullptr;
        schedule_self_close_();
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace win32
