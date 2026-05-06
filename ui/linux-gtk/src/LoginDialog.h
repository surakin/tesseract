#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace gtk4 {

/// Modal sign-in dialog driving the OAuth / Matrix Authentication Service
/// flow. Two pages (form, waiting) toggle inside a GtkStack. The OAuth work
/// runs on a std::thread and reports back to the GTK main loop via
/// g_idle_add so the UI thread stays responsive while the user is in their
/// browser.
///
/// Usage:
///   LoginDialog dlg(parent_window, client);
///   if (dlg.run()) {
///       client.start_sync(...);
///   }
class LoginDialog {
public:
    LoginDialog(GtkWindow* parent, tesseract::Client& client);
    ~LoginDialog();

    LoginDialog(const LoginDialog&)            = delete;
    LoginDialog& operator=(const LoginDialog&) = delete;

    /// Show modally and pump the GTK main loop until the dialog closes.
    /// Returns true on a successful sign-in.
    bool run();

private:
    static void on_signin_clicked(GtkButton*, gpointer);
    static void on_close_clicked (GtkButton*, gpointer);
    static void on_cancel_clicked(GtkButton*, gpointer);
    static void on_window_close_request(GtkWindow*, gpointer);

    static gboolean on_begin_done(gpointer);
    static gboolean on_await_done(gpointer);

    void show_form();
    void show_waiting();
    void set_error(const std::string& msg);
    void start_phase1();
    void start_phase2();
    void join_worker();
    void finish(bool accepted);

    GtkWindow*     parent_       = nullptr;
    GtkWidget*     window_       = nullptr;
    GtkWidget*     stack_        = nullptr;
    GtkWidget*     hs_entry_     = nullptr;
    GtkWidget*     error_lbl_    = nullptr;
    GtkWidget*     status_lbl_   = nullptr;
    GtkWidget*     signin_btn_   = nullptr;
    GtkWidget*     close_btn_    = nullptr;
    GtkWidget*     cancel_btn_   = nullptr;

    GMainLoop*     loop_         = nullptr;

    tesseract::Client& client_;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
    bool              accepted_  = false;
};

// Heap payload for cross-thread messages (deleted by the idle callback).
struct LoginIdle {
    LoginDialog* dlg;
    bool         ok;
    std::string  text;       // auth_url on phase1 success, error otherwise
};

} // namespace gtk4
