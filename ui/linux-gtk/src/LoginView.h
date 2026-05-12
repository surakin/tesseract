#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/LoginView.h"

namespace gtk4 {

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Visuals come from the shared `tesseract::views::LoginView`
/// rendered through a `tk::gtk4::Surface`; the OAuth state machine +
/// worker threads + native GtkEntry overlay live in this shell. The
/// homeserver field stays native until tk::TextField + IME passthrough
/// lands.
class LoginView {
public:
    explicit LoginView(tesseract::Client& client);
    ~LoginView();

    LoginView(const LoginView&)            = delete;
    LoginView& operator=(const LoginView&) = delete;

    /// Root widget — add to a container / GtkStack.
    GtkWidget* widget() const;

    /// Called on the main thread when the OAuth flow completes successfully.
    void set_on_success(std::function<void()> cb) { on_success_ = std::move(cb); }

    /// Return the view to its initial "form" state.
    void reset();

    /// Show a message above the form (e.g. "Saved session expired").
    void set_status_message(const std::string& msg);

private:
    void on_sign_in();
    void on_cancel();
    void on_begin_completed(bool ok, std::string err_or_url);
    void on_await_completed(bool ok, std::string err);
    void position_overlay();
    void join_worker();

    static std::string trim(std::string s);

    tesseract::Client&                     client_;
    std::function<void()>                  on_success_;

    std::unique_ptr<tk::gtk4::Surface>     surface_;
    tesseract::views::LoginView*           shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>   hs_field_;

    std::thread       worker_;
    std::atomic<bool> cancelled_{ false };
};

} // namespace gtk4
