#include "MainWindow.h"
#include <string>

namespace gtk4 {

// ---------------------------------------------------------------------------
// g_idle_add helpers: heap-allocated payload structs
// ---------------------------------------------------------------------------

struct IdleMessage {
    MainWindow*    window;
    matrix::Message msg;
};

struct IdleRooms {
    MainWindow*                  window;
    std::vector<matrix::RoomInfo> rooms;
};

struct IdleError {
    MainWindow*  window;
    std::string  description;
};

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_message(const matrix::Message& msg) {
    auto* p = new IdleMessage{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        msg
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleMessage*>(data);
        d->window->push_message(std::move(d->msg));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_rooms_updated(
    const std::vector<matrix::RoomInfo>& rooms)
{
    auto* p = new IdleRooms{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        rooms
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleRooms*>(data);
        d->window->push_rooms(std::move(d->rooms));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_sync_error(
    const std::string& /*context*/,
    const std::string& description)
{
    auto* p = new IdleError{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        description
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleError*>(data);
        d->window->push_error(std::move(d->description));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(GtkApplication* app) : app_(app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Matrix Client");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1024, 768);

    // Store C++ pointer so idle callbacks can reach us.
    g_object_set_data(G_OBJECT(window_), "cpp_window", this);

    // ---- Layout: horizontal box ----
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), hbox);

    // ---- Room list (left) ----
    GtkWidget* room_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(room_scroll, 200, -1);
    room_list_ = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(room_scroll), room_list_);
    gtk_box_append(GTK_BOX(hbox), room_scroll);

    g_signal_connect(room_list_, "row-activated",
                     G_CALLBACK(on_room_row_activated), this);

    // ---- Right panel ----
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    // Message view
    msg_scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(msg_scroll_, TRUE);
    msg_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(msg_view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(msg_view_), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(msg_scroll_), msg_view_);
    gtk_box_append(GTK_BOX(vbox), msg_scroll_);

    // Input row
    GtkWidget* input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(input_row, 4);
    gtk_widget_set_margin_end(input_row, 4);
    gtk_widget_set_margin_bottom(input_row, 4);
    gtk_box_append(GTK_BOX(vbox), input_row);

    input_entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(input_entry_, TRUE);
    gtk_box_append(GTK_BOX(input_row), input_entry_);

    send_btn_ = gtk_button_new_with_label("Send");
    gtk_box_append(GTK_BOX(input_row), send_btn_);
    g_signal_connect(send_btn_, "clicked", G_CALLBACK(on_send_clicked), this);

    // Status bar
    status_bar_ = gtk_label_new("Not logged in");
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_box_append(GTK_BOX(vbox), status_bar_);

    gtk_widget_show(window_);

    // Trigger login after the main loop starts.
    g_idle_add([](gpointer data) -> gboolean {
        static_cast<MainWindow*>(data)->do_login();
        return G_SOURCE_REMOVE;
    }, this);
}

MainWindow::~MainWindow() {
    client_.stop_sync();
}

// ---------------------------------------------------------------------------

void MainWindow::do_login() {
    // TODO: show a login dialog.
    gtk_label_set_text(GTK_LABEL(status_bar_), "Logging in…");

    auto res = client_.login("https://matrix.org", "user", "password");
    if (res) {
        event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
        client_.start_sync(event_handler_.get());
        gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
    } else {
        gtk_label_set_text(GTK_LABEL(status_bar_), res.message.c_str());
    }
}

void MainWindow::on_send_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->current_room_id_.empty()) return;

    const char* text = gtk_editable_get_text(GTK_EDITABLE(self->input_entry_));
    if (!text || !*text) return;

    auto res = self->client_.send_message(self->current_room_id_, text);
    if (res) {
        gtk_editable_set_text(GTK_EDITABLE(self->input_entry_), "");
    }
}

void MainWindow::on_room_row_activated(
    GtkListBox*, GtkListBoxRow* row, gpointer user_data)
{
    auto* self  = static_cast<MainWindow*>(user_data);
    int   index = gtk_list_box_row_get_index(row);

    if (index < 0 || index >= static_cast<int>(self->rooms_.size())) return;
    self->current_room_id_ = self->rooms_[index].id;

    // Clear and load messages.
    GtkTextBuffer* buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->msg_view_));
    gtk_text_buffer_set_text(buf, "", 0);

    auto msgs = self->client_.room_messages(self->current_room_id_, 50);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        self->append_message(it->sender, it->body);
}

void MainWindow::on_login_clicked(GtkButton*, gpointer user_data) {
    static_cast<MainWindow*>(user_data)->do_login();
}

// ---------------------------------------------------------------------------

void MainWindow::push_message(matrix::Message msg) {
    if (msg.room_id == current_room_id_)
        append_message(msg.sender, msg.body);
    populate_rooms(client_.list_rooms());
}

void MainWindow::push_rooms(std::vector<matrix::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populate_rooms(rooms_);
}

void MainWindow::push_error(std::string description) {
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}

void MainWindow::populate_rooms(const std::vector<matrix::RoomInfo>& rooms) {
    // Remove existing rows.
    while (GtkWidget* child =
               gtk_widget_get_first_child(room_list_))
    {
        gtk_list_box_remove(GTK_LIST_BOX(room_list_), child);
    }

    for (const auto& r : rooms) {
        std::string label = r.name;
        if (r.unread_count > 0)
            label += " (" + std::to_string(r.unread_count) + ")";

        GtkWidget* lbl = gtk_label_new(label.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_top(lbl, 4);
        gtk_widget_set_margin_bottom(lbl, 4);
        gtk_list_box_append(GTK_LIST_BOX(room_list_), lbl);
    }
}

void MainWindow::append_message(
    const std::string& sender,
    const std::string& body)
{
    GtkTextBuffer* buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(msg_view_));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);

    std::string line = sender + ": " + body + "\n";
    gtk_text_buffer_insert(buf, &end, line.c_str(), -1);

    // Scroll to bottom.
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark* mark =
        gtk_text_buffer_get_mark(buf, "insert");
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(msg_view_), mark);
}

} // namespace gtk4
