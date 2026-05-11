#include "RecoveryDialog.h"

namespace gtk4 {

namespace {

GtkWidget* make_label(const char* text, bool error = false) {
    GtkWidget* lbl = gtk_label_new(text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    if (error) gtk_widget_add_css_class(lbl, "error");
    return lbl;
}

} // namespace

RecoveryDialog::RecoveryDialog(GtkWindow* parent, tesseract::Client& client)
    : parent_(parent), client_(client)
{
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "Verify this device");
    gtk_window_set_modal(GTK_WINDOW(window_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(window_), 460, 220);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);

    g_signal_connect(window_, "close-request",
                     G_CALLBACK(on_window_close_request), this);

    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(
        GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_margin_top(stack_,    16);
    gtk_widget_set_margin_bottom(stack_, 16);
    gtk_widget_set_margin_start(stack_,  16);
    gtk_widget_set_margin_end(stack_,    16);
    gtk_window_set_child(GTK_WINDOW(window_), stack_);

    // ---- Form page ----
    {
        GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

        GtkWidget* intro = make_label(
            "Enter your recovery key or passphrase to verify this device "
            "and decrypt historical messages.");
        gtk_box_append(GTK_BOX(col), intro);

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* lbl = make_label("Recovery key:");
        gtk_widget_set_size_request(lbl, 100, -1);
        gtk_box_append(GTK_BOX(row), lbl);

        key_entry_ = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(key_entry_), FALSE);
        gtk_widget_set_hexpand(key_entry_, TRUE);
        gtk_box_append(GTK_BOX(row), key_entry_);
        gtk_box_append(GTK_BOX(col), row);

        error_lbl_ = make_label("", /*error=*/true);
        gtk_widget_set_visible(error_lbl_, FALSE);
        gtk_box_append(GTK_BOX(col), error_lbl_);

        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(col), spacer);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        skip_btn_   = gtk_button_new_with_label("Skip");
        verify_btn_ = gtk_button_new_with_label("Verify");
        gtk_widget_add_css_class(verify_btn_, "suggested-action");

        gtk_box_append(GTK_BOX(buttons), skip_btn_);
        gtk_box_append(GTK_BOX(buttons), verify_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(verify_btn_, "clicked",
                         G_CALLBACK(on_verify_clicked), this);
        g_signal_connect(skip_btn_,   "clicked",
                         G_CALLBACK(on_skip_clicked),   this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "form");
    }

    // ---- Waiting page ----
    {
        GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

        progress_lbl_ = make_label("Unlocking secret storage…");
        gtk_box_append(GTK_BOX(col), progress_lbl_);

        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(col), spacer);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        close_btn_ = gtk_button_new_with_label("Close");
        gtk_widget_set_sensitive(close_btn_, FALSE);
        gtk_box_append(GTK_BOX(buttons), close_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(close_btn_, "clicked",
                         G_CALLBACK(on_close_clicked), this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "waiting");
    }

    show_form();
}

RecoveryDialog::~RecoveryDialog() {
    cancelled_.store(true);
    join_worker();
    if (loop_) {
        if (g_main_loop_is_running(loop_)) g_main_loop_quit(loop_);
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    if (window_) gtk_window_destroy(GTK_WINDOW(window_));
}

bool RecoveryDialog::run() {
    loop_ = g_main_loop_new(nullptr, FALSE);

    gtk_window_present(GTK_WINDOW(window_));
    gtk_widget_grab_focus(key_entry_);

    g_main_loop_run(loop_);

    return accepted_;
}

void RecoveryDialog::show_form() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "form");
    gtk_widget_set_sensitive(verify_btn_, TRUE);
    gtk_widget_set_sensitive(key_entry_,  TRUE);
}

void RecoveryDialog::show_waiting() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "waiting");
}

void RecoveryDialog::set_error(const std::string& msg) {
    if (msg.empty()) {
        gtk_widget_set_visible(error_lbl_, FALSE);
    } else {
        gtk_label_set_text(GTK_LABEL(error_lbl_), msg.c_str());
        gtk_widget_set_visible(error_lbl_, TRUE);
    }
}

void RecoveryDialog::set_progress(const tesseract::BackupProgress& progress) {
    if (!recover_done_) return;
    if (!progress_lbl_) return;

    switch (progress.state) {
        case tesseract::BackupState::Enabled: {
            std::string txt = "Done. Imported "
                + std::to_string(progress.imported_keys) + " keys.";
            gtk_label_set_text(GTK_LABEL(progress_lbl_), txt.c_str());
            gtk_widget_set_sensitive(close_btn_, TRUE);
            break;
        }
        case tesseract::BackupState::Downloading: {
            std::string txt = "Importing keys… "
                + std::to_string(progress.imported_keys) + " imported.";
            gtk_label_set_text(GTK_LABEL(progress_lbl_), txt.c_str());
            break;
        }
        case tesseract::BackupState::Disabled: {
            gtk_label_set_text(GTK_LABEL(progress_lbl_),
                               "Backup is not enabled on the server.");
            gtk_widget_set_sensitive(close_btn_, TRUE);
            break;
        }
        default:
            break;
    }
}

void RecoveryDialog::on_verify_clicked(GtkButton*, gpointer data) {
    static_cast<RecoveryDialog*>(data)->start_recover();
}

void RecoveryDialog::on_skip_clicked(GtkButton*, gpointer data) {
    static_cast<RecoveryDialog*>(data)->finish(false);
}

void RecoveryDialog::on_close_clicked(GtkButton*, gpointer data) {
    static_cast<RecoveryDialog*>(data)->finish(true);
}

void RecoveryDialog::on_window_close_request(GtkWindow*, gpointer data) {
    static_cast<RecoveryDialog*>(data)->finish(false);
}

void RecoveryDialog::start_recover() {
    const char* key_c = gtk_editable_get_text(GTK_EDITABLE(key_entry_));
    std::string key   = key_c ? key_c : "";
    if (key.empty()) {
        set_error("Please enter a recovery key or passphrase.");
        return;
    }
    set_error("");
    gtk_widget_set_sensitive(verify_btn_, FALSE);
    gtk_widget_set_sensitive(key_entry_,  FALSE);
    show_waiting();

    join_worker();
    cancelled_.store(false);

    worker_ = std::thread([this, key]() {
        auto res = client_.recover(key);
        if (cancelled_.load()) return;
        auto* p = new RecoveryIdle{ this, res.ok, res.message };
        g_idle_add(on_recover_done, p);
    });
}

gboolean RecoveryDialog::on_recover_done(gpointer data) {
    auto* d    = static_cast<RecoveryIdle*>(data);
    auto* self = d->dlg;

    self->join_worker();

    if (!d->ok) {
        self->set_error("Recovery failed: " + d->message);
        self->show_form();
        delete d;
        return G_SOURCE_REMOVE;
    }

    self->recover_done_ = true;
    gtk_label_set_text(GTK_LABEL(self->progress_lbl_),
                       "Downloading historical keys…");
    delete d;
    return G_SOURCE_REMOVE;
}

void RecoveryDialog::finish(bool accepted) {
    accepted_ = accepted;
    cancelled_.store(true);
    join_worker();
    if (loop_ && g_main_loop_is_running(loop_)) {
        g_main_loop_quit(loop_);
    }
    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void RecoveryDialog::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace gtk4
