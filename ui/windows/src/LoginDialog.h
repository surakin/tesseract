#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <tesseract/client.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace win32 {

/// Modal sign-in dialog driving the OAuth / Matrix Authentication Service
/// flow. Two states are toggled in place (form vs. waiting); the OAuth work
/// runs on a std::thread and posts WM_APP messages back to the dialog HWND.
///
/// Usage:
///   LoginDialog dlg(hParent, hInst, client);
///   if (dlg.run()) {
///       client.start_sync(...);
///   }
class LoginDialog {
public:
    LoginDialog(HWND hParent, HINSTANCE hInst, tesseract::Client& client);
    ~LoginDialog();

    LoginDialog(const LoginDialog&)            = delete;
    LoginDialog& operator=(const LoginDialog&) = delete;

    /// Show modally; returns true on a successful sign-in.
    bool run();

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    static bool register_class(HINSTANCE);

    void on_create();
    void on_size(int w, int h);
    void on_command(WPARAM wParam);
    void on_begin_completed(bool ok, std::wstring text);
    void on_await_completed(bool ok, std::wstring text);

    void show_form();
    void show_waiting();
    void set_error(const std::wstring& msg);
    void start_phase1();
    void start_phase2();
    void join_worker();

    HWND      hParent_ = nullptr;
    HINSTANCE hInst_   = nullptr;
    HWND      hwnd_    = nullptr;

    HWND      hHsLabel_  = nullptr;
    HWND      hHsEdit_   = nullptr;
    HWND      hError_    = nullptr;
    HWND      hStatus_   = nullptr;
    HWND      hSignIn_   = nullptr;
    HWND      hClose_    = nullptr;
    HWND      hCancel_   = nullptr;

    tesseract::Client& client_;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
    bool              accepted_  = false;
    bool              showing_form_ = true;

    static constexpr const wchar_t* CLASS_NAME = L"TesseractLoginDlg";
    static constexpr int IDC_HS      = 201;
    static constexpr int IDC_SIGNIN  = 202;
    static constexpr int IDC_CLOSE   = 203;
    static constexpr int IDC_CANCEL  = 204;
};

// Custom WM_APP messages (kept distinct from MainWindow's range so a stale
// message can't leak across).
constexpr UINT WM_LOGIN_BEGIN_DONE = WM_APP + 100;
constexpr UINT WM_LOGIN_AWAIT_DONE = WM_APP + 101;

} // namespace win32
