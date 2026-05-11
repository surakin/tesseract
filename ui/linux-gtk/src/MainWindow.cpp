#include "MainWindow.h"
#include "LoginView.h"

#include <thread>

#include <tesseract/session_store.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <gdk-pixbuf/gdk-pixbuf.h>

namespace gtk4 {

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

// Decode raw image bytes, scale down to fit within max_w×max_h (preserving
// aspect ratio), and return a new GdkTexture owned by the caller.
// Returns nullptr on decode failure.
static GdkTexture* make_scaled_texture(const std::vector<uint8_t>& data,
                                        int max_w, int max_h) {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    gdk_pixbuf_loader_write(loader,
        reinterpret_cast<const guchar*>(data.data()),
        static_cast<gsize>(data.size()), &err);
    if (err) { g_error_free(err); g_object_unref(loader); return nullptr; }
    gdk_pixbuf_loader_close(loader, nullptr);
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pb) { g_object_unref(loader); return nullptr; }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    GdkTexture* tex = nullptr;
    if (w > max_w || h > max_h) {
        double scale = std::min(static_cast<double>(max_w) / w,
                                static_cast<double>(max_h) / h);
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
            pb, static_cast<int>(w * scale), static_cast<int>(h * scale),
            GDK_INTERP_BILINEAR);
        tex = gdk_texture_new_for_pixbuf(scaled);
        g_object_unref(scaled);
    } else {
        tex = gdk_texture_new_for_pixbuf(pb);
    }
    g_object_unref(loader);
    return tex;
}

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

namespace {
struct IdleBackupProgress {
    MainWindow*               window;
    tesseract::BackupProgress progress;
};
} // namespace

void EventHandler::on_backup_progress(const tesseract::BackupProgress& progress) {
    auto* p = new IdleBackupProgress{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        progress
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleBackupProgress*>(data);
        d->window->push_backup_progress(d->progress);
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
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
        .message-body {
            padding: 2px 0px;
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
        .avatar-initial {
            background-color: #8E8E93;
            color: white;
            font-weight: bold;
            font-size: 16px;
            border-radius: 16px;
            min-width: 32px;
            min-height: 32px;
            padding: 0;
        }
        .unread-badge {
            background-color: #0084FF;
            color: white;
            border-radius: 10px;
            padding: 0px 6px;
            font-size: 11px;
            font-weight: bold;
        }
        .room-header {
            background-color: white;
            border-bottom: 1px solid #D0D3D8;
        }
        .room-header-name {
            font-weight: bold;
            font-size: 15px;
        }
        .room-header-topic {
            font-size: 12px;
            color: #65676B;
        }
    )css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // ---- Layout ----
    // Top-level stack swaps between the inline login view and the main UI.
    content_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(
        GTK_STACK(content_stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_window_set_child(GTK_WINDOW(window_), content_stack_);

    login_view_ = std::make_unique<LoginView>(client_);
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    gtk_stack_add_named(GTK_STACK(content_stack_),
                        login_view_->widget(), "login");

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    main_content_ = hbox;
    gtk_stack_add_named(GTK_STACK(content_stack_), hbox, "main");

    // Sidebar (nav bar + room list, wrapped in a vertical box)
    GtkWidget* side_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(side_vbox, 260, -1);
    gtk_widget_add_css_class(side_vbox, "sidebar");
    gtk_box_append(GTK_BOX(hbox), side_vbox);

    // Nav bar (hidden at root, shown when inside a space)
    room_nav_bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(room_nav_bar_, 4);
    gtk_widget_set_margin_end(room_nav_bar_, 4);
    gtk_widget_set_margin_top(room_nav_bar_, 4);
    gtk_widget_set_margin_bottom(room_nav_bar_, 4);
    back_button_ = gtk_button_new_with_label("←");
    space_name_lbl_ = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(space_name_lbl_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(space_name_lbl_, TRUE);
    gtk_box_append(GTK_BOX(room_nav_bar_), back_button_);
    gtk_box_append(GTK_BOX(room_nav_bar_), space_name_lbl_);
    gtk_widget_set_visible(room_nav_bar_, FALSE);
    g_signal_connect(back_button_, "clicked", G_CALLBACK(on_back_clicked_), this);
    gtk_box_append(GTK_BOX(side_vbox), room_nav_bar_);

    GtkWidget* room_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(room_scroll, TRUE);
    room_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(room_list_), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(room_scroll), room_list_);
    gtk_box_append(GTK_BOX(side_vbox), room_scroll);
    g_signal_connect(room_list_, "row-activated",
                     G_CALLBACK(on_room_row_activated), this);

    // ---- User identity strip (sidebar footer) ----
    user_strip_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(user_strip_, "user-strip");
    gtk_widget_set_margin_start(user_strip_, 8);
    gtk_widget_set_margin_end(user_strip_,   8);
    gtk_widget_set_margin_top(user_strip_,   6);
    gtk_widget_set_margin_bottom(user_strip_, 6);
    gtk_widget_set_visible(user_strip_, FALSE);
    {
        user_avatar_img_ = gtk_image_new();
        gtk_widget_set_size_request(user_avatar_img_, 32, 32);
        gtk_image_set_pixel_size(GTK_IMAGE(user_avatar_img_), 32);
        gtk_box_append(GTK_BOX(user_strip_), user_avatar_img_);

        user_name_lbl_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(user_name_lbl_), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(user_name_lbl_), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(user_name_lbl_, TRUE);
        gtk_box_append(GTK_BOX(user_strip_), user_name_lbl_);

        // Right-click gesture → popover menu with single "Logout" item.
        GtkGesture* gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(on_user_strip_right_click_), this);
        gtk_widget_add_controller(user_strip_, GTK_EVENT_CONTROLLER(gesture));

        // Build the GMenu model + GSimpleActionGroup once.
        GMenu* menu = g_menu_new();
        g_menu_append(menu, "Logout", "user.logout");
        user_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        gtk_widget_set_parent(user_popover_, user_strip_);
        gtk_popover_set_has_arrow(GTK_POPOVER(user_popover_), FALSE);
        g_object_unref(menu);

        GSimpleActionGroup* group = g_simple_action_group_new();
        GSimpleAction* act = g_simple_action_new("logout", nullptr);
        g_signal_connect(act, "activate", G_CALLBACK(on_logout_activate_), this);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
        g_object_unref(act);
        gtk_widget_insert_action_group(user_strip_, "user", G_ACTION_GROUP(group));
        g_object_unref(group);
    }
    gtk_box_append(GTK_BOX(side_vbox), user_strip_);

    // Chat area
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    // Room header bar
    room_header_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(room_header_, "room-header");
    gtk_widget_set_margin_start(room_header_, 16);
    gtk_widget_set_margin_end(room_header_, 16);
    gtk_widget_set_margin_top(room_header_, 10);
    gtk_widget_set_margin_bottom(room_header_, 10);
    gtk_widget_set_visible(room_header_, FALSE);

    room_header_avatar_ = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(room_header_avatar_), 40);
    gtk_widget_set_valign(room_header_avatar_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(room_header_), room_header_avatar_);

    GtkWidget* name_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(name_box, TRUE);

    room_header_name_ = gtk_label_new("");
    gtk_widget_add_css_class(room_header_name_, "room-header-name");
    gtk_label_set_xalign(GTK_LABEL(room_header_name_), 0.0f);
    gtk_box_append(GTK_BOX(name_box), room_header_name_);

    room_header_topic_ = gtk_label_new("");
    gtk_widget_add_css_class(room_header_topic_, "room-header-topic");
    gtk_label_set_xalign(GTK_LABEL(room_header_topic_), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(room_header_topic_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_visible(room_header_topic_, FALSE);
    gtk_box_append(GTK_BOX(name_box), room_header_topic_);

    gtk_box_append(GTK_BOX(room_header_), name_box);
    gtk_box_append(GTK_BOX(vbox), room_header_);

    // Recovery banner (Step 6) — hidden until needs_recovery() is true.
    // Inline recovery: the key-entry field + Verify button live in the banner
    // itself; no modal dialog.
    recovery_banner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(recovery_banner_, "recovery-banner");
    gtk_widget_set_margin_start(recovery_banner_,  12);
    gtk_widget_set_margin_end(recovery_banner_,    6);
    gtk_widget_set_margin_top(recovery_banner_,    6);
    gtk_widget_set_margin_bottom(recovery_banner_, 6);
    gtk_widget_set_visible(recovery_banner_, FALSE);
    {
        recovery_label_ = gtk_label_new("Verify this device:");
        gtk_label_set_xalign(GTK_LABEL(recovery_label_), 0.0f);
        gtk_box_append(GTK_BOX(recovery_banner_), recovery_label_);

        recovery_key_entry_ = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(recovery_key_entry_), FALSE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(recovery_key_entry_),
                                       "Recovery key or passphrase");
        gtk_widget_set_hexpand(recovery_key_entry_, TRUE);
        g_signal_connect(recovery_key_entry_, "activate",
                         G_CALLBACK(on_recovery_verify_clicked_), this);
        gtk_box_append(GTK_BOX(recovery_banner_), recovery_key_entry_);

        recovery_verify_btn_ = gtk_button_new_with_label("Verify");
        gtk_widget_add_css_class(recovery_verify_btn_, "suggested-action");
        g_signal_connect(recovery_verify_btn_, "clicked",
                         G_CALLBACK(on_recovery_verify_clicked_), this);
        gtk_box_append(GTK_BOX(recovery_banner_), recovery_verify_btn_);

        GtkWidget* dismiss_btn = gtk_button_new_with_label("✕");
        gtk_widget_set_size_request(dismiss_btn, 24, 24);
        g_signal_connect(dismiss_btn, "clicked",
                         G_CALLBACK(on_recovery_dismiss_clicked_), this);
        gtk_box_append(GTK_BOX(recovery_banner_), dismiss_btn);
    }
    gtk_box_append(GTK_BOX(vbox), recovery_banner_);

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
    g_signal_connect(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(msg_scroll_)),
        "notify::upper",
        G_CALLBACK(MainWindow::on_adj_upper_changed_), this);
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
    // login_view_ holds a reference to client_ and calls cancel_oauth() +
    // joins its worker on destruction. Tear it down here so client_ is
    // still alive when ~LoginView runs.
    login_view_.reset();
}

// ---------------------------------------------------------------------------

void MainWindow::do_login() {
    std::string status_msg;
    if (auto saved = tesseract::SessionStore::load()) {
        gtk_label_set_text(GTK_LABEL(status_bar_), "Restoring session…");
        auto res = client_.restore_session(*saved);
        if (res) {
            my_user_id_       = client_.get_user_id();
            my_display_name_  = client_.get_display_name();
            my_avatar_url_    = client_.get_avatar_url();
            populate_user_strip();
            event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
            client_.start_sync(event_handler_.get());
            gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            maybe_show_recovery_banner();
            return;
        }
        tesseract::SessionStore::clear();
        status_msg = "Saved session expired: " + res.message;
    }

    login_view_->reset();
    login_view_->set_status_message(status_msg);
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    gtk_label_set_text(GTK_LABEL(status_bar_), "Not logged in");
}

void MainWindow::on_login_succeeded() {
    my_user_id_       = client_.get_user_id();
    my_display_name_  = client_.get_display_name();
    my_avatar_url_    = client_.get_avatar_url();
    populate_user_strip();
    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
    client_.start_sync(event_handler_.get());
    gtk_label_set_text(GTK_LABEL(status_bar_), "Connected");
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    maybe_show_recovery_banner();
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
    auto* self = static_cast<MainWindow*>(user_data);
    const char* room_id = static_cast<const char*>(
        g_object_get_data(G_OBJECT(row), "room_id"));
    if (!room_id) return;

    gboolean is_space = static_cast<gboolean>(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(row), "is_space")));

    if (is_space) {
        self->space_stack_.push_back(std::string(room_id));
        self->refresh_room_list();
        return;
    }

    const std::string new_id(room_id);
    if (!self->current_room_id_.empty() && self->current_room_id_ != new_id)
        self->client_.unsubscribe_room(self->current_room_id_);

    self->current_room_id_ = new_id;
    for (const auto& r : self->rooms_)
        if (r.id == new_id) { self->update_room_header(r); break; }

    auto res = self->client_.subscribe_room(self->current_room_id_);
    if (res) {
        self->client_.paginate_back(self->current_room_id_, 50);
        self->client_.start_background_backfill();
    }
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
    refresh_room_list();
    if (!current_room_id_.empty())
        for (const auto& r : rooms_)
            if (r.id == current_room_id_) { update_room_header(r); break; }
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
                my_user_id_       = client_.get_user_id();
                my_display_name_  = client_.get_display_name();
                my_avatar_url_    = client_.get_avatar_url();
                populate_user_strip();
                client_.start_sync(event_handler_.get());
                gtk_label_set_text(GTK_LABEL(status_bar_), "Reconnected");
                maybe_show_recovery_banner();
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

void MainWindow::update_room_header(const tesseract::RoomInfo& info) {
    gtk_label_set_text(GTK_LABEL(room_header_name_), info.name.c_str());

    if (!info.topic.empty()) {
        gtk_label_set_text(GTK_LABEL(room_header_topic_), info.topic.c_str());
        gtk_widget_set_tooltip_text(room_header_topic_, info.topic.c_str());
        gtk_widget_set_visible(room_header_topic_, TRUE);
    } else {
        gtk_widget_set_visible(room_header_topic_, FALSE);
    }

    if (!info.avatar_url.empty()) {
        if (avatar_cache_.find(info.avatar_url) == avatar_cache_.end())
            avatar_cache_[info.avatar_url] = client_.fetch_avatar_bytes(info.id);
        auto it = avatar_cache_.find(info.avatar_url);
        if (it != avatar_cache_.end() && !it->second.empty()) {
            GBytes*     gb  = g_bytes_new(it->second.data(), it->second.size());
            GError*     err = nullptr;
            GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
            g_bytes_unref(gb);
            if (tex) {
                gtk_image_set_from_paintable(GTK_IMAGE(room_header_avatar_),
                                             GDK_PAINTABLE(tex));
                g_object_unref(tex);
            } else if (err) {
                g_error_free(err);
            }
        }
    } else {
        gtk_image_clear(GTK_IMAGE(room_header_avatar_));
    }

    gtk_widget_set_visible(room_header_, TRUE);
}

void MainWindow::clear_messages() {
    msg_event_widgets_.clear();
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(msg_box_)) != nullptr)
        gtk_box_remove(GTK_BOX(msg_box_), child);
}

// ---------------------------------------------------------------------------

void MainWindow::show_rooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Remove all existing rows.
    while (GtkWidget* child = gtk_widget_get_first_child(room_list_))
        gtk_list_box_remove(GTK_LIST_BOX(room_list_), child);

    // Sort: regular rooms first, spaces at the bottom.
    std::vector<const tesseract::RoomInfo*> sorted;
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(&r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(&r);

    for (const tesseract::RoomInfo* rp : sorted) {
        const auto& r = *rp;

        // Fetch avatar on first sight.
        if (!r.avatar_url.empty() &&
            avatar_cache_.find(r.avatar_url) == avatar_cache_.end())
        {
            avatar_cache_[r.avatar_url] = client_.fetch_avatar_bytes(r.id);
        }

        // Row content box
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

        // Spaces get a "# " prefix to distinguish them visually.
        std::string display_name = r.is_space ? "# " + r.name : r.name;
        GtkWidget* name_lbl = gtk_label_new(display_name.c_str());
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

        // Wrap in an explicit GtkListBoxRow so we can attach room_id / is_space.
        GtkWidget* lbrow = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lbrow), row_box);
        g_object_set_data_full(G_OBJECT(lbrow), "room_id",
                               g_strdup(r.id.c_str()), g_free);
        g_object_set_data(G_OBJECT(lbrow), "is_space",
                          GINT_TO_POINTER(r.is_space ? 1 : 0));
        gtk_list_box_append(GTK_LIST_BOX(room_list_), lbrow);
    }
}

void MainWindow::refresh_room_list() {
    if (space_stack_.empty()) {
        show_rooms(rooms_);
        gtk_widget_set_visible(room_nav_bar_, FALSE);
    } else {
        const std::string& space_id = space_stack_.back();
        auto child_ids = client_.space_children(space_id);
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
            if (std::find(child_ids.begin(), child_ids.end(), r.id) != child_ids.end())
                filtered.push_back(r);
        show_rooms(filtered);
        for (const auto& r : rooms_)
            if (r.id == space_id) {
                gtk_label_set_text(GTK_LABEL(space_name_lbl_), r.name.c_str());
                break;
            }
        gtk_widget_set_visible(room_nav_bar_, TRUE);
    }
}

void MainWindow::on_back_clicked_(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->space_stack_.empty()) self->space_stack_.pop_back();
    self->refresh_room_list();
}

// ---------------------------------------------------------------------------

void MainWindow::on_adj_upper_changed_(GObject* obj, GParamSpec*, gpointer user_data) {
    MainWindow* self = static_cast<MainWindow*>(user_data);
    if (!self->auto_scroll_pending_) return;
    self->auto_scroll_pending_ = false;
    GtkAdjustment* a = GTK_ADJUSTMENT(obj);
    gtk_adjustment_set_value(a,
        gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
}

// ---------------------------------------------------------------------------

namespace {
struct ReactionClickCtx {
    MainWindow* window;
    std::string room_id;
    std::string event_id;
    std::string key;
};
} // namespace

void MainWindow::on_reaction_clicked_(GtkButton* /*btn*/, gpointer user_data) {
    auto* ctx = static_cast<ReactionClickCtx*>(user_data);
    if (!ctx || !ctx->window) return;
    auto result = ctx->window->client_.send_reaction(
        ctx->room_id, ctx->event_id, ctx->key);
    if (!result.ok && ctx->window->status_bar_) {
        std::string msg = "Failed to toggle reaction: " + result.message;
        gtk_label_set_text(GTK_LABEL(ctx->window->status_bar_), msg.c_str());
    }
}

GtkWidget* MainWindow::build_message_footer(const tesseract::Event& ev,
                                            const std::string& ts_str) {
    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(footer, 2);

    GtkWidget* chip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(chip_box, GTK_ALIGN_START);
    gtk_widget_set_hexpand(chip_box, TRUE);

    for (const auto& r : ev.reactions) {
        GtkWidget* chip = gtk_button_new();
        gtk_widget_add_css_class(chip, "reaction-chip");
        if (r.reacted_by_me)
            gtk_widget_add_css_class(chip, "reaction-mine");

        // For MSC 4027 custom-image reactions, use the cached chip icon
        // when available; otherwise fall back to the shortcode text.
        GtkWidget* chip_content = nullptr;
        if (!r.source_json.empty()) {
            auto it = image_cache_.find(r.source_json);
            if (it != image_cache_.end() && !it->second.empty()) {
                GdkTexture* tex = make_scaled_texture(it->second, 20, 20);
                if (tex) {
                    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                    GtkWidget* img  = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
                    gtk_image_set_pixel_size(GTK_IMAGE(img), 20);
                    gtk_box_append(GTK_BOX(hbox), img);
                    std::string count = std::to_string(r.count);
                    gtk_box_append(GTK_BOX(hbox), gtk_label_new(count.c_str()));
                    g_object_unref(tex);
                    chip_content = hbox;
                }
            }
        }
        if (!chip_content) {
            std::string label = r.key + " " + std::to_string(r.count);
            chip_content = gtk_label_new(label.c_str());
        }
        gtk_button_set_child(GTK_BUTTON(chip), chip_content);

        if (!r.senders.empty()) {
            std::string tip = "Reacted by:";
            for (const auto& s : r.senders)
                tip += "\n  " + s;
            gtk_widget_set_tooltip_text(chip, tip.c_str());
        }

        auto* ctx = new ReactionClickCtx{this, ev.room_id, ev.event_id, r.key};
        g_signal_connect_data(
            chip, "clicked",
            G_CALLBACK(MainWindow::on_reaction_clicked_),
            ctx,
            +[](gpointer data, GClosure*) {
                delete static_cast<ReactionClickCtx*>(data);
            },
            G_CONNECT_DEFAULT);

        gtk_box_append(GTK_BOX(chip_box), chip);
    }

    gtk_box_append(GTK_BOX(footer), chip_box);

    if (!ts_str.empty()) {
        GtkWidget* ts_lbl = gtk_label_new(ts_str.c_str());
        gtk_label_set_xalign(GTK_LABEL(ts_lbl), 1.0f);
        gtk_widget_set_halign(ts_lbl, GTK_ALIGN_END);
        gtk_widget_add_css_class(ts_lbl, "timestamp");
        gtk_box_append(GTK_BOX(footer), ts_lbl);
    }

    return footer;
}

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
            // For image/sticker events, body_lbl is the caption label (may be
            // nullptr if no caption exists) — only update text-bearing labels.
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
                // MSC2530: only update if a caption label was stored.
                const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
                if (!img.filename.empty()) body_text = img.body;
            } else if (ev.type == tesseract::EventType::Sticker) {
                // No text label for stickers.
            } else {
                body_text = ev.body;
            }
            auto* body_lbl = static_cast<GtkWidget*>(
                g_object_get_data(G_OBJECT(row), "body_lbl"));
            if (body_lbl && !body_text.empty())
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

            // Rebuild the reactions+timestamp footer in place. The reaction
            // set changes on every live update, so always swap the footer
            // rather than diff individual chips.
            std::string ts_str_in;
            if (ev.timestamp > 0) {
                time_t t = static_cast<time_t>(ev.timestamp / 1000);
                struct tm tm_info;
                localtime_r(&t, &tm_info);
                char buf[6];
                strftime(buf, sizeof(buf), "%H:%M", &tm_info);
                ts_str_in = buf;
            }
            // Pre-fetch MSC 4027 chip icons so they render with the image.
            for (const auto& r : ev.reactions) {
                if (!r.source_json.empty() &&
                    image_cache_.find(r.source_json) == image_cache_.end())
                {
                    image_cache_[r.source_json] =
                        client_.fetch_media_bytes(r.source_json);
                }
            }
            GtkWidget* old_footer = static_cast<GtkWidget*>(
                g_object_get_data(G_OBJECT(row), "footer"));
            GtkWidget* bubble = static_cast<GtkWidget*>(
                g_object_get_data(G_OBJECT(row), "bubble"));
            if (old_footer && bubble) {
                gtk_box_remove(GTK_BOX(bubble), old_footer);
            }
            if (bubble) {
                GtkWidget* footer = build_message_footer(ev, ts_str_in);
                gtk_box_append(GTK_BOX(bubble), footer);
                g_object_set_data(G_OBJECT(row), "footer", footer);
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

    // Fetch/cache image bytes for image and sticker events.
    auto fetch_if_missing = [&](const std::string& url) {
        if (!url.empty() && image_cache_.find(url) == image_cache_.end())
            image_cache_[url] = client_.fetch_media_bytes(url);
    };
    if (ev.type == tesseract::EventType::Image)
        fetch_if_missing(static_cast<const tesseract::ImageEvent&>(ev).image_url);
    else if (ev.type == tesseract::EventType::Sticker)
        fetch_if_missing(static_cast<const tesseract::StickerEvent&>(ev).image_url);

    // Pre-fetch MSC 4027 chip icons so they can render with the image
    // instead of falling back to the shortcode label.
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            fetch_if_missing(r.source_json);
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
        // Initials disc fallback (matches Qt's makeInitialsPixmap style:
        // grey #8E8E93 circle, bold white capital letter).
        std::string initial = "?";
        if (!name.empty()) {
            unsigned char c = static_cast<unsigned char>(name[0]);
            if (c < 0x80) {
                initial = std::string(1, static_cast<char>(std::toupper(c)));
            } else {
                // Keep the whole first UTF-8 codepoint intact.
                int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                initial = name.substr(0, std::min<size_t>(len, name.size()));
            }
        }
        av_widget = gtk_label_new(initial.c_str());
        gtk_widget_add_css_class(av_widget, "avatar-initial");
        gtk_widget_set_size_request(av_widget, kMsgAvatarSize, kMsgAvatarSize);
        gtk_widget_set_valign(av_widget, GTK_ALIGN_START);
    }
    gtk_box_append(GTK_BOX(row), av_widget);
    g_object_set_data(G_OBJECT(row), "avatar", av_widget);

    // ---- Content vbox (name + bubble) ----
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);

    GtkWidget* name_lbl = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_widget_add_css_class(name_lbl, "sender-name");
    gtk_box_append(GTK_BOX(content), name_lbl);
    g_object_set_data(G_OBJECT(row), "name_lbl", name_lbl);

    // ---- Message content ----
    GtkWidget* bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    // Stickers are borderless; all other types get the chat-bubble style.
    if (ev.type != tesseract::EventType::Sticker)
        gtk_widget_add_css_class(bubble, "message-body");
    gtk_widget_set_halign(bubble, GTK_ALIGN_START);

    // Build bubble content depending on event type.
    GtkWidget* body_lbl = nullptr;
    if (ev.type == tesseract::EventType::File) {
        const auto& file = static_cast<const tesseract::FileEvent&>(ev);
        std::string body_text = "📎 " + file.file_name;
        if (file.file_size > 0) {
            double kb = file.file_size / 1024.0;
            char size_buf[32];
            if (kb < 1024)
                snprintf(size_buf, sizeof(size_buf), " (%.1f KB)", kb);
            else
                snprintf(size_buf, sizeof(size_buf), " (%.1f MB)", kb / 1024.0);
            body_text += size_buf;
        }
        body_lbl = gtk_label_new(body_text.c_str());
        gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(body_lbl), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(body_lbl), 0.0f);
        gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 55);
        gtk_box_append(GTK_BOX(bubble), body_lbl);
    } else if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        auto it = image_cache_.find(img.image_url);
        if (it != image_cache_.end() && !it->second.empty()) {
            GdkTexture* tex = make_scaled_texture(it->second,
                                                   320, 200);
            if (tex) {
                GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
                gtk_widget_set_halign(picture, GTK_ALIGN_START);
                gtk_box_append(GTK_BOX(bubble), picture);
                g_object_unref(tex);
            }
        }
        // MSC2530: show body as caption only when a distinct filename was supplied.
        if (!img.filename.empty() && !img.body.empty()) {
            body_lbl = gtk_label_new(img.body.c_str());
            gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(body_lbl), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_xalign(GTK_LABEL(body_lbl), 0.0f);
            gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 55);
            gtk_box_append(GTK_BOX(bubble), body_lbl);
        }
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        auto it = image_cache_.find(s.image_url);
        if (it != image_cache_.end() && !it->second.empty()) {
            GdkTexture* tex = make_scaled_texture(it->second, 256, 256);
            if (tex) {
                GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
                gtk_widget_set_halign(picture, GTK_ALIGN_START);
                gtk_box_append(GTK_BOX(bubble), picture);
                g_object_unref(tex);
            }
        }
        // Sticker body is alt-text only; never displayed.
    } else {
        body_lbl = gtk_label_new(ev.body.c_str());
        gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(body_lbl), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(body_lbl), 0.0f);
        gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 55);
        gtk_box_append(GTK_BOX(bubble), body_lbl);
    }
    g_object_set_data(G_OBJECT(row), "body_lbl", body_lbl); // may be nullptr for images/stickers

    // Footer: reaction chips (left) + right-anchored timestamp.
    GtkWidget* footer = build_message_footer(ev, ts_str);
    gtk_box_append(GTK_BOX(bubble), footer);
    g_object_set_data(G_OBJECT(row), "footer", footer);
    g_object_set_data(G_OBJECT(row), "bubble", bubble);

    gtk_box_append(GTK_BOX(content), bubble);
    gtk_widget_set_halign(content, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(row), content);
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);

    if (!ev.event_id.empty())
        msg_event_widgets_[ev.event_id] = row;
    auto_scroll_pending_ = true;
    gtk_box_append(GTK_BOX(msg_box_), row);
    gtk_widget_set_visible(row, TRUE);
}

// ---------------------------------------------------------------------------
// Recovery banner (Step 6) — inline key entry, no modal dialog.
// ---------------------------------------------------------------------------

namespace {
struct RecoverDone {
    MainWindow* window;
    bool        ok;
    std::string message;
};
} // namespace

void MainWindow::maybe_show_recovery_banner() {
    if (recovery_banner_dismissed_) return;
    if (!client_.needs_recovery()) return;
    if (!gtk_widget_get_visible(recovery_banner_)) {
        // Fresh prompt — restore the input row.
        gtk_label_set_text(GTK_LABEL(recovery_label_), "Verify this device:");
        gtk_editable_set_text(GTK_EDITABLE(recovery_key_entry_), "");
        gtk_widget_set_visible(recovery_key_entry_, TRUE);
        gtk_widget_set_sensitive(recovery_key_entry_, TRUE);
        gtk_widget_set_visible(recovery_verify_btn_, TRUE);
        gtk_widget_set_sensitive(recovery_verify_btn_, TRUE);
        gtk_widget_set_visible(recovery_banner_, TRUE);
    }
}

void MainWindow::on_recovery_verify_clicked_(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    const char* key_c = gtk_editable_get_text(GTK_EDITABLE(self->recovery_key_entry_));
    std::string key   = key_c ? key_c : "";
    if (key.empty()) {
        gtk_label_set_text(GTK_LABEL(self->recovery_label_),
                           "Please enter a recovery key or passphrase.");
        return;
    }
    gtk_widget_set_sensitive(self->recovery_key_entry_, FALSE);
    gtk_widget_set_sensitive(self->recovery_verify_btn_, FALSE);
    gtk_widget_set_visible(self->recovery_key_entry_, FALSE);
    gtk_widget_set_visible(self->recovery_verify_btn_, FALSE);
    gtk_label_set_text(GTK_LABEL(self->recovery_label_), "Verifying…");

    // Worker thread; marshal result back via g_idle_add.
    std::thread([self, key]() {
        auto res = self->client_.recover(key);
        auto* p  = new RecoverDone{ self, res.ok, res.message };
        g_idle_add([](gpointer data) -> gboolean {
            auto* d = static_cast<RecoverDone*>(data);
            if (d->ok) {
                // Backup watcher will repaint into "Importing keys…" and hide
                // the banner once state reaches Enabled.
                gtk_label_set_text(GTK_LABEL(d->window->recovery_label_),
                                   "Downloading historical keys…");
            } else {
                std::string txt = "Recovery failed: " + d->message;
                gtk_label_set_text(GTK_LABEL(d->window->recovery_label_), txt.c_str());
                gtk_widget_set_visible(d->window->recovery_key_entry_,  TRUE);
                gtk_widget_set_sensitive(d->window->recovery_key_entry_, TRUE);
                gtk_widget_grab_focus(d->window->recovery_key_entry_);
                gtk_widget_set_visible(d->window->recovery_verify_btn_,  TRUE);
                gtk_widget_set_sensitive(d->window->recovery_verify_btn_, TRUE);
            }
            delete d;
            return G_SOURCE_REMOVE;
        }, p);
    }).detach();
}

void MainWindow::on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->recovery_banner_dismissed_ = true;
    gtk_widget_set_visible(self->recovery_banner_, FALSE);
}

void MainWindow::push_backup_progress(tesseract::BackupProgress progress) {
    // Recovery state is populated asynchronously by the first sync cycle, so
    // re-evaluate the banner each time the SDK pings us.
    maybe_show_recovery_banner();

    // Live progress only when the input field is hidden (recovery in flight
    // or finished), so we don't clobber "Verify this device:" while the user
    // is typing.
    if (gtk_widget_get_visible(recovery_banner_)
        && !gtk_widget_get_visible(recovery_key_entry_)
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        std::string txt = "Importing keys from backup… "
            + std::to_string(progress.imported_keys) + " imported.";
        gtk_label_set_text(GTK_LABEL(recovery_label_), txt.c_str());
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !client_.needs_recovery())
    {
        gtk_widget_set_visible(recovery_banner_, FALSE);
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populate_user_strip() {
    std::string shown = my_display_name_.empty() ? my_user_id_ : my_display_name_;
    gtk_label_set_text(GTK_LABEL(user_name_lbl_), shown.c_str());

    bool has_avatar = false;
    if (!my_avatar_url_.empty()) {
        auto bytes = client_.fetch_media_bytes(my_avatar_url_);
        if (!bytes.empty()) {
            GdkTexture* tex = make_scaled_texture(bytes, 32, 32);
            if (tex) {
                gtk_image_set_from_paintable(GTK_IMAGE(user_avatar_img_),
                                             GDK_PAINTABLE(tex));
                g_object_unref(tex);
                has_avatar = true;
            }
        }
    }
    if (!has_avatar) {
        // Fallback: GTK's "avatar-default-symbolic" if available, otherwise a
        // generic person icon.
        gtk_image_set_from_icon_name(GTK_IMAGE(user_avatar_img_),
                                     "avatar-default-symbolic");
    }
    gtk_widget_set_visible(user_strip_, TRUE);
}

void MainWindow::on_user_strip_right_click_(GtkGestureClick* gesture,
                                            int /*n_press*/,
                                            double x, double y,
                                            gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    GdkRectangle r = { static_cast<int>(x), static_cast<int>(y), 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->user_popover_), &r);
    gtk_popover_popup(GTK_POPOVER(self->user_popover_));
}

void MainWindow::on_logout_activate_(GSimpleAction* /*action*/,
                                     GVariant* /*parameter*/,
                                     gpointer user_data) {
    static_cast<MainWindow*>(user_data)->do_logout();
}

void MainWindow::do_logout() {
    gtk_popover_popdown(GTK_POPOVER(user_popover_));

    auto res = client_.logout();
    tesseract::SessionStore::clear();
    client_.stop_sync();
    event_handler_.reset();

    // Reset visible state.
    if (!current_room_id_.empty())
        client_.unsubscribe_room(current_room_id_);
    current_room_id_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    rooms_.clear();
    refresh_room_list();
    clear_messages();
    gtk_widget_set_visible(user_strip_, FALSE);
    gtk_widget_set_visible(recovery_banner_, FALSE);
    recovery_banner_dismissed_ = false;
    gtk_widget_set_visible(room_header_, FALSE);

    gtk_label_set_text(GTK_LABEL(status_bar_),
                       res ? "Signed out"
                           : ("Sign out failed: " + res.message).c_str());

    login_view_->reset();
    login_view_->set_status_message("");
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

} // namespace gtk4
