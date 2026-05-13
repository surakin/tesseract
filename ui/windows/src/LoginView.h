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
#include <memory>
#include <string>
#include <thread>

#include "tk/host.h"
#include "tk/host_win32.h"
#include "views/LoginView.h"

namespace win32 {

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Visuals come from the shared `tesseract::views::LoginView`
/// rendered through `tk::win32::Surface`; the OAuth state machine +
/// worker threads + native EDIT overlay live in this shell. The
/// homeserver field stays native until tk::TextField + IME passthrough
/// lands.
class LoginView {
public:
    LoginView(HINSTANCE hInst, HWND hParent);
    ~LoginView();

    LoginView(const LoginView&)            = delete;
    LoginView& operator=(const LoginView&) = delete;

    HWND hwnd() const;

    /// Rebind before each login attempt.
    void set_client(tesseract::Client* c) { client_ = c; }

    /// Initial = Cancel hidden; AddAccount = Cancel visible in Form + Waiting.
    void set_mode(tesseract::views::LoginView::Mode m);

    /// Lay the surface out at (0, 0, w, h) inside the parent client area.
    void layout(int w, int h);

    /// Return the view to its initial "form" state.
    void reset();

    /// Display a message above the form (e.g. "Saved session expired").
    void set_status_message(const std::wstring& msg);

    /// Called on the UI thread when the OAuth flow completes successfully.
    void set_on_success(std::function<void()> cb) { on_success_ = std::move(cb); }

    /// Called on the UI thread when the user cancels (AddAccount mode only).
    void set_on_cancel(std::function<void()> cb) { on_cancel_fn_ = std::move(cb); }

private:
    void on_sign_in();
    void on_cancel();
    void on_begin_completed(bool ok, std::string err_or_url);
    void on_await_completed(bool ok, std::string err);
    void position_overlay();
    void join_worker();

    static std::string  trim         (std::string s);
    static std::string  wstring_to_utf8(const std::wstring& s);

    tesseract::Client*                     client_ = nullptr;
    std::function<void()>                  on_success_;
    std::function<void()>                  on_cancel_fn_;

    std::unique_ptr<tk::win32::Surface>    surface_;
    tesseract::views::LoginView*           shared_   = nullptr;
    std::unique_ptr<tk::NativeTextField>   hs_field_;

    std::thread       worker_;
    std::atomic<bool> cancelled_{ false };
};

} // namespace win32
