#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>

#include <functional>

#include "tk/host_gtk.h"
#include "views/LoginView.h"

namespace gtk4
{

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Visuals come from the shared `tesseract::views::LoginView`
/// rendered through a `tk::gtk4::Surface`; controller logic lives in the
/// shared view. This shell wires platform-specific hooks and exposes a
/// thin public API for the main window.
class LoginView
{
public:
    LoginView();
    ~LoginView();

    LoginView(const LoginView&) = delete;
    LoginView& operator=(const LoginView&) = delete;

    /// Root widget — add to a container / GtkStack.
    GtkWidget* widget() const;

    void set_client(tesseract::Client* c);
    void set_mode(tesseract::views::LoginView::Mode m);
    void set_theme(const tk::Theme& t);
    void set_on_begin_oauth(std::function<void()> cb);
    void set_on_success(std::function<void()> cb);
    void set_on_cancel(std::function<void()> cb);
    void set_run_async(std::function<void(std::function<void()>)> fn);
    void reset();
    void set_status_message(const std::string& msg);
    void show_restore_error(const std::string& body,
                            std::function<void()> retry_cb);

private:
    std::unique_ptr<tk::gtk4::Surface> surface_;
    tesseract::views::LoginView*       shared_ = nullptr; // borrowed from surface
};

} // namespace gtk4
