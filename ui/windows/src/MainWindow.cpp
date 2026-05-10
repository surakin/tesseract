#include "MainWindow.h"
#include "LoginDialog.h"

#include <tesseract/session_store.h>

#include <windowsx.h>
#include <string>

namespace win32 {

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_message(tesseract::Event* ev) {
    auto* p = ev;
    PostMessage(hwnd_, WM_TESSERACT_MESSAGE, 0, reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_rooms_updated(
    const std::vector<tesseract::RoomInfo>& rooms)
{
    auto* p = new std::vector<tesseract::RoomInfo>(rooms);
    PostMessage(hwnd_, WM_TESSERACT_ROOMS, 0, reinterpret_cast<LPARAM>(p));
}

void EventHandler::on_sync_error(
    const std::string& context,
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

// ---------------------------------------------------------------------------
// MainWindow
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

LRESULT CALLBACK MainWindow::wnd_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
        if (HIWORD(wParam) == LBN_SELCHANGE && LOWORD(wParam) == IDC_ROOMLIST) {
            int idx = static_cast<int>(
                SendMessageW(self->hRoomList_, LB_GETCURSEL, 0, 0));
            if (idx != LB_ERR) self->on_room_selected(idx);
        }
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

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------

MainWindow::MainWindow(HINSTANCE hInst) : hInst_(hInst) {}

MainWindow::~MainWindow() {
    client_.stop_sync();
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
    hRoomList_ = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        0, 0, 200, 600,
        hwnd, reinterpret_cast<HMENU>(IDC_ROOMLIST), hInst_, nullptr);

    hMsgView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        200, 0, 600, 700,
        hwnd, reinterpret_cast<HMENU>(IDC_MSGVIEW), hInst_, nullptr);

    hInput_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        200, 700, 500, 30,
        hwnd, reinterpret_cast<HMENU>(IDC_INPUT), hInst_, nullptr);

    hSend_ = CreateWindowExW(
        0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        700, 700, 100, 30,
        hwnd, reinterpret_cast<HMENU>(IDC_SEND), hInst_, nullptr);

    hStatus_ = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"Not logged in",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        hwnd, nullptr, hInst_, nullptr);

    on_login_clicked();
}

void MainWindow::on_destroy() {
    client_.stop_sync();
}

void MainWindow::on_size(int w, int h) {
    constexpr int ROOM_W   = 200;
    constexpr int SEND_W   = 100;
    constexpr int INPUT_H  = 32;
    constexpr int STATUS_H = 22;

    int content_h = h - STATUS_H;
    int msg_h     = content_h - INPUT_H;

    SetWindowPos(hRoomList_, nullptr, 0, 0, ROOM_W, msg_h, SWP_NOZORDER);
    SetWindowPos(hMsgView_,  nullptr, ROOM_W, 0, w - ROOM_W, msg_h, SWP_NOZORDER);
    SetWindowPos(hInput_,    nullptr, ROOM_W, msg_h, w - ROOM_W - SEND_W, INPUT_H, SWP_NOZORDER);
    SetWindowPos(hSend_,     nullptr, w - SEND_W, msg_h, SEND_W, INPUT_H, SWP_NOZORDER);
    SendMessageW(hStatus_, WM_SIZE, 0, 0);
}

void MainWindow::on_login_clicked() {
    if (auto saved = tesseract::SessionStore::load()) {
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Restoring session…"));
        auto res = client_.restore_session(*saved);
        if (res) {
            event_handler_ = std::make_unique<EventHandler>(hwnd_);
            client_.start_sync(event_handler_.get());
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Connected"));
            // Room list arrives via on_rooms_updated from RoomListService.
            return;
        }
        tesseract::SessionStore::clear();
        std::wstring msg = L"Saved session expired: ";
        msg.append(res.message.begin(), res.message.end());
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(msg.c_str()));
    }

    LoginDialog dlg(hwnd_, hInst_, client_);
    if (!dlg.run()) {
        SendMessageW(hStatus_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"Not logged in"));
        return;
    }

    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(hwnd_);
    client_.start_sync(event_handler_.get());
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Connected"));
}

void MainWindow::on_send_clicked() {
    if (current_room_id_.empty()) return;

    wchar_t buf[2048] = {};
    GetWindowTextW(hInput_, buf, static_cast<int>(std::size(buf)));

    std::string body(buf, buf + wcslen(buf));
    if (body.empty()) return;

    auto res = client_.send_message(current_room_id_, body);
    if (res) {
        SetWindowTextW(hInput_, L"");
    } else {
        std::wstring err(res.message.begin(), res.message.end());
        MessageBoxW(hwnd_, err.c_str(), L"Send failed", MB_ICONWARNING);
    }
}

void MainWindow::on_room_selected(int index) {
    if (index < 0 || index >= static_cast<int>(rooms_.size())) return;

    const std::string new_id = rooms_[index].id;

    if (!current_room_id_.empty() && current_room_id_ != new_id)
        client_.unsubscribe_room(current_room_id_);

    current_room_id_ = new_id;

    // subscribe_room will post WM_TESSERACT_TIMELINE_RESET (clears the view)
    // followed by WM_TESSERACT_MESSAGE for each cached item.
    auto res = client_.subscribe_room(current_room_id_);
    if (res)
        client_.paginate_back(current_room_id_, 50);
}

void MainWindow::on_reconnect() {
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Sync error: reconnecting…"));
    client_.stop_sync();
    on_login_clicked();
}

void MainWindow::on_auth_error(bool soft_logout) {
    if (soft_logout) {
        if (auto saved = tesseract::SessionStore::load()) {
            SendMessageW(hStatus_, SB_SETTEXTW, 0,
                         reinterpret_cast<LPARAM>(L"Reconnecting session…"));
            if (client_.restore_session(*saved)) {
                event_handler_ = std::make_unique<EventHandler>(hwnd_);
                client_.start_sync(event_handler_.get());
                SendMessageW(hStatus_, SB_SETTEXTW, 0,
                             reinterpret_cast<LPARAM>(L"Reconnected"));
                return;
            }
        }
    }
    tesseract::SessionStore::clear();
    client_.stop_sync();
    SendMessageW(hStatus_, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Session expired; please log in again."));
    on_login_clicked();
}

void MainWindow::on_tesseract_message(tesseract::Event* ev) {
    if (ev->room_id == current_room_id_)
        append_message(*ev);
    // Room list unread counts update via on_tesseract_rooms from RoomListService.
}

void MainWindow::on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms) {
    rooms_ = *rooms;

    SendMessageW(hRoomList_, LB_RESETCONTENT, 0, 0);
    for (const auto& r : rooms_) {
        std::wstring name(r.name.begin(), r.name.end());
        if (r.unread_count > 0)
            name += L" (" + std::to_wstring(r.unread_count) + L")";
        SendMessageW(hRoomList_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }
}

void MainWindow::on_tesseract_timeline_reset(std::string* room_id) {
    if (*room_id == current_room_id_)
        SetWindowTextW(hMsgView_, L"");
}

void MainWindow::append_message(const tesseract::Event& ev) {
    // Win32 EDIT control: text-only (no inline images; see roadmap decision).
    const std::string& name = ev.sender_name.empty() ? ev.sender : ev.sender_name;
    std::string line = name + ": " + ev.body + "\r\n";
    // Use MultiByteToWideChar so display names with non-ASCII chars render correctly.
    int sz = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    std::wstring wline(sz > 0 ? sz - 1 : 0, L'\0');
    if (sz > 0) MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &wline[0], sz);

    int len = GetWindowTextLengthW(hMsgView_);
    SendMessage(hMsgView_, EM_SETSEL, len, len);
    SendMessage(hMsgView_, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(wline.c_str()));
}

} // namespace win32
