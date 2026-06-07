#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>

#include "tk/host.h"
#include "tk/host_win32.h"
#include "views/LoginView.h"

namespace win32
{

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Visuals come from the shared `tesseract::views::LoginView`
/// rendered through `tk::win32::Surface`; controller logic lives in the
/// shared view. This shell wires platform-specific hooks and exposes a
/// thin public API for the main window.
class LoginView
{
public:
    LoginView(HINSTANCE hInst, HWND hParent);
    ~LoginView();

    LoginView(const LoginView&) = delete;
    LoginView& operator=(const LoginView&) = delete;

    HWND hwnd() const;

    void set_client(tesseract::Client* c);
    void set_mode(tesseract::views::LoginView::Mode m);
    void layout(int w, int h);
    void set_theme(const tk::Theme& t);
    void reset();
    void show_restore_error(const std::string& body,
                            std::function<void()> retry_cb);

    /// UTF-16 overload kept for Win32 callers passing TCHAR strings.
    void set_status_message(const std::wstring& msg);

    void set_on_begin_oauth(std::function<void()> cb);
    void set_on_success(std::function<void()> cb);
    void set_on_cancel(std::function<void()> cb);
    void set_run_async(std::function<void(std::function<void()>)> fn);

private:
    static std::string wstring_to_utf8(const std::wstring& s);

    std::unique_ptr<tk::win32::Surface> surface_;
    tesseract::views::LoginView*        shared_ = nullptr; // borrowed from surface
};

} // namespace win32
