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
    LoginView();
    ~LoginView();

    LoginView(const LoginView&)            = delete;
    LoginView& operator=(const LoginView&) = delete;

    /// Root widget — add to a container / GtkStack.
    GtkWidget* widget() const;

    /// Rebind the target Client before each login attempt (set before showing).
    void set_client(tesseract::Client* c) { client_ = c; }

    /// Toggle between initial-login (no Cancel button) and add-account
    /// (Cancel visible in both Form and Waiting states) presentation.
    void set_mode(tesseract::views::LoginView::Mode m);

    /// Called on the main thread when the OAuth flow completes successfully.
    void set_on_success(std::function<void()> cb) { on_success_ = std::move(cb); }

    /// Called on the main thread when the user clicks Cancel in AddAccount mode.
    void set_on_cancel(std::function<void()> cb) { on_cancel_fn_ = std::move(cb); }

    /// Return the view to its initial "form" state.
    void reset();

    /// Show a message above the form (e.g. "Saved session expired").
    void set_status_message(const std::string& msg);

private:
    void on_sign_in();
    void on_cancel();
    void on_begin_completed(bool ok, std::string err_or_url);
    void on_await_completed(bool ok, std::string err);
    void on_hs_text_changed(const std::string& text);
    void position_overlay();
    void join_worker();

    static std::string trim(std::string s);

    tesseract::Client*                     client_ = nullptr;  // non-owning
    std::function<void()>                  on_success_;
    std::function<void()>                  on_cancel_fn_;

    std::unique_ptr<tk::gtk4::Surface>     surface_;
    tesseract::views::LoginView*           shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>   hs_field_;

    std::thread                 worker_;
    std::atomic<bool>           cancelled_{ false };
    std::atomic<uint32_t>       discovery_gen_{ 0 };
};

} // namespace gtk4
