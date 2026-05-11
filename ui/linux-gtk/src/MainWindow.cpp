#include "MainWindow.h"
#include "LoginDialog.h"

#include <tesseract/session_store.h>

#include <ctime>
#include <string>

namespace gtk4 {

// ---------------------------------------------------------------------------
// g_idle_add helpers
// ---------------------------------------------------------------------------

struct IdleMessage {
    MainWindow*                        window;
    std::unique_ptr<tesseract::Event> ev;
};

struct IdleRooms {
    MainWindow*                  window;
    std::vector<tesseract::RoomInfo> rooms;
};

struct IdleError {
    MainWindow* window;
    std::string context;
    std::string description;
    bool soft_logout;
};

struct IdleTimelineReset {
    MainWindow* window;
    std::string room_id;
};

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

void EventHandler::on_message(tesseract::Event* ev) {
    auto* p = new IdleMessage{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        std::unique_ptr<tesseract::Event>(ev)
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleMessage*>(data);
        d->window->push_event(std::move(d->ev));
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
    const std::string& description,
    bool soft_logout)
{
    auto* p = new IdleError{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        context,
        description,
        soft_logout
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleError*>(data);
        if (d->context == "sync_reconnect")
            d->window->handle_reconnect();
        else if (d->context == "sync_auth_error")
            d->window->handle_auth_error(d->soft_logout);
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
    gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 768);

#ifdef TESSERACT_ICON_SEARCH_PATH
    gtk_icon_theme_add_search_path(
        gtk_icon_theme_get_for_display(gtk_widget_get_display(window_)),
        TESSERACT_ICON_SEARCH_PATH);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "tesseract");
#endif

    g_object_set_data(G_OBJECT(window_), "cpp_window", this);

    // ---- CSS ----
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, R"css(
        .sidebar {
            background-color: #F0F2F5;
            border-right: 1px solid #D0D3D8;
        }
        .bubble-own {
            background-color: #0084FF;
            border-radius: 18px;
            padding: 10px 14px;
        }
        .bubble-own label {
            color: white;
        }
        .bubble-other {
            background-color: #E4E6EB;
            border-radius: 18px;
            padding: 10px 14px;
        }
        .bubble-other label {
            color: #1C1E21;
        }
        .sender-name {
            font-weight: bold;
            font-size: 12px;
            color: #555555;
        }
        .timestamp {
            font-size: 10px;
            color: rgba(0,0,0,0.45);
        }
        .timestamp-own {
            font-size: 10px;
            color: rgba(255,255,255,0.7);
        }
        .unread-badge {
            background-color: #0084FF;
            color: white;
            border-radius: 10px;
            padding: 0px 6px;
            font-size: 11px;
            font-weight: bold;
        }
    )css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // ---- Layout ----
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), hbox);

    // Sidebar
    GtkWidget* room_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(room_scroll, 260, -1);
    gtk_widget_add_css_class(room_scroll, "sidebar");
    room_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(room_list_), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(room_scroll), room_list_);
    gtk_box_append(GTK_BOX(hbox), room_scroll);
    g_signal_connect(room_list_, "row-activated",
                     G_CALLBACK(on_room_row_activated), this);

    // Chat area
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    // Message scroll area (replaces GtkTextView)
    msg_scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(msg_scroll_, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(msg_scroll_),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    msg_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_vexpand(msg_box_, TRUE);
    gtk_widget_set_valign(msg_box_, GTK_ALIGN_END);
    gtk_widget_set_margin_start(msg_box_, 12);
    gtk_widget_set_margin_end(msg_box_, 12);
    gtk_widget_set_margin_top(msg_box_, 12);
    gtk_widget_set_margin_bottom(msg_box_, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(msg_scroll_), msg_box_);
    gtk_box_append(GTK_BOX(vbox), msg_scroll_);

    // Compose row
    GtkWidget* compose_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(compose_bar, 12);
    gtk_widget_set_margin_end(compose_bar, 12);
    gtk_widget_set_margin_top(compose_bar, 8);
    gtk_widget_set_margin_bottom(compose_bar, 8);

    GtkWidget* input_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(input_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(input_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(input_scroll, -1, 40);

    input_text_view_ = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(input_text_view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(input_text_view_), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(input_text_view_), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(input_text_view_), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(input_text_view_), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(input_scroll), input_text_view_);
    gtk_box_append(GTK_BOX(compose_bar), input_scroll);

    // Key controller for Enter-to-send
    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(input_text_view_, key_ctrl);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), this);

    send_btn_ = gtk_button_new_with_label("Send");
    gtk_widget_set_valign(send_btn_, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(compose_bar), send_btn_);
    g_signal_connect(send_btn_, "clicked", G_CALLBACK(on_send_clicked), this);

    gtk_box_append(GTK_BOX(vbox), compose_bar);

    status_bar_ = gtk_label_new("Not logged in");
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_widget_set_margin_bottom(status_bar_, 2);
    gtk_box_append(GTK_BOX(vbox), status_bar_);

    gtk_widget_set_visible(window_, TRUE);

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
            my_user_id_ = client_.get_user_id();
            event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
            client_.start_sync(event_handler_.get());
            gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
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

    my_user_id_ = client_.get_user_id();
    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
    client_.start_sync(event_handler_.get());
    gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
}

void MainWindow::on_send_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->current_room_id_.empty()) return;

    GtkTextBuffer* buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->input_text_view_));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar* raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

    if (!raw || !*raw) { g_free(raw); return; }
    std::string body(raw);
    g_free(raw);

    // Trim leading/trailing whitespace
    auto l = body.find_first_not_of(" \t\n\r");
    auto r = body.find_last_not_of(" \t\n\r");
    if (l == std::string::npos) return;
    body = body.substr(l, r - l + 1);
    if (body.empty()) return;

    auto res = self->client_.send_message(self->current_room_id_, body);
    if (res)
        gtk_text_buffer_set_text(buf, "", 0);
}

gboolean MainWindow::on_key_pressed(
    GtkEventControllerKey* /*controller*/,
    guint keyval, guint /*keycode*/,
    GdkModifierType state,
    gpointer user_data)
{
    if (keyval == GDK_KEY_Return && !(state & GDK_SHIFT_MASK)) {
        on_send_clicked(nullptr, user_data);
        return TRUE;
    }
    return FALSE;
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

void MainWindow::push_event(std::unique_ptr<tesseract::Event> ev) {
    if (ev->room_id == current_room_id_)
        append_event(*ev);
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

void MainWindow::handle_auth_error(bool soft_logout) {
    if (soft_logout) {
        if (auto saved = tesseract::SessionStore::load()) {
            gtk_label_set_text(GTK_LABEL(status_bar_), "Reconnecting session…");
            if (client_.restore_session(*saved)) {
                my_user_id_ = client_.get_user_id();
                client_.start_sync(event_handler_.get());
                gtk_label_set_text(GTK_LABEL(status_bar_), "Reconnected");
                return;
            }
        }
    }
    tesseract::SessionStore::clear();
    client_.stop_sync();
    gtk_label_set_text(GTK_LABEL(status_bar_), "Session expired; please log in again.");
    do_login();
}

void MainWindow::push_error(std::string description) {
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}

void MainWindow::push_timeline_reset(std::string room_id) {
    if (room_id != current_room_id_) return;
    clear_messages();
}

void MainWindow::clear_messages() {
    msg_event_widgets_.clear();
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(msg_box_)) != nullptr)
        gtk_box_remove(GTK_BOX(msg_box_), child);
}

// ---------------------------------------------------------------------------

void MainWindow::populate_rooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Remove all existing rows.
    while (GtkWidget* child = gtk_widget_get_first_child(room_list_))
        gtk_list_box_remove(GTK_LIST_BOX(room_list_), child);

    for (const auto& r : rooms) {
        // Fetch avatar on first sight.
        if (!r.avatar_url.empty() &&
            avatar_cache_.find(r.avatar_url) == avatar_cache_.end())
        {
            avatar_cache_[r.avatar_url] = client_.fetch_avatar_bytes(r.id);
        }

        // Row: hbox with avatar + vbox(name + last_msg) + optional badge
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row_box, 8);
        gtk_widget_set_margin_end(row_box, 8);
        gtk_widget_set_margin_top(row_box, 6);
        gtk_widget_set_margin_bottom(row_box, 6);

        // Avatar (36×36 or placeholder)
        auto it = !r.avatar_url.empty() ?
                  avatar_cache_.find(r.avatar_url) : avatar_cache_.end();
        if (it != avatar_cache_.end() && !it->second.empty()) {
            GBytes*     gb  = g_bytes_new(it->second.data(), it->second.size());
            GError*     err = nullptr;
            GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
            g_bytes_unref(gb);
            if (tex) {
                GtkWidget* img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                gtk_image_set_pixel_size(GTK_IMAGE(img), kRoomAvatarSize);
                gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(row_box), img);
                g_object_unref(tex);
            } else {
                if (err) g_error_free(err);
                GtkWidget* ph = gtk_label_new(nullptr);
                gtk_widget_set_size_request(ph, kRoomAvatarSize, kRoomAvatarSize);
                gtk_box_append(GTK_BOX(row_box), ph);
            }
        } else {
            GtkWidget* ph = gtk_label_new(nullptr);
            gtk_widget_set_size_request(ph, kRoomAvatarSize, kRoomAvatarSize);
            gtk_box_append(GTK_BOX(row_box), ph);
        }

        // Text vbox: room name + last message preview
        GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(text_box, TRUE);
        gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

        GtkWidget* name_lbl = gtk_label_new(r.name.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_widget_add_css_class(name_lbl, "sender-name");
        gtk_box_append(GTK_BOX(text_box), name_lbl);

        if (!r.last_message_body.empty()) {
            GtkWidget* preview_lbl = gtk_label_new(r.last_message_body.c_str());
            gtk_label_set_ellipsize(GTK_LABEL(preview_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_xalign(GTK_LABEL(preview_lbl), 0.0f);
            gtk_widget_add_css_class(preview_lbl, "timestamp");
            gtk_box_append(GTK_BOX(text_box), preview_lbl);
        }

        gtk_box_append(GTK_BOX(row_box), text_box);

        // Unread badge
        if (r.unread_count > 0) {
            std::string badge_str = r.unread_count > 99 ?
                "99+" : std::to_string(r.unread_count);
            GtkWidget* badge = gtk_label_new(badge_str.c_str());
            gtk_widget_add_css_class(badge, "unread-badge");
            gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row_box), badge);
        }

        gtk_list_box_append(GTK_LIST_BOX(room_list_), row_box);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::append_event(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    // Update in place if we already have this event (sender profile resolved / edit).
    if (!ev.event_id.empty()) {
        auto it = msg_event_widgets_.find(ev.event_id);
        if (it != msg_event_widgets_.end()) {
            GtkWidget* row = it->second;
            const std::string& name = ev.sender_name.empty() ? ev.sender : ev.sender_name;
            auto* name_lbl = static_cast<GtkWidget*>(
                g_object_get_data(G_OBJECT(row), "name_lbl"));
            if (name_lbl)
                gtk_label_set_text(GTK_LABEL(name_lbl), name.c_str());
            std::string body_text;
            if (ev.type == tesseract::EventType::File) {
                const auto& file = static_cast<const tesseract::FileEvent&>(ev);
                body_text = "📎 " + file.file_name;
                if (file.file_size > 0) {
                    double kb = file.file_size / 1024.0;
                    char size_buf[32];
                    if (kb < 1024)
                        snprintf(size_buf, sizeof(size_buf), " (%.1f KB)", kb);
                    else
                        snprintf(size_buf, sizeof(size_buf), " (%.1f MB)", kb / 1024.0);
                    body_text += size_buf;
                }
            } else if (ev.type == tesseract::EventType::Image) {
                const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
                body_text = img.body.empty() ? "🖼 Image" : img.body;
            } else {
                body_text = ev.body;
            }
            auto* body_lbl = static_cast<GtkWidget*>(
                g_object_get_data(G_OBJECT(row), "body_lbl"));
            if (body_lbl)
                gtk_label_set_text(GTK_LABEL(body_lbl), body_text.c_str());
            if (!ev.sender_avatar_url.empty()) {
                if (user_avatar_cache_.find(ev.sender_avatar_url) == user_avatar_cache_.end())
                    user_avatar_cache_[ev.sender_avatar_url] =
                        client_.fetch_media_bytes(ev.sender_avatar_url);
                auto avit = user_avatar_cache_.find(ev.sender_avatar_url);
                if (avit != user_avatar_cache_.end() && !avit->second.empty()) {
                    auto* old_av = static_cast<GtkWidget*>(
                        g_object_get_data(G_OBJECT(row), "avatar"));
                    if (old_av) {
                        GBytes*     gb  = g_bytes_new(avit->second.data(), avit->second.size());
                        GError*     err = nullptr;
                        GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
                        g_bytes_unref(gb);
                        if (tex) {
                            GtkWidget* new_img =
                                gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                            gtk_image_set_pixel_size(GTK_IMAGE(new_img), kMsgAvatarSize);
                            gtk_widget_set_valign(new_img, GTK_ALIGN_START);
                            gtk_box_remove(GTK_BOX(row), old_av);
                            gtk_box_prepend(GTK_BOX(row), new_img);
                            gtk_widget_set_visible(new_img, TRUE);
                            g_object_set_data(G_OBJECT(row), "avatar", new_img);
                            g_object_unref(tex);
                        } else {
                            if (err) g_error_free(err);
                        }
                    }
                }
            }
            return;
        }
    }

    const bool is_own = (!my_user_id_.empty() && ev.sender == my_user_id_);
    const std::string& name =
        ev.sender_name.empty() ? ev.sender : ev.sender_name;

    // Fetch/cache sender avatar.
    if (!ev.sender_avatar_url.empty() &&
        user_avatar_cache_.find(ev.sender_avatar_url) == user_avatar_cache_.end())
    {
        user_avatar_cache_[ev.sender_avatar_url] =
            client_.fetch_media_bytes(ev.sender_avatar_url);
    }

    // Timestamp string (HH:MM).
    std::string ts_str;
    if (ev.timestamp > 0) {
        time_t t = static_cast<time_t>(ev.timestamp / 1000);
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        char buf[6];
        strftime(buf, sizeof(buf), "%H:%M", &tm_info);
        ts_str = buf;
    }

    // ---- Outer row ----
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 0);
    gtk_widget_set_margin_end(row, 0);
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);

    if (!is_own) {
        // ---- Sender avatar ----
        GtkWidget* av_widget = nullptr;
        auto avit = !ev.sender_avatar_url.empty() ?
                    user_avatar_cache_.find(ev.sender_avatar_url) :
                    user_avatar_cache_.end();
        if (avit != user_avatar_cache_.end() && !avit->second.empty()) {
            GBytes*     gb  = g_bytes_new(avit->second.data(), avit->second.size());
            GError*     err = nullptr;
            GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
            g_bytes_unref(gb);
            if (tex) {
                av_widget = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                gtk_image_set_pixel_size(GTK_IMAGE(av_widget), kMsgAvatarSize);
                gtk_widget_set_valign(av_widget, GTK_ALIGN_START);
                g_object_unref(tex);
            } else {
                if (err) g_error_free(err);
            }
        }
        if (!av_widget) {
            av_widget = gtk_label_new(nullptr);
            gtk_widget_set_size_request(av_widget, kMsgAvatarSize, kMsgAvatarSize);
        }
        gtk_box_append(GTK_BOX(row), av_widget);
        g_object_set_data(G_OBJECT(row), "avatar", av_widget);
    }

    // ---- Content vbox (name + bubble) ----
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);

    if (!is_own) {
        GtkWidget* name_lbl = gtk_label_new(name.c_str());
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_widget_add_css_class(name_lbl, "sender-name");
        gtk_box_append(GTK_BOX(content), name_lbl);
        g_object_set_data(G_OBJECT(row), "name_lbl", name_lbl);
    }

    // ---- Bubble ----
    GtkWidget* bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(bubble, is_own ? "bubble-own" : "bubble-other");
    gtk_widget_set_halign(bubble, is_own ? GTK_ALIGN_END : GTK_ALIGN_START);

    // Body text
    std::string body_text;
    if (ev.type == tesseract::EventType::File) {
        const auto& file = static_cast<const tesseract::FileEvent&>(ev);
        body_text = "📎 " + file.file_name;
        if (file.file_size > 0) {
            double kb = file.file_size / 1024.0;
            char size_buf[32];
            if (kb < 1024)
                snprintf(size_buf, sizeof(size_buf), " (%.1f KB)", kb);
            else
                snprintf(size_buf, sizeof(size_buf), " (%.1f MB)", kb / 1024.0);
            body_text += size_buf;
        }
    } else if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        body_text = img.body.empty() ? "🖼 Image" : img.body;
    } else {
        body_text = ev.body;
    }

    GtkWidget* body_lbl = gtk_label_new(body_text.c_str());
    gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(body_lbl), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 55);
    gtk_box_append(GTK_BOX(bubble), body_lbl);
    g_object_set_data(G_OBJECT(row), "body_lbl", body_lbl);

    // Timestamp
    if (!ts_str.empty()) {
        GtkWidget* ts_lbl = gtk_label_new(ts_str.c_str());
        gtk_label_set_xalign(GTK_LABEL(ts_lbl), 1.0f);
        gtk_widget_add_css_class(ts_lbl, is_own ? "timestamp-own" : "timestamp");
        gtk_box_append(GTK_BOX(bubble), ts_lbl);
    }

    gtk_box_append(GTK_BOX(content), bubble);
    gtk_widget_set_halign(content, is_own ? GTK_ALIGN_END : GTK_ALIGN_START);

    if (is_own) {
        // Spacer on the left, content on the right
        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(row), spacer);
        gtk_box_append(GTK_BOX(row), content);
    } else {
        gtk_box_append(GTK_BOX(row), content);
        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(row), spacer);
    }

    if (!ev.event_id.empty())
        msg_event_widgets_[ev.event_id] = row;
    gtk_box_append(GTK_BOX(msg_box_), row);
    gtk_widget_set_visible(row, TRUE);

    // Scroll to bottom after layout.
    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(msg_scroll_));
    g_idle_add([](gpointer data) -> gboolean {
        GtkAdjustment* a = static_cast<GtkAdjustment*>(data);
        gtk_adjustment_set_value(a,
            gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
        return G_SOURCE_REMOVE;
    }, adj);
}

} // namespace gtk4
