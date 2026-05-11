#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace gtk4 {

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Drives the same two-phase OAuth / MAS flow as the previous
/// modal LoginDialog (form → worker → browser → worker → done), but is a
/// plain GtkWidget the main window can swap into a GtkStack instead of
/// running modally.
class LoginView {
public:
    explicit LoginView(tesseract::Client& client);
    ~LoginView();

    LoginView(const LoginView&)            = delete;
    LoginView& operator=(const LoginView&) = delete;

    /// Root widget — add to a container / GtkStack.
    GtkWidget* widget() const { return root_; }

    /// Called on the main thread when the OAuth flow completes successfully.
    void set_on_success(std::function<void()> cb) { on_success_ = std::move(cb); }

    /// Return the view to its initial "form" state (cancel any in-flight
    /// OAuth and clear errors). Call before showing the view again.
    void reset();

    /// Display a message above the form (e.g. "Saved session expired").
    void set_status_message(const std::string& msg);

private:
    static void on_signin_clicked(GtkButton*, gpointer);
    static void on_cancel_clicked(GtkButton*, gpointer);

    static gboolean on_begin_done(gpointer);
    static gboolean on_await_done(gpointer);

    void show_form();
    void show_waiting();
    void set_error(const std::string& msg);
    void start_phase1();
    void start_phase2();
    void join_worker();

    tesseract::Client& client_;
    std::function<void()> on_success_;

    GtkWidget* root_        = nullptr;
    GtkWidget* stack_       = nullptr;
    GtkWidget* hs_entry_    = nullptr;
    GtkWidget* status_lbl_  = nullptr;
    GtkWidget* error_lbl_   = nullptr;
    GtkWidget* wait_lbl_    = nullptr;
    GtkWidget* signin_btn_  = nullptr;
    GtkWidget* cancel_btn_  = nullptr;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
};

// Heap payload for cross-thread messages (deleted by the idle callback).
struct LoginViewIdle {
    LoginView*  view;
    bool        ok;
    std::string text;
};

} // namespace gtk4
