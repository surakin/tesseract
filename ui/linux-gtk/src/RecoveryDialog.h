#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <atomic>
#include <string>
#include <thread>

namespace gtk4 {

/// Modal dialog driving the Matrix key-recovery flow (Step 6).
///
/// Two pages (form, waiting) toggle inside a GtkStack. recover() runs on a
/// std::thread and reports back via g_idle_add. The waiting page closes
/// itself once on_backup_progress reaches BackupState::Enabled.
///
/// Usage:
///   RecoveryDialog dlg(parent_window, client);
///   dlg.show();              // non-blocking — caller stays in main loop
///   dlg.set_progress(p);     // forward from MainWindow
class RecoveryDialog {
public:
    RecoveryDialog(GtkWindow* parent, tesseract::Client& client);
    ~RecoveryDialog();

    RecoveryDialog(const RecoveryDialog&)            = delete;
    RecoveryDialog& operator=(const RecoveryDialog&) = delete;

    /// Show modally and pump the GTK main loop until the dialog closes.
    bool run();

    /// Forward a backup-progress update from the MainWindow event handler.
    void set_progress(const tesseract::BackupProgress& progress);

private:
    static void on_verify_clicked(GtkButton*, gpointer);
    static void on_skip_clicked  (GtkButton*, gpointer);
    static void on_close_clicked (GtkButton*, gpointer);
    static void on_window_close_request(GtkWindow*, gpointer);

    static gboolean on_recover_done(gpointer);

    void show_form();
    void show_waiting();
    void set_error(const std::string& msg);
    void start_recover();
    void join_worker();
    void finish(bool accepted);

    GtkWindow*     parent_       = nullptr;
    GtkWidget*     window_       = nullptr;
    GtkWidget*     stack_        = nullptr;
    GtkWidget*     key_entry_    = nullptr;
    GtkWidget*     error_lbl_    = nullptr;
    GtkWidget*     progress_lbl_ = nullptr;
    GtkWidget*     verify_btn_   = nullptr;
    GtkWidget*     skip_btn_     = nullptr;
    GtkWidget*     close_btn_    = nullptr;

    GMainLoop*     loop_         = nullptr;

    tesseract::Client& client_;

    std::thread       worker_;
    std::atomic<bool> cancelled_     { false };
    bool              accepted_      = false;
    bool              recover_done_  = false;
};

// Heap payload for the recover-done idle callback.
struct RecoveryIdle {
    RecoveryDialog* dlg;
    bool            ok;
    std::string     message;
};

} // namespace gtk4
