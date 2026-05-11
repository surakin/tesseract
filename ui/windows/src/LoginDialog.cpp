#include "LoginDialog.h"
#include <windowsx.h>

#include <string>

namespace win32 {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string narrow(HWND hEdit) {
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

HFONT default_ui_font() {
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

} // namespace

// ---------------------------------------------------------------------------

bool LoginDialog::register_class(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LoginDialog::wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;
    registered = true;
    return true;
}

LoginDialog::LoginDialog(HWND hParent, HINSTANCE hInst, tesseract::Client& client)
    : hParent_(hParent), hInst_(hInst), client_(client)
{}

LoginDialog::~LoginDialog() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
}

bool LoginDialog::run() {
    if (!register_class(hInst_)) return false;

    constexpr int W = 460, H = 220;
    RECT pr{};
    if (hParent_) GetWindowRect(hParent_, &pr);
    int x = pr.left + (pr.right  - pr.left - W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME, L"Sign in to Tesseract",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        hParent_, nullptr, hInst_, this);

    if (!hwnd_) return false;

    // Modal: disable parent and pump messages until the dialog closes.
    if (hParent_) EnableWindow(hParent_, FALSE);
    SetFocus(hHsEdit_);

    MSG msg{};
    while (IsWindow(hwnd_) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hParent_) {
        EnableWindow(hParent_, TRUE);
        SetForegroundWindow(hParent_);
    }
    return accepted_;
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK LoginDialog::wnd_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LoginDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<LoginDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<LoginDialog*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE:
        self->on_create();
        return 0;

    case WM_CLOSE:
        self->cancelled_.store(true);
        self->client_.cancel_oauth();
        self->accepted_ = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        return 0;

    case WM_SIZE:
        self->on_size(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        self->on_command(wParam);
        return 0;

    case WM_LOGIN_BEGIN_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_begin_completed(wParam != 0, std::move(*p));
        delete p;
        return 0;
    }
    case WM_LOGIN_AWAIT_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_await_completed(wParam != 0, std::move(*p));
        delete p;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------

void LoginDialog::on_create() {
    HFONT font = default_ui_font();
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                   int x, int y, int w, int h, int id) -> HWND
    {
        HWND ctl = CreateWindowExW(0, cls, text,
                                  WS_CHILD | WS_VISIBLE | style,
                                  x, y, w, h,
                                  hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  hInst_, nullptr);
        SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return ctl;
    };

    hHsLabel_ = add(L"STATIC", L"Homeserver:", 0,                 16, 18, 100, 20, 0);
    hHsEdit_  = add(L"EDIT", L"matrix.org",
                    WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,     120, 16, 320, 24, IDC_HS);

    hError_   = add(L"STATIC", L"", SS_LEFT,                      16, 50, 424, 36, 0);

    hStatus_  = add(L"STATIC",
                    L"Waiting for sign-in in your browser…",
                    SS_LEFT,                                       16, 22, 424, 40, 0);
    ShowWindow(hStatus_, SW_HIDE);

    hSignIn_  = add(L"BUTTON", L"Sign in",
                    BS_DEFPUSHBUTTON | WS_TABSTOP,                 340, 150, 100, 28, IDC_SIGNIN);
    hClose_   = add(L"BUTTON", L"Close",
                    BS_PUSHBUTTON | WS_TABSTOP,                    230, 150, 100, 28, IDC_CLOSE);
    hCancel_  = add(L"BUTTON", L"Cancel",
                    BS_PUSHBUTTON | WS_TABSTOP,                    340, 150, 100, 28, IDC_CANCEL);
    ShowWindow(hCancel_, SW_HIDE);

    show_form();
}

void LoginDialog::on_size(int /*w*/, int /*h*/) {
    // Fixed-size dialog; nothing to relayout.
}

void LoginDialog::on_command(WPARAM wParam) {
    if (HIWORD(wParam) != BN_CLICKED) return;
    switch (LOWORD(wParam)) {
    case IDC_SIGNIN: start_phase1(); break;
    case IDC_CLOSE:  PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDC_CANCEL:
        cancelled_.store(true);
        client_.cancel_oauth();
        SetWindowTextW(hStatus_, L"Cancelling…");
        EnableWindow(hCancel_, FALSE);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------

void LoginDialog::show_form() {
    showing_form_ = true;
    ShowWindow(hHsLabel_, SW_SHOW); EnableWindow(hHsEdit_, TRUE);
    ShowWindow(hHsEdit_,  SW_SHOW);
    ShowWindow(hError_,   SW_SHOW);
    ShowWindow(hStatus_,  SW_HIDE);
    ShowWindow(hSignIn_,  SW_SHOW); EnableWindow(hSignIn_, TRUE);
    ShowWindow(hClose_,   SW_SHOW);
    ShowWindow(hCancel_,  SW_HIDE);
    SetFocus(hHsEdit_);
}

void LoginDialog::show_waiting() {
    showing_form_ = false;
    ShowWindow(hHsLabel_, SW_HIDE);
    ShowWindow(hHsEdit_,  SW_HIDE);
    ShowWindow(hError_,   SW_HIDE);
    ShowWindow(hStatus_,  SW_SHOW);
    ShowWindow(hSignIn_,  SW_HIDE);
    ShowWindow(hClose_,   SW_HIDE);
    ShowWindow(hCancel_,  SW_SHOW); EnableWindow(hCancel_, TRUE);
    SetWindowTextW(hStatus_, L"Waiting for sign-in in your browser…");
}

void LoginDialog::set_error(const std::wstring& msg) {
    SetWindowTextW(hError_, msg.c_str());
}

// ---------------------------------------------------------------------------

void LoginDialog::start_phase1() {
    std::string hs = narrow(hHsEdit_);
    if (hs.empty()) {
        set_error(L"Please enter a homeserver.");
        return;
    }
    set_error(L"");
    EnableWindow(hSignIn_, FALSE);
    EnableWindow(hHsEdit_, FALSE);

    join_worker();
    cancelled_.store(false);

    HWND target = hwnd_;
    worker_ = std::thread([this, target, hs]() {
        auto flow = client_.begin_oauth(hs);
        if (cancelled_.load()) return;

        WPARAM ok = flow.ok ? 1 : 0;
        auto*  p  = new std::wstring(
            flow.ok ? widen(flow.auth_url) : widen(flow.message));
        PostMessageW(target, WM_LOGIN_BEGIN_DONE,
                     ok, reinterpret_cast<LPARAM>(p));
    });
}

void LoginDialog::on_begin_completed(bool ok, std::wstring text) {
    join_worker();

    if (!ok) {
        std::wstring msg = L"Sign-in failed: ";
        msg += text;
        set_error(msg);
        EnableWindow(hSignIn_, TRUE);
        EnableWindow(hHsEdit_, TRUE);
        return;
    }

    // text == auth_url
    tesseract::Client::open_in_browser(narrow(text));
    show_waiting();
    start_phase2();
}

void LoginDialog::start_phase2() {
    join_worker();
    cancelled_.store(false);

    HWND target = hwnd_;
    worker_ = std::thread([this, target]() {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        WPARAM ok = res.ok ? 1 : 0;
        auto*  p  = new std::wstring(widen(res.message));
        PostMessageW(target, WM_LOGIN_AWAIT_DONE,
                     ok, reinterpret_cast<LPARAM>(p));
    });
}

void LoginDialog::on_await_completed(bool ok, std::wstring text) {
    join_worker();

    if (ok) {
        accepted_ = true;
        DestroyWindow(hwnd_);
        return;
    }
    std::wstring msg = L"Sign-in failed: ";
    msg += text;
    set_error(msg);
    show_form();
}

void LoginDialog::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace win32
