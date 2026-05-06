#include "LoginDialog.h"

namespace gtk4 {

namespace {

GtkWidget* make_label(const char* text, bool error = false) {
    GtkWidget* lbl = gtk_label_new(text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    if (error) {
        gtk_widget_add_css_class(lbl, "error");
    }
    return lbl;
}

} // namespace

// ---------------------------------------------------------------------------

LoginDialog::LoginDialog(GtkWindow* parent, tesseract::Client& client)
    : parent_(parent), client_(client)
{
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "Sign in to Tesseract");
    gtk_window_set_modal(GTK_WINDOW(window_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(window_), 460, 220);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);

    g_signal_connect(window_, "close-request",
                     G_CALLBACK(on_window_close_request), this);

    // ---- Stack ----
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

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* lbl = make_label("Homeserver:");
        gtk_widget_set_size_request(lbl, 100, -1);
        gtk_box_append(GTK_BOX(row), lbl);

        hs_entry_ = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(hs_entry_), "matrix.org");
        gtk_widget_set_hexpand(hs_entry_, TRUE);
        gtk_box_append(GTK_BOX(row), hs_entry_);
        gtk_box_append(GTK_BOX(col), row);

        error_lbl_ = make_label("", /*error=*/true);
        gtk_widget_set_visible(error_lbl_, FALSE);
        gtk_box_append(GTK_BOX(col), error_lbl_);

        // Spacer
        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(col), spacer);

        // Buttons row
        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        close_btn_ = gtk_button_new_with_label("Close");
        signin_btn_ = gtk_button_new_with_label("Sign in");
        gtk_widget_add_css_class(signin_btn_, "suggested-action");

        gtk_box_append(GTK_BOX(buttons), close_btn_);
        gtk_box_append(GTK_BOX(buttons), signin_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(signin_btn_, "clicked",
                         G_CALLBACK(on_signin_clicked), this);
        g_signal_connect(close_btn_,  "clicked",
                         G_CALLBACK(on_close_clicked),  this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "form");
    }

    // ---- Waiting page ----
    {
        GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

        status_lbl_ = make_label("Waiting for sign-in in your browser…");
        gtk_box_append(GTK_BOX(col), status_lbl_);

        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(col), spacer);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        cancel_btn_ = gtk_button_new_with_label("Cancel");
        gtk_box_append(GTK_BOX(buttons), cancel_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(cancel_btn_, "clicked",
                         G_CALLBACK(on_cancel_clicked), this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "waiting");
    }

    show_form();
}

LoginDialog::~LoginDialog() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    if (loop_) {
        if (g_main_loop_is_running(loop_)) g_main_loop_quit(loop_);
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    if (window_) gtk_window_destroy(GTK_WINDOW(window_));
}

bool LoginDialog::run() {
    loop_ = g_main_loop_new(nullptr, FALSE);

    gtk_window_present(GTK_WINDOW(window_));
    gtk_widget_grab_focus(hs_entry_);

    // Pump the main loop until finish() quits it.
    g_main_loop_run(loop_);

    return accepted_;
}

// ---------------------------------------------------------------------------

void LoginDialog::show_form() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "form");
    gtk_widget_set_sensitive(signin_btn_, TRUE);
    gtk_widget_set_sensitive(hs_entry_,   TRUE);
}

void LoginDialog::show_waiting() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "waiting");
    gtk_label_set_text(GTK_LABEL(status_lbl_),
                       "Waiting for sign-in in your browser…");
    gtk_widget_set_sensitive(cancel_btn_, TRUE);
}

void LoginDialog::set_error(const std::string& msg) {
    if (msg.empty()) {
        gtk_widget_set_visible(error_lbl_, FALSE);
    } else {
        gtk_label_set_text(GTK_LABEL(error_lbl_), msg.c_str());
        gtk_widget_set_visible(error_lbl_, TRUE);
    }
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void LoginDialog::on_signin_clicked(GtkButton*, gpointer user_data) {
    static_cast<LoginDialog*>(user_data)->start_phase1();
}

void LoginDialog::on_close_clicked(GtkButton*, gpointer user_data) {
    static_cast<LoginDialog*>(user_data)->finish(false);
}

void LoginDialog::on_cancel_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<LoginDialog*>(user_data);
    self->cancelled_.store(true);
    self->client_.cancel_oauth();
    gtk_label_set_text(GTK_LABEL(self->status_lbl_), "Cancelling…");
    gtk_widget_set_sensitive(self->cancel_btn_, FALSE);
    // Worker thread will return; on_await_done will surface the result.
}

void LoginDialog::on_window_close_request(GtkWindow*, gpointer user_data) {
    static_cast<LoginDialog*>(user_data)->finish(false);
}

// ---------------------------------------------------------------------------
// Worker phases
// ---------------------------------------------------------------------------

void LoginDialog::start_phase1() {
    const char* hs_c = gtk_editable_get_text(GTK_EDITABLE(hs_entry_));
    std::string hs   = hs_c ? hs_c : "";
    if (hs.empty()) {
        set_error("Please enter a homeserver.");
        return;
    }
    set_error("");
    gtk_widget_set_sensitive(signin_btn_, FALSE);
    gtk_widget_set_sensitive(hs_entry_,   FALSE);

    join_worker();
    cancelled_.store(false);

    worker_ = std::thread([this, hs]() {
        auto flow = client_.begin_oauth(hs);
        if (cancelled_.load()) return;

        auto* p = new LoginIdle{
            this, flow.ok,
            flow.ok ? flow.auth_url : flow.message,
        };
        g_idle_add(on_begin_done, p);
    });
}

gboolean LoginDialog::on_begin_done(gpointer data) {
    auto* d    = static_cast<LoginIdle*>(data);
    auto* self = d->dlg;

    self->join_worker();

    if (!d->ok) {
        self->set_error("Sign-in failed: " + d->text);
        gtk_widget_set_sensitive(self->signin_btn_, TRUE);
        gtk_widget_set_sensitive(self->hs_entry_,   TRUE);
        delete d;
        return G_SOURCE_REMOVE;
    }

    // text == auth_url
    tesseract::Client::open_in_browser(d->text);
    self->show_waiting();
    self->start_phase2();
    delete d;
    return G_SOURCE_REMOVE;
}

void LoginDialog::start_phase2() {
    join_worker();
    cancelled_.store(false);

    worker_ = std::thread([this]() {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        auto* p = new LoginIdle{ this, res.ok, res.message };
        g_idle_add(on_await_done, p);
    });
}

gboolean LoginDialog::on_await_done(gpointer data) {
    auto* d    = static_cast<LoginIdle*>(data);
    auto* self = d->dlg;

    self->join_worker();

    if (d->ok) {
        self->finish(true);
    } else {
        self->set_error("Sign-in failed: " + d->text);
        self->show_form();
    }
    delete d;
    return G_SOURCE_REMOVE;
}

void LoginDialog::finish(bool accepted) {
    accepted_ = accepted;
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    if (loop_ && g_main_loop_is_running(loop_)) {
        g_main_loop_quit(loop_);
    }
    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void LoginDialog::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace gtk4
