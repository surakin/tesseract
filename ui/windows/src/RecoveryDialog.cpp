#include "RecoveryDialog.h"
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

std::string narrow_edit(HWND hEdit) {
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

bool RecoveryDialog::register_class(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = RecoveryDialog::wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;
    registered = true;
    return true;
}

RecoveryDialog::RecoveryDialog(HWND hParent, HINSTANCE hInst, tesseract::Client& client)
    : hParent_(hParent), hInst_(hInst), client_(client)
{}

RecoveryDialog::~RecoveryDialog() {
    cancelled_.store(true);
    join_worker();
}

bool RecoveryDialog::run() {
    if (!register_class(hInst_)) return false;

    constexpr int W = 460, H = 240;
    RECT pr{};
    if (hParent_) GetWindowRect(hParent_, &pr);
    int x = pr.left + (pr.right  - pr.left - W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME, L"Verify this device",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        hParent_, nullptr, hInst_, this);

    if (!hwnd_) return false;

    if (hParent_) EnableWindow(hParent_, FALSE);
    SetFocus(hKeyEdit_);

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

LRESULT CALLBACK RecoveryDialog::wnd_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RecoveryDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<RecoveryDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<RecoveryDialog*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE:
        self->on_create();
        return 0;

    case WM_CLOSE:
        self->cancelled_.store(true);
        self->accepted_ = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        return 0;

    case WM_COMMAND:
        self->on_command(wParam);
        return 0;

    case WM_RECOVERY_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lParam);
        self->on_recover_completed(wParam != 0, std::move(*p));
        delete p;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void RecoveryDialog::on_create() {
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

    hIntro_    = add(L"STATIC",
        L"Enter your recovery key or passphrase to verify this device "
        L"and decrypt historical messages.",
        SS_LEFT,                                       16, 12, 424, 40, 0);
    hKeyLabel_ = add(L"STATIC", L"Recovery key:", 0,  16, 64, 100, 20, 0);
    hKeyEdit_  = add(L"EDIT", L"",
                    WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                                                       120, 62, 320, 24, IDC_KEY);
    hError_    = add(L"STATIC", L"", SS_LEFT,          16, 94, 424, 36, 0);
    hStatus_   = add(L"STATIC", L"Unlocking secret storage…",
                    SS_LEFT,                            16, 94, 424, 60, 0);
    ShowWindow(hStatus_, SW_HIDE);

    hVerify_ = add(L"BUTTON", L"Verify",
                  BS_DEFPUSHBUTTON | WS_TABSTOP,        340, 170, 100, 28, IDC_VERIFY);
    hSkip_   = add(L"BUTTON", L"Skip",
                  BS_PUSHBUTTON | WS_TABSTOP,           230, 170, 100, 28, IDC_SKIP);
    hClose_  = add(L"BUTTON", L"Close",
                  BS_PUSHBUTTON | WS_TABSTOP,           340, 170, 100, 28, IDC_CLOSE);
    EnableWindow(hClose_, FALSE);
    ShowWindow(hClose_, SW_HIDE);

    show_form();
}

void RecoveryDialog::on_command(WPARAM wParam) {
    if (HIWORD(wParam) != BN_CLICKED) return;
    switch (LOWORD(wParam)) {
    case IDC_VERIFY: start_recover(); break;
    case IDC_SKIP:   PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDC_CLOSE:
        accepted_ = true;
        DestroyWindow(hwnd_);
        break;
    default: break;
    }
}

void RecoveryDialog::show_form() {
    ShowWindow(hIntro_,    SW_SHOW);
    ShowWindow(hKeyLabel_, SW_SHOW);
    ShowWindow(hKeyEdit_,  SW_SHOW); EnableWindow(hKeyEdit_, TRUE);
    ShowWindow(hError_,    SW_SHOW);
    ShowWindow(hStatus_,   SW_HIDE);
    ShowWindow(hVerify_,   SW_SHOW); EnableWindow(hVerify_, TRUE);
    ShowWindow(hSkip_,     SW_SHOW);
    ShowWindow(hClose_,    SW_HIDE);
    SetFocus(hKeyEdit_);
}

void RecoveryDialog::show_waiting() {
    ShowWindow(hIntro_,    SW_HIDE);
    ShowWindow(hKeyLabel_, SW_HIDE);
    ShowWindow(hKeyEdit_,  SW_HIDE);
    ShowWindow(hError_,    SW_HIDE);
    ShowWindow(hStatus_,   SW_SHOW);
    ShowWindow(hVerify_,   SW_HIDE);
    ShowWindow(hSkip_,     SW_HIDE);
    ShowWindow(hClose_,    SW_SHOW); EnableWindow(hClose_, FALSE);
}

void RecoveryDialog::set_error(const std::wstring& msg) {
    SetWindowTextW(hError_, msg.c_str());
}

void RecoveryDialog::set_progress(const tesseract::BackupProgress& progress) {
    if (!recover_done_) return;
    switch (progress.state) {
        case tesseract::BackupState::Enabled: {
            std::wstring txt = L"Done. Imported "
                + std::to_wstring(progress.imported_keys) + L" keys.";
            SetWindowTextW(hStatus_, txt.c_str());
            EnableWindow(hClose_, TRUE);
            break;
        }
        case tesseract::BackupState::Downloading: {
            std::wstring txt = L"Importing keys… "
                + std::to_wstring(progress.imported_keys) + L" imported.";
            SetWindowTextW(hStatus_, txt.c_str());
            break;
        }
        case tesseract::BackupState::Disabled: {
            SetWindowTextW(hStatus_, L"Backup is not enabled on the server.");
            EnableWindow(hClose_, TRUE);
            break;
        }
        default: break;
    }
}

void RecoveryDialog::start_recover() {
    std::string key = narrow_edit(hKeyEdit_);
    if (key.empty()) {
        set_error(L"Please enter a recovery key or passphrase.");
        return;
    }
    set_error(L"");
    EnableWindow(hVerify_, FALSE);
    EnableWindow(hKeyEdit_, FALSE);
    show_waiting();

    join_worker();
    cancelled_.store(false);

    HWND target = hwnd_;
    worker_ = std::thread([this, target, key]() {
        auto res = client_.recover(key);
        if (cancelled_.load()) return;
        WPARAM ok = res.ok ? 1 : 0;
        auto*  p  = new std::wstring(widen(res.message));
        PostMessageW(target, WM_RECOVERY_DONE,
                     ok, reinterpret_cast<LPARAM>(p));
    });
}

void RecoveryDialog::on_recover_completed(bool ok, std::wstring text) {
    join_worker();

    if (!ok) {
        std::wstring msg = L"Recovery failed: ";
        msg += text;
        set_error(msg);
        show_form();
        return;
    }
    recover_done_ = true;
    SetWindowTextW(hStatus_, L"Downloading historical keys…");
    // hClose_ stays disabled until backup state reaches Enabled.
}

void RecoveryDialog::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace win32
