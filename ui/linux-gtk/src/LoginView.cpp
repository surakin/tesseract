#include "LoginView.h"

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

LoginView::LoginView(tesseract::Client& client) : client_(client) {
    // ---- Root: vertical box centered horizontally + vertically ----
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);

    // Outer top spacer
    GtkWidget* top_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(top_spacer, TRUE);
    gtk_box_append(GTK_BOX(root_), top_spacer);

    // Centered horizontal row with the card
    GtkWidget* center_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(center_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root_), center_row);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(card, 420, -1);
    gtk_widget_add_css_class(card, "login-card");
    gtk_widget_set_margin_top(card,    24);
    gtk_widget_set_margin_bottom(card, 24);
    gtk_widget_set_margin_start(card,  24);
    gtk_widget_set_margin_end(card,    24);
    gtk_box_append(GTK_BOX(center_row), card);

    GtkWidget* title = gtk_label_new("Sign in to Tesseract");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_add_css_class(title, "login-title");
    gtk_box_append(GTK_BOX(card), title);

    status_lbl_ = make_label("");
    gtk_widget_set_visible(status_lbl_, FALSE);
    gtk_box_append(GTK_BOX(card), status_lbl_);

    // ---- Stack: form / waiting pages ----
    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(
        GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_append(GTK_BOX(card), stack_);

    // Form page
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

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        signin_btn_ = gtk_button_new_with_label("Sign in");
        gtk_widget_add_css_class(signin_btn_, "suggested-action");

        gtk_box_append(GTK_BOX(buttons), signin_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(signin_btn_, "clicked",
                         G_CALLBACK(on_signin_clicked), this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "form");
    }

    // Waiting page
    {
        GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

        wait_lbl_ = make_label("Waiting for sign-in in your browser…");
        gtk_box_append(GTK_BOX(col), wait_lbl_);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);

        cancel_btn_ = gtk_button_new_with_label("Cancel");
        gtk_box_append(GTK_BOX(buttons), cancel_btn_);
        gtk_box_append(GTK_BOX(col), buttons);

        g_signal_connect(cancel_btn_, "clicked",
                         G_CALLBACK(on_cancel_clicked), this);

        gtk_stack_add_named(GTK_STACK(stack_), col, "waiting");
    }

    // Outer bottom spacer
    GtkWidget* bottom_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(bottom_spacer, TRUE);
    gtk_box_append(GTK_BOX(root_), bottom_spacer);

    show_form();
}

LoginView::~LoginView() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
}

// ---------------------------------------------------------------------------

void LoginView::reset() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    cancelled_.store(false);
    set_error("");
    show_form();
}

void LoginView::set_status_message(const std::string& msg) {
    if (msg.empty()) {
        gtk_widget_set_visible(status_lbl_, FALSE);
    } else {
        gtk_label_set_text(GTK_LABEL(status_lbl_), msg.c_str());
        gtk_widget_set_visible(status_lbl_, TRUE);
    }
}

void LoginView::show_form() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "form");
    gtk_widget_set_sensitive(signin_btn_, TRUE);
    gtk_widget_set_sensitive(hs_entry_,   TRUE);
    gtk_widget_grab_focus(hs_entry_);
}

void LoginView::show_waiting() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "waiting");
    gtk_label_set_text(GTK_LABEL(wait_lbl_),
                       "Waiting for sign-in in your browser…");
    gtk_widget_set_sensitive(cancel_btn_, TRUE);
}

void LoginView::set_error(const std::string& msg) {
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

void LoginView::on_signin_clicked(GtkButton*, gpointer user_data) {
    static_cast<LoginView*>(user_data)->start_phase1();
}

void LoginView::on_cancel_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<LoginView*>(user_data);
    self->cancelled_.store(true);
    self->client_.cancel_oauth();
    gtk_label_set_text(GTK_LABEL(self->wait_lbl_), "Cancelling…");
    gtk_widget_set_sensitive(self->cancel_btn_, FALSE);
}

// ---------------------------------------------------------------------------
// Worker phases
// ---------------------------------------------------------------------------

void LoginView::start_phase1() {
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

        auto* p = new LoginViewIdle{
            this, flow.ok,
            flow.ok ? flow.auth_url : flow.message,
        };
        g_idle_add(on_begin_done, p);
    });
}

gboolean LoginView::on_begin_done(gpointer data) {
    auto* d    = static_cast<LoginViewIdle*>(data);
    auto* self = d->view;

    self->join_worker();

    if (!d->ok) {
        self->set_error("Sign-in failed: " + d->text);
        gtk_widget_set_sensitive(self->signin_btn_, TRUE);
        gtk_widget_set_sensitive(self->hs_entry_,   TRUE);
        delete d;
        return G_SOURCE_REMOVE;
    }

    tesseract::Client::open_in_browser(d->text);
    self->show_waiting();
    self->start_phase2();
    delete d;
    return G_SOURCE_REMOVE;
}

void LoginView::start_phase2() {
    join_worker();
    cancelled_.store(false);

    worker_ = std::thread([this]() {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        auto* p = new LoginViewIdle{ this, res.ok, res.message };
        g_idle_add(on_await_done, p);
    });
}

gboolean LoginView::on_await_done(gpointer data) {
    auto* d    = static_cast<LoginViewIdle*>(data);
    auto* self = d->view;

    self->join_worker();

    if (d->ok) {
        if (self->on_success_) self->on_success_();
    } else {
        self->set_error("Sign-in failed: " + d->text);
        self->show_form();
    }
    delete d;
    return G_SOURCE_REMOVE;
}

void LoginView::join_worker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace gtk4
