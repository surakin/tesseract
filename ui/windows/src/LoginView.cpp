#include "LoginView.h"

#include <windowsx.h>

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

std::string narrow_hwnd(HWND hEdit) {
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

std::string narrow_wstr(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

HFONT default_ui_font() {
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

HFONT title_font() {
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight  = lf.lfHeight - 4;  // slightly larger
    lf.lfWeight  = FW_BOLD;
    return CreateFontIndirectW(&lf);
}

} // namespace

// ---------------------------------------------------------------------------

bool LoginView::register_class(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LoginView::wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;
    registered = true;
    return true;
}

LoginView::LoginView(HINSTANCE hInst, HWND hParent, tesseract::Client& client)
    : hInst_(hInst), hParent_(hParent), client_(client)
{
    if (!register_class(hInst_)) return;

    hwnd_ = CreateWindowExW(
        0, CLASS_NAME, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        hParent_, nullptr, hInst_, this);
}

LoginView::~LoginView() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK LoginView::wnd_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LoginView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<LoginView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<LoginView*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE:
        self->on_create();
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

void LoginView::on_create() {
    HFONT font  = default_ui_font();
    HFONT tfont = title_font();
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                   int id, HFONT f) -> HWND
    {
        HWND ctl = CreateWindowExW(0, cls, text,
                                  WS_CHILD | WS_VISIBLE | style,
                                  0, 0, 0, 0,
                                  hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  hInst_, nullptr);
        SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return ctl;
    };

    hCardTitle_ = add(L"STATIC", L"Sign in to Tesseract", 0,           0, tfont);
    hStatusMsg_ = add(L"STATIC", L"", SS_LEFT,                         0, font);
    ShowWindow(hStatusMsg_, SW_HIDE);

    hHsLabel_   = add(L"STATIC", L"Homeserver:", 0,                    0, font);
    hHsEdit_    = add(L"EDIT", L"matrix.org",
                      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,         IDC_HS, font);

    hError_     = add(L"STATIC", L"", SS_LEFT,                         0, font);
    hWaitLbl_   = add(L"STATIC", L"Waiting for sign-in in your browser…",
                      SS_LEFT,                                         0, font);
    ShowWindow(hWaitLbl_, SW_HIDE);

    hSignIn_    = add(L"BUTTON", L"Sign in",
                      BS_DEFPUSHBUTTON | WS_TABSTOP,                   IDC_SIGNIN, font);
    hCancel_    = add(L"BUTTON", L"Cancel",
                      BS_PUSHBUTTON | WS_TABSTOP,                      IDC_CANCEL, font);
    ShowWindow(hCancel_, SW_HIDE);

    show_form();
}

void LoginView::layout(int w, int h) {
    constexpr int CARD_W   = 420;
    constexpr int CARD_H   = 260;
    constexpr int PAD      = 24;
    const int card_x = (w - CARD_W) / 2;
    const int card_y = (h - CARD_H) / 2;

    // Title
    SetWindowPos(hCardTitle_, nullptr,
                 card_x + PAD, card_y + PAD,
                 CARD_W - 2 * PAD, 24, SWP_NOZORDER);

    int y = card_y + PAD + 28;

    // Status message (e.g. saved session expired)
    if (IsWindowVisible(hStatusMsg_)) {
        SetWindowPos(hStatusMsg_, nullptr,
                     card_x + PAD, y,
                     CARD_W - 2 * PAD, 36, SWP_NOZORDER);
        y += 40;
    }

    // Form vs waiting
    if (showing_form_) {
        SetWindowPos(hHsLabel_, nullptr,
                     card_x + PAD, y + 4,
                     90, 20, SWP_NOZORDER);
        SetWindowPos(hHsEdit_, nullptr,
                     card_x + PAD + 96, y,
                     CARD_W - 2 * PAD - 96, 24, SWP_NOZORDER);
        y += 36;

        SetWindowPos(hError_, nullptr,
                     card_x + PAD, y,
                     CARD_W - 2 * PAD, 36, SWP_NOZORDER);
        y += 44;

        SetWindowPos(hSignIn_, nullptr,
                     card_x + CARD_W - PAD - 100, y,
                     100, 28, SWP_NOZORDER);
    } else {
        SetWindowPos(hWaitLbl_, nullptr,
                     card_x + PAD, y,
                     CARD_W - 2 * PAD, 40, SWP_NOZORDER);
        y += 56;
        SetWindowPos(hCancel_, nullptr,
                     card_x + CARD_W - PAD - 100, y,
                     100, 28, SWP_NOZORDER);
    }
}

void LoginView::on_command(WPARAM wParam) {
    if (HIWORD(wParam) != BN_CLICKED) return;
    switch (LOWORD(wParam)) {
    case IDC_SIGNIN: start_phase1(); break;
    case IDC_CANCEL:
        cancelled_.store(true);
        client_.cancel_oauth();
        SetWindowTextW(hWaitLbl_, L"Cancelling…");
        EnableWindow(hCancel_, FALSE);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------

void LoginView::show_form() {
    showing_form_ = true;
    ShowWindow(hHsLabel_, SW_SHOW); EnableWindow(hHsEdit_, TRUE);
    ShowWindow(hHsEdit_,  SW_SHOW);
    ShowWindow(hError_,   SW_SHOW);
    ShowWindow(hWaitLbl_, SW_HIDE);
    ShowWindow(hSignIn_,  SW_SHOW); EnableWindow(hSignIn_, TRUE);
    ShowWindow(hCancel_,  SW_HIDE);
    SetFocus(hHsEdit_);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    layout(rc.right, rc.bottom);
}

void LoginView::show_waiting() {
    showing_form_ = false;
    ShowWindow(hHsLabel_, SW_HIDE);
    ShowWindow(hHsEdit_,  SW_HIDE);
    ShowWindow(hError_,   SW_HIDE);
    ShowWindow(hWaitLbl_, SW_SHOW);
    ShowWindow(hSignIn_,  SW_HIDE);
    ShowWindow(hCancel_,  SW_SHOW); EnableWindow(hCancel_, TRUE);
    SetWindowTextW(hWaitLbl_, L"Waiting for sign-in in your browser…");

    RECT rc;
    GetClientRect(hwnd_, &rc);
    layout(rc.right, rc.bottom);
}

void LoginView::set_error(const std::wstring& msg) {
    SetWindowTextW(hError_, msg.c_str());
}

void LoginView::set_status_message(const std::wstring& msg) {
    if (msg.empty()) {
        ShowWindow(hStatusMsg_, SW_HIDE);
    } else {
        SetWindowTextW(hStatusMsg_, msg.c_str());
        ShowWindow(hStatusMsg_, SW_SHOW);
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    layout(rc.right, rc.bottom);
}

void LoginView::reset() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    cancelled_.store(false);
    set_error(L"");
    show_form();
}

// ---------------------------------------------------------------------------

void LoginView::start_phase1() {
    std::string hs = narrow_hwnd(hHsEdit_);
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

void LoginView::on_begin_completed(bool ok, std::wstring text) {
    join_worker();

    if (!ok) {
        std::wstring msg = L"Sign-in failed: ";
        msg += text;
        set_error(msg);
        EnableWindow(hSignIn_, TRUE);
        EnableWindow(hHsEdit_, TRUE);
        return;
    }

    tesseract::Client::open_in_browser(narrow_wstr(text));
    show_waiting();
    start_phase2();
}

void LoginView::start_phase2() {
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

void LoginView::on_await_completed(bool ok, std::wstring text) {
    join_worker();

    if (ok) {
        if (on_success_) on_success_();
        return;
    }
    std::wstring msg = L"Sign-in failed: ";
    msg += text;
    set_error(msg);
    show_form();
}

void LoginView::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace win32
