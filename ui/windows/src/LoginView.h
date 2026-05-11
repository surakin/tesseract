#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <tesseract/client.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace win32 {

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Drives the same two-phase OAuth / MAS flow as the previous
/// modal LoginDialog (form → worker → browser → worker → done), but is a
/// plain child HWND the main window can show/hide instead of running modally.
class LoginView {
public:
    LoginView(HINSTANCE hInst, HWND hParent, tesseract::Client& client);
    ~LoginView();

    LoginView(const LoginView&)            = delete;
    LoginView& operator=(const LoginView&) = delete;

    HWND hwnd() const { return hwnd_; }

    /// Lay the inner controls out within the given client rect.
    void layout(int w, int h);

    /// Return the view to its initial "form" state. Cancels any in-flight
    /// OAuth and clears errors.
    void reset();

    /// Display a status message above the form (e.g. "Saved session expired").
    void set_status_message(const std::wstring& msg);

    /// Called on the UI thread when the OAuth flow completes successfully.
    void set_on_success(std::function<void()> cb) { on_success_ = std::move(cb); }

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    static bool register_class(HINSTANCE);

    void on_create();
    void on_command(WPARAM wParam);
    void on_begin_completed(bool ok, std::wstring text);
    void on_await_completed(bool ok, std::wstring text);

    void show_form();
    void show_waiting();
    void set_error(const std::wstring& msg);
    void start_phase1();
    void start_phase2();
    void join_worker();

    HINSTANCE hInst_   = nullptr;
    HWND      hParent_ = nullptr;
    HWND      hwnd_    = nullptr;

    HWND      hCardTitle_ = nullptr;
    HWND      hStatusMsg_ = nullptr;
    HWND      hHsLabel_   = nullptr;
    HWND      hHsEdit_    = nullptr;
    HWND      hError_     = nullptr;
    HWND      hWaitLbl_   = nullptr;
    HWND      hSignIn_    = nullptr;
    HWND      hCancel_    = nullptr;

    tesseract::Client&    client_;
    std::function<void()> on_success_;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
    bool              showing_form_ = true;

    static constexpr const wchar_t* CLASS_NAME = L"TesseractLoginView";
    static constexpr int IDC_HS      = 201;
    static constexpr int IDC_SIGNIN  = 202;
    static constexpr int IDC_CANCEL  = 204;
};

// Custom WM_APP messages routed to the LoginView's HWND. Kept distinct from
// the MainWindow's WM_APP range so a stale message can't leak across.
constexpr UINT WM_LOGIN_BEGIN_DONE = WM_APP + 100;
constexpr UINT WM_LOGIN_AWAIT_DONE = WM_APP + 101;

} // namespace win32
