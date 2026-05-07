#include "MainWindow.h"
#include "LoginDialog.h"

#include <tesseract/session_store.h>

#include <string>

namespace gtk4 {

// ---------------------------------------------------------------------------
// g_idle_add helpers
// ---------------------------------------------------------------------------

struct IdleMessage {
    MainWindow*    window;
    tesseract::Message msg;
};

struct IdleRooms {
    MainWindow*                  window;
    std::vector<tesseract::RoomInfo> rooms;
};

struct IdleError {
    MainWindow* window;
    std::string context;
    std::string description;
};

struct IdleTimelineReset {
    MainWindow* window;
    std::string room_id;
};

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_message(const tesseract::Message& msg) {
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
    const std::vector<tesseract::RoomInfo>& rooms)
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
    const std::string& context,
    const std::string& description)
{
    auto* p = new IdleError{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        context,
        description
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleError*>(data);
        if (d->context == "sync_reconnect")
            d->window->handle_reconnect();
        else
            d->window->push_error(std::move(d->description));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_timeline_reset(const std::string& room_id) {
    auto* p = new IdleTimelineReset{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        room_id
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleTimelineReset*>(data);
        d->window->push_timeline_reset(std::move(d->room_id));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_session_saved(const std::string& session_json) {
    tesseract::SessionStore::save(session_json);
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(GtkApplication* app) : app_(app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Tesseract");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1024, 768);

    // Register the bundled icon so the window manager can show it.
#ifdef TESSERACT_ICON_SEARCH_PATH
    gtk_icon_theme_add_search_path(
        gtk_icon_theme_get_for_display(gtk_widget_get_display(window_)),
        TESSERACT_ICON_SEARCH_PATH);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "tesseract");
#endif

    g_object_set_data(G_OBJECT(window_), "cpp_window", this);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), hbox);

    GtkWidget* room_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(room_scroll, 200, -1);
    room_list_ = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(room_scroll), room_list_);
    gtk_box_append(GTK_BOX(hbox), room_scroll);

    g_signal_connect(room_list_, "row-activated",
                     G_CALLBACK(on_room_row_activated), this);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    msg_scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(msg_scroll_, TRUE);
    msg_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(msg_view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(msg_view_), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(msg_scroll_), msg_view_);
    gtk_box_append(GTK_BOX(vbox), msg_scroll_);

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

    status_bar_ = gtk_label_new("Not logged in");
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_box_append(GTK_BOX(vbox), status_bar_);

    gtk_widget_show(window_);

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
    if (auto saved = tesseract::SessionStore::load()) {
        gtk_label_set_text(GTK_LABEL(status_bar_), "Restoring session…");
        auto res = client_.restore_session(*saved);
        if (res) {
            event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
            client_.start_sync(event_handler_.get());
            gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
            // Room list arrives via on_rooms_updated from RoomListService.
            return;
        }
        tesseract::SessionStore::clear();
        std::string msg = "Saved session expired: " + res.message;
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
    }

    LoginDialog dlg(GTK_WINDOW(window_), client_);
    if (!dlg.run()) {
        gtk_label_set_text(GTK_LABEL(status_bar_), "Not logged in");
        return;
    }

    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
    client_.start_sync(event_handler_.get());
    gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
}

void MainWindow::on_send_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->current_room_id_.empty()) return;

    const char* text = gtk_editable_get_text(GTK_EDITABLE(self->input_entry_));
    if (!text || !*text) return;

    auto res = self->client_.send_message(self->current_room_id_, text);
    if (res)
        gtk_editable_set_text(GTK_EDITABLE(self->input_entry_), "");
}

void MainWindow::on_room_row_activated(
    GtkListBox*, GtkListBoxRow* row, gpointer user_data)
{
    auto* self  = static_cast<MainWindow*>(user_data);
    int   index = gtk_list_box_row_get_index(row);

    if (index < 0 || index >= static_cast<int>(self->rooms_.size())) return;

    const std::string new_id = self->rooms_[index].id;

    if (!self->current_room_id_.empty() && self->current_room_id_ != new_id)
        self->client_.unsubscribe_room(self->current_room_id_);

    self->current_room_id_ = new_id;

    auto res = self->client_.subscribe_room(self->current_room_id_);
    if (res)
        self->client_.paginate_back(self->current_room_id_, 50);
}

void MainWindow::on_login_clicked(GtkButton*, gpointer user_data) {
    static_cast<MainWindow*>(user_data)->do_login();
}

// ---------------------------------------------------------------------------

void MainWindow::push_message(tesseract::Message msg) {
    if (msg.room_id == current_room_id_)
        append_message(msg);
    // Room list unread counts update via push_rooms from RoomListService.
}

void MainWindow::push_rooms(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populate_rooms(rooms_);
}

void MainWindow::handle_reconnect() {
    gtk_label_set_text(GTK_LABEL(status_bar_), "Sync error: reconnecting…");
    client_.stop_sync();
    do_login();
}

void MainWindow::push_error(std::string description) {
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}

void MainWindow::push_timeline_reset(std::string room_id) {
    if (room_id != current_room_id_) return;

    GtkTextBuffer* buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(msg_view_));
    gtk_text_buffer_set_text(buf, "", 0);
}

void MainWindow::populate_rooms(const std::vector<tesseract::RoomInfo>& rooms) {
    while (GtkWidget* child = gtk_widget_get_first_child(room_list_))
        gtk_list_box_remove(GTK_LIST_BOX(room_list_), child);

    for (const auto& r : rooms) {
        // Populate avatar cache on first sight of this URL.
        if (!r.avatar_url.empty() && avatar_cache_.find(r.avatar_url) == avatar_cache_.end()) {
            auto bytes = client_.fetch_avatar_bytes(r.id);
            avatar_cache_[r.avatar_url] = std::move(bytes);
        }

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);

        // Avatar slot (32×32).
        auto it = !r.avatar_url.empty() ? avatar_cache_.find(r.avatar_url) : avatar_cache_.end();
        if (it != avatar_cache_.end() && !it->second.empty()) {
            GBytes*     gb  = g_bytes_new(it->second.data(), it->second.size());
            GError*     err = nullptr;
            GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
            g_bytes_unref(gb);
            if (tex) {
                GtkWidget* img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                gtk_image_set_pixel_size(GTK_IMAGE(img), kRoomAvatarSize);
                gtk_box_append(GTK_BOX(row), img);
                g_object_unref(tex);
            } else {
                if (err) g_error_free(err);
                GtkWidget* placeholder = gtk_label_new(nullptr);
                gtk_widget_set_size_request(placeholder, kRoomAvatarSize, kRoomAvatarSize);
                gtk_box_append(GTK_BOX(row), placeholder);
            }
        } else {
            GtkWidget* placeholder = gtk_label_new(nullptr);
            gtk_widget_set_size_request(placeholder, kRoomAvatarSize, kRoomAvatarSize);
            gtk_box_append(GTK_BOX(row), placeholder);
        }

        // Room name + unread count.
        std::string label = r.name;
        if (r.unread_count > 0)
            label += " (" + std::to_string(r.unread_count) + ")";
        GtkWidget* lbl = gtk_label_new(label.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), lbl);

        gtk_list_box_append(GTK_LIST_BOX(room_list_), row);
    }
}

void MainWindow::append_message(const tesseract::Message& msg) {
    const std::string& name      = msg.sender_name.empty() ? msg.sender : msg.sender_name;
    const std::string& avatarUrl = msg.sender_avatar_url;

    // Fetch and cache sender avatar on first sight of this mxc URL.
    if (!avatarUrl.empty() && user_avatar_cache_.find(avatarUrl) == user_avatar_cache_.end())
        user_avatar_cache_[avatarUrl] = client_.fetch_media_bytes(avatarUrl);

    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(msg_view_));
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf, &iter);

    // Inline avatar via GtkTextChildAnchor.
    auto it = !avatarUrl.empty() ? user_avatar_cache_.find(avatarUrl) : user_avatar_cache_.end();
    if (it != user_avatar_cache_.end() && !it->second.empty()) {
        GBytes*     gb  = g_bytes_new(it->second.data(), it->second.size());
        GError*     err = nullptr;
        GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
        g_bytes_unref(gb);
        if (tex) {
            GtkTextChildAnchor* anchor =
                gtk_text_buffer_create_child_anchor(buf, &iter);
            GtkWidget* img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
            gtk_image_set_pixel_size(GTK_IMAGE(img), kUserAvatarSize);
            gtk_text_view_add_child_at_anchor(
                GTK_TEXT_VIEW(msg_view_), img, anchor);
            gtk_widget_show(img);
            g_object_unref(tex);
            gtk_text_buffer_get_end_iter(buf, &iter);
            gtk_text_buffer_insert(buf, &iter, " ", 1);
        } else {
            if (err) g_error_free(err);
        }
    }

    // Bold sender name — reuse a named tag so it isn't recreated per message.
    gtk_text_buffer_get_end_iter(buf, &iter);
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag* bold = gtk_text_tag_table_lookup(table, "bold");
    if (!bold)
        bold = gtk_text_buffer_create_tag(buf, "bold",
                                          "weight", PANGO_WEIGHT_BOLD, nullptr);
    std::string header = name + ": ";
    gtk_text_buffer_insert_with_tags(buf, &iter, header.c_str(), -1, bold, nullptr);

    // Plain message body + newline.
    gtk_text_buffer_get_end_iter(buf, &iter);
    std::string line = msg.body + "\n";
    gtk_text_buffer_insert(buf, &iter, line.c_str(), -1);

    gtk_text_buffer_get_end_iter(buf, &iter);
    GtkTextMark* mark = gtk_text_buffer_get_mark(buf, "insert");
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(msg_view_), mark);
}

} // namespace gtk4
