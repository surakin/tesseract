#include "MainWindow.h"
#include "LoginView.h"

#include "tk/canvas_cairo.h"
#include "tk/theme.h"

#include <cairo.h>
#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

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

struct IdlePrepend {
    MainWindow*                        window;
    std::unique_ptr<tesseract::Event> ev;
};

struct IdlePaginateResult {
    MainWindow* window;
    std::string room_id;
    bool        reached_start;
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

void EventHandler::on_message_prepended(tesseract::Event* ev) {
    auto* p = new IdlePrepend{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        std::unique_ptr<tesseract::Event>(ev)
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdlePrepend*>(data);
        d->window->push_prepended_event(std::move(d->ev));
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
        }
        .sidebar-separator {
            background-color: #D0D3D8;
            min-width: 1px;
        }
        .message-body {
            padding: 2px 0px;
        }
        .sender-name {
            font-weight: bold;
            font-size: 11px;
            color: #555555;
        }
        .timestamp {
            font-size: 9px;
            color: rgba(0,0,0,0.45);
        }
        .avatar-initial {
            background-color: #8E8E93;
            color: white;
            font-weight: bold;
            font-size: 15px;
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
            font-size: 10px;
            font-weight: bold;
        }
        .room-header {
            background-color: white;
            border-bottom: 1px solid #D0D3D8;
        }
        .room-header-name {
            font-weight: bold;
            font-size: 14px;
        }
        .room-header-topic {
            font-size: 11px;
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

    // Shared-toolkit room list. The Surface hosts a tk::Widget tree
    // painted via Cairo + Pango; RoomListView inside owns the actual
    // layout, selection state, and unread-badge rendering.
    room_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    auto room_view_owner = std::make_unique<tesseract::views::RoomListView>();
    room_list_view_ = room_view_owner.get();
    room_list_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = tk_avatars_.find(mxc);
            return it == tk_avatars_.end() ? nullptr : it->second.get();
        });
    room_list_view_->on_room_selected =
        [this](const std::string& room_id) { on_room_selected(room_id); };
    room_surface_->set_root(std::move(room_view_owner));

    GtkWidget* room_surface_widget = room_surface_->widget();
    gtk_widget_set_vexpand(room_surface_widget, TRUE);
    gtk_widget_set_hexpand(room_surface_widget, TRUE);
    gtk_box_append(GTK_BOX(side_vbox), room_surface_widget);

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

    // 1px vertical separator between sidebar and chat area. A dedicated
    // widget is used instead of the sidebar's CSS `border-right`, because
    // the scrolled-window's own background paints over the parent border.
    GtkWidget* side_separator =
        gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(side_separator, "sidebar-separator");
    gtk_box_append(GTK_BOX(hbox), side_separator);

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

    // Recovery banner — shared widget on a tk::gtk4::Surface. Hidden
    // until needs_recovery() is true; surfaced via maybe_show_recovery_banner.
    recovery_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::RecoveryBanner>();
        recovery_shared_ = banner.get();
        recovery_shared_->on_verify = [this](const std::string& /*key*/) {
            on_recovery_verify_clicked_(nullptr, this);
        };
        recovery_shared_->on_dismiss = [this] {
            on_recovery_dismiss_clicked_(nullptr, this);
        };
        recovery_surface_->set_root(std::move(banner));

        recovery_key_field_ = recovery_surface_->host().make_text_field();
        recovery_key_field_->set_placeholder("Recovery key or passphrase");
        recovery_key_field_->set_password(true);
        recovery_key_field_->set_on_changed([this](const std::string& k) {
            if (recovery_shared_) recovery_shared_->set_current_key(k);
        });
        recovery_key_field_->set_on_submit([this] {
            on_recovery_verify_clicked_(nullptr, this);
        });
        recovery_surface_->set_on_layout([this] {
            if (!recovery_shared_ || !recovery_key_field_) return;
            recovery_key_field_->set_visible(
                recovery_shared_->recovery_key_field_visible());
            recovery_key_field_->set_rect(
                recovery_shared_->recovery_key_field_rect());
        });
    }
    GtkWidget* recovery_widget = recovery_surface_->widget();
    gtk_widget_set_size_request(recovery_widget, -1, 48);
    gtk_widget_set_visible(recovery_widget, FALSE);
    gtk_box_append(GTK_BOX(vbox), recovery_widget);

    // Shared-toolkit message list. Each row is paint-only (no per-row
    // GtkWidget); the Surface owns scrolling, hit-testing and virtual-
    // isation, and the sticky-bottom auto-scroll behaviour is built
    // into MessageListView::append_message.
    msg_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    auto msg_view_owner = std::make_unique<tesseract::views::MessageListView>();
    message_list_view_ = msg_view_owner.get();
    message_list_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = tk_avatars_.find(mxc);
            return it == tk_avatars_.end() ? nullptr : it->second.get();
        });
    message_list_view_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = tk_images_.find(mxc);
            return it == tk_images_.end() ? nullptr : it->second.get();
        });
    msg_surface_->set_root(std::move(msg_view_owner));

    GtkWidget* msg_surface_widget = msg_surface_->widget();
    gtk_widget_set_vexpand(msg_surface_widget, TRUE);
    gtk_widget_set_hexpand(msg_surface_widget, TRUE);
    gtk_box_append(GTK_BOX(vbox), msg_surface_widget);

    // Compose bar — shared widget on a tk::gtk4::Surface. Text input is
    // a NativeTextArea overlay on the bar's text_area_rect; emoji + send
    // buttons paint into the toolkit.
    compose_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    auto compose_owner = std::make_unique<tesseract::views::ComposeBar>();
    compose_shared_ = compose_owner.get();
    compose_surface_->set_root(std::move(compose_owner));

    GtkWidget* compose_surface_widget = compose_surface_->widget();
    gtk_widget_set_size_request(compose_surface_widget, -1,
        static_cast<int>(tesseract::views::ComposeBar::kMinHeight));
    gtk_widget_set_hexpand(compose_surface_widget, TRUE);
    gtk_box_append(GTK_BOX(vbox), compose_surface_widget);

    compose_text_area_ = compose_surface_->host().make_text_area();
    compose_text_area_->set_placeholder("Message…");
    compose_text_area_->set_on_changed([this](const std::string& s) {
        if (compose_shared_) compose_shared_->set_current_text(s);
    });
    compose_text_area_->set_on_submit([this] { on_send_clicked(); });
    compose_text_area_->set_on_height_changed([this](float h) {
        if (!compose_shared_ || !compose_surface_) return;
        compose_shared_->set_text_area_natural_height(h);
        gtk_widget_set_size_request(compose_surface_->widget(), -1,
            static_cast<int>(compose_shared_->natural_height()));
        compose_surface_->relayout();
    });
    compose_text_area_->set_on_image_paste(
        [this](std::vector<std::uint8_t> bytes, std::string mime) {
            if (compose_shared_)
                compose_shared_->set_pending_image(std::move(bytes),
                                                    std::move(mime));
        });

    // Drag-and-drop: dropping an image file on either the message list
    // or the composer parks it in the same pending-image slot the paste
    // path uses.
    auto on_image_drop = [this](std::vector<std::uint8_t> bytes,
                                std::string mime,
                                std::string filename) {
        if (compose_shared_)
            compose_shared_->set_pending_image(std::move(bytes),
                                               std::move(mime),
                                               std::move(filename));
    };
    compose_surface_->set_on_image_drop(on_image_drop);
    if (msg_surface_) msg_surface_->set_on_image_drop(on_image_drop);
    compose_surface_->set_on_layout([this] {
        if (compose_shared_ && compose_text_area_)
            compose_text_area_->set_rect(compose_shared_->text_area_rect());
    });

    compose_shared_->on_send  = [this](const std::string&) { on_send_clicked(); };
    compose_shared_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                              std::string mime,
                                              std::string filename,
                                              std::string caption,
                                              std::uint32_t /*src_w*/,
                                              std::uint32_t /*src_h*/) {
        if (current_room_id_.empty()) return;
        const bool compress =
            tesseract::Settings::instance().image_quality
            == tesseract::Settings::ImageQuality::Compressed;
        auto enc = compose_surface_->host().encode_for_send(
            bytes.data(), bytes.size(), compress);
        if (enc.bytes.empty()) return;
        std::string out_name = filename;
        if (enc.mime == "image/jpeg") {
            auto dot = out_name.find_last_of('.');
            if (dot != std::string::npos) out_name = out_name.substr(0, dot);
            out_name += ".jpg";
        }
        auto res = client_.send_image(current_room_id_, enc.bytes, enc.mime,
                                        out_name, caption,
                                        enc.width, enc.height);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        }
    };
    compose_shared_->on_size_changed = [this] {
        if (!compose_shared_ || !compose_surface_) return;
        gtk_widget_set_size_request(compose_surface_->widget(), -1,
            static_cast<int>(compose_shared_->natural_height()));
        compose_surface_->relayout();
    };
    compose_shared_->on_emoji = [this] { toggle_emoji_picker(); };

    message_list_view_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            if (current_room_id_.empty()) return;
            client_.send_reaction(current_room_id_, event_id, key);
        };
    message_list_view_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor) {
            if (!emoji_popover_ || current_room_id_.empty()) return;
            pending_reaction_event_id_ = event_id;
            // anchor is in MessageListView-local coords; the view is the
            // root of msg_surface_, so the rect maps directly to its
            // widget coords.
            popup_emoji_at_rect(msg_surface_->widget(), anchor);
        };

    // Back-pagination on scroll-to-top. The shared MessageListView fires
    // this once per crossing of the near-top threshold.
    message_list_view_->on_near_top = [this]{
        if (current_room_id_.empty()) return;
        request_more_history(current_room_id_);
    };

    // Lazily build the picker — the popover is parented to the compose
    // surface widget. Recents live in account-data now
    // (io.element.recent_emoji); no local-disk load.
    build_emoji_popover();

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

void MainWindow::on_send_clicked() {
    if (current_room_id_.empty() || !compose_text_area_) return;

    std::string body = compose_text_area_->text();

    // Trim leading/trailing whitespace.
    auto l = body.find_first_not_of(" \t\n\r");
    auto r = body.find_last_not_of(" \t\n\r");
    if (l == std::string::npos) return;
    body = body.substr(l, r - l + 1);
    if (body.empty()) return;

    auto res = client_.send_message(current_room_id_, body);
    if (res) {
        compose_text_area_->set_text("");
        if (compose_shared_) compose_shared_->set_current_text({});
    }
}

void MainWindow::on_room_selected(const std::string& room_id) {
    if (room_id.empty()) return;

    // Drill into a space if the clicked row is one.
    for (const auto& r : rooms_) {
        if (r.id == room_id && r.is_space) {
            space_stack_.push_back(room_id);
            refresh_room_list();
            return;
        }
    }

    if (!current_room_id_.empty() && current_room_id_ != room_id)
        client_.unsubscribe_room(current_room_id_);

    current_room_id_ = room_id;
    for (const auto& r : rooms_)
        if (r.id == current_room_id_) { update_room_header(r); break; }

    auto res = client_.subscribe_room(current_room_id_);
    if (res) {
        auto& state = pagination_[current_room_id_];
        state.in_flight = false;
        auto pr = client_.paginate_back_with_status(current_room_id_,
                                                     kPaginationBatch);
        state.reached_start = pr.ok && pr.reached_start;
        client_.start_background_backfill();
    }
}

void MainWindow::push_prepended_event(std::unique_ptr<tesseract::Event> ev) {
    if (ev->room_id == current_room_id_)
        prepend_event(*ev);
}

void MainWindow::push_paginate_result(std::string room_id, bool reached_start) {
    auto it = pagination_.find(room_id);
    if (it == pagination_.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached_start;
    if (room_id == current_room_id_ && message_list_view_)
        message_list_view_->reset_near_top_latch();
}

void MainWindow::request_more_history(const std::string& room_id) {
    if (room_id.empty()) return;
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    // Worker thread: invoke the blocking SDK call, marshal the result
    // back via g_idle_add on the main loop.
    std::thread([this, room_id]{
        auto pr = client_.paginate_back_with_status(room_id, kPaginationBatch);
        auto* p = new IdlePaginateResult{
            this, room_id, pr.ok && pr.reached_start
        };
        g_idle_add([](gpointer data) -> gboolean {
            auto* d = static_cast<IdlePaginateResult*>(data);
            d->window->push_paginate_result(std::move(d->room_id),
                                              d->reached_start);
            delete d;
            return G_SOURCE_REMOVE;
        }, p);
    }).detach();
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
    message_list_view_->set_messages({});
    msg_surface_->relayout();
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

namespace {

// Decode raw image bytes to a premultiplied-ARGB32 cairo_surface_t the
// shared CairoImage wrapper expects. Reuses GdkPixbufLoader so the
// existing matrix-sdk attachments path (PNG/JPEG/WebP/AVIF) decodes
// identically to the legacy GTK rendering.
cairo_surface_t* decode_image_to_cairo_surface(
    const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return nullptr;
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err)) {
        if (err) g_error_free(err);
        g_object_unref(loader);
        return nullptr;
    }
    if (!gdk_pixbuf_loader_close(loader, &err)) {
        if (err) g_error_free(err);
        g_object_unref(loader);
        return nullptr;
    }
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pb) { g_object_unref(loader); return nullptr; }

    int  w        = gdk_pixbuf_get_width (pb);
    int  h        = gdk_pixbuf_get_height(pb);
    int  channels = gdk_pixbuf_get_n_channels(pb);
    int  in_stride = gdk_pixbuf_get_rowstride(pb);
    const guchar* pixels = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        g_object_unref(loader);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char* dst = cairo_image_surface_get_data(surface);
    int out_stride     = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < h; ++y) {
        const guchar*  src_row = pixels + y * in_stride;
        unsigned char* dst_row = dst    + y * out_stride;
        for (int x = 0; x < w; ++x) {
            guchar r = src_row[x * channels + 0];
            guchar g = src_row[x * channels + 1];
            guchar b = src_row[x * channels + 2];
            guchar a = channels == 4 ? src_row[x * channels + 3] : 255;
            unsigned r_p = (r * a + 127) / 255;
            unsigned g_p = (g * a + 127) / 255;
            unsigned b_p = (b * a + 127) / 255;
            dst_row[x * 4 + 0] = static_cast<unsigned char>(b_p);
            dst_row[x * 4 + 1] = static_cast<unsigned char>(g_p);
            dst_row[x * 4 + 2] = static_cast<unsigned char>(r_p);
            dst_row[x * 4 + 3] = a;
        }
    }
    cairo_surface_mark_dirty(surface);
    g_object_unref(loader);
    return surface;
}

} // namespace

void MainWindow::ensure_room_avatar(const tesseract::RoomInfo& r) {
    if (r.avatar_url.empty() || tk_avatars_.count(r.avatar_url)) return;
    auto bytes = client_.fetch_avatar_bytes(r.id);
    if (bytes.empty()) return;
    cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
    if (!surface) return;
    auto img = tk::cairo_pango::make_image(surface);
    cairo_surface_destroy(surface);   // make_image took its own ref
    tk_avatars_.emplace(r.avatar_url, std::move(img));
}

void MainWindow::ensure_user_avatar(const std::string& mxc) {
    if (mxc.empty() || tk_avatars_.count(mxc)) return;
    auto bytes = client_.fetch_media_bytes(mxc);
    if (bytes.empty()) return;
    cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
    if (!surface) return;
    auto img = tk::cairo_pango::make_image(surface);
    cairo_surface_destroy(surface);
    tk_avatars_.emplace(mxc, std::move(img));
}

void MainWindow::ensure_media_image(const std::string& url,
                                      int /*max_w*/, int /*max_h*/) {
    if (url.empty() || tk_images_.count(url)) return;
    auto bytes = client_.fetch_media_bytes(url);
    if (bytes.empty()) return;
    cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
    if (!surface) return;
    auto img = tk::cairo_pango::make_image(surface);
    cairo_surface_destroy(surface);
    tk_images_.emplace(url, std::move(img));
}

// ---------------------------------------------------------------------------

void MainWindow::show_rooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(r);

    // Eagerly fetch avatars for the new room set so the first paint has
    // them ready. Bytes-already-cached is a no-op via tk_avatars_.count.
    for (const auto& r : sorted) ensure_room_avatar(r);

    room_list_view_->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
        room_list_view_->set_selected_room(current_room_id_);
    room_surface_->relayout();
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
//  Event → MessageRowData + append into the shared MessageListView
// ---------------------------------------------------------------------------

tesseract::views::MessageRowData MainWindow::to_row_data(
    const tesseract::Event& ev)
{
    using Kind = tesseract::views::MessageRowData::Kind;
    tesseract::views::MessageRowData row;
    row.event_id          = ev.event_id;
    row.sender            = ev.sender;
    row.sender_name       = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body              = ev.body;
    row.timestamp_ms      = ev.timestamp;
    row.is_own            = (ev.sender == my_user_id_);
    row.reactions         = ev.reactions;

    switch (ev.type) {
        case tesseract::EventType::Text:    row.kind = Kind::Text;    break;
        case tesseract::EventType::Image: {
            row.kind = Kind::Image;
            const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
            row.media_url            = img.image_url;
            row.media_w              = static_cast<int>(img.width);
            row.media_h              = static_cast<int>(img.height);
            row.has_filename_caption = !img.filename.empty();
            break;
        }
        case tesseract::EventType::Sticker: {
            row.kind = Kind::Sticker;
            const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
            row.media_url = s.image_url;
            row.media_w   = static_cast<int>(s.width);
            row.media_h   = static_cast<int>(s.height);
            break;
        }
        case tesseract::EventType::File: {
            row.kind = Kind::File;
            const auto& f = static_cast<const tesseract::FileEvent&>(ev);
            row.file_name = f.file_name;
            row.file_size = f.file_size;
            row.media_url = f.file_url;
            break;
        }
        case tesseract::EventType::Redacted:  row.kind = Kind::Redacted;  break;
        case tesseract::EventType::Unhandled: row.kind = Kind::Unhandled; break;
    }
    return row;
}

void MainWindow::append_event(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    // Pre-fetch any media this row will reference. The shared view's
    // provider lambdas look up tk_avatars_ / tk_images_ on each paint.
    ensure_user_avatar(ev.sender_avatar_url);
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        ensure_media_image(img.image_url,
                            tesseract::visual::kMaxInlineImageWidth,
                            tesseract::visual::kMaxInlineImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        ensure_media_image(s.image_url,
                            tesseract::visual::kStickerSize,
                            tesseract::visual::kStickerSize);
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            ensure_media_image(r.source_json, 20, 20);
    }

    auto row = to_row_data(ev);

    // Live re-emit (reactions, edits, sender-profile resolution): replace
    // an existing row with the same event_id rather than appending.
    auto& msgs = const_cast<std::vector<tesseract::views::MessageRowData>&>(
        message_list_view_->messages());
    auto it = std::find_if(msgs.begin(), msgs.end(),
        [&](const tesseract::views::MessageRowData& m) {
            return m.event_id == row.event_id;
        });
    if (it != msgs.end()) {
        *it = std::move(row);
        message_list_view_->invalidate_data();
        msg_surface_->relayout();
        return;
    }
    message_list_view_->append_message(std::move(row));
    msg_surface_->relayout();
}

void MainWindow::prepend_event(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    ensure_user_avatar(ev.sender_avatar_url);
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        ensure_media_image(img.image_url,
                            tesseract::visual::kMaxInlineImageWidth,
                            tesseract::visual::kMaxInlineImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        ensure_media_image(s.image_url,
                            tesseract::visual::kStickerSize,
                            tesseract::visual::kStickerSize);
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            ensure_media_image(r.source_json, 20, 20);
    }

    auto row = to_row_data(ev);

    auto& msgs = const_cast<std::vector<tesseract::views::MessageRowData>&>(
        message_list_view_->messages());
    auto it = std::find_if(msgs.begin(), msgs.end(),
        [&](const tesseract::views::MessageRowData& m) {
            return m.event_id == row.event_id;
        });
    if (it != msgs.end()) {
        *it = std::move(row);
        message_list_view_->invalidate_data();
        msg_surface_->relayout();
        return;
    }
    message_list_view_->prepend_message(std::move(row));
    msg_surface_->relayout();
}

// ---------------------------------------------------------------------------

void MainWindow::maybe_show_recovery_banner() {
    if (recovery_banner_dismissed_) return;
    if (!client_.needs_recovery()) return;
    if (!recovery_surface_) return;
    GtkWidget* w = recovery_surface_->widget();
    if (!gtk_widget_get_visible(w)) {
        if (recovery_shared_) {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Form);
            recovery_shared_->set_current_key("");
        }
        if (recovery_key_field_) {
            recovery_key_field_->set_text("");
            recovery_key_field_->set_enabled(true);
        }
        gtk_widget_set_visible(w, TRUE);
        recovery_surface_->relayout();
    }
}

void MainWindow::on_recovery_verify_clicked_(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    std::string key;
    if (self->recovery_key_field_) key = self->recovery_key_field_->text();
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of (" \t\r\n");
    if (a == std::string::npos) {
        if (self->recovery_shared_) {
            self->recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            self->recovery_shared_->set_failure_message(
                "Please enter a recovery key or passphrase.");
            self->recovery_surface_->relayout();
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (self->recovery_shared_)
        self->recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    if (self->recovery_key_field_) self->recovery_key_field_->set_enabled(false);
    self->recovery_surface_->relayout();

    struct RecoverDone {
        MainWindow* window;
        bool        ok;
        std::string message;
    };
    std::thread([self, key]() {
        auto res = self->client_.recover(key);
        auto* p  = new RecoverDone{ self, res.ok, res.message };
        g_idle_add([](gpointer data) -> gboolean {
            auto* d = static_cast<RecoverDone*>(data);
            if (d->ok) {
                if (d->window->recovery_shared_) {
                    d->window->recovery_shared_->set_state(
                        tesseract::views::RecoveryBanner::State::Importing);
                }
            } else {
                if (d->window->recovery_shared_) {
                    d->window->recovery_shared_->set_state(
                        tesseract::views::RecoveryBanner::State::Failed);
                    d->window->recovery_shared_->set_failure_message(d->message);
                }
                if (d->window->recovery_key_field_) {
                    d->window->recovery_key_field_->set_enabled(true);
                    d->window->recovery_key_field_->set_focused(true);
                }
            }
            if (d->window->recovery_surface_)
                d->window->recovery_surface_->relayout();
            delete d;
            return G_SOURCE_REMOVE;
        }, p);
    }).detach();
}

void MainWindow::on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->recovery_banner_dismissed_ = true;
    if (self->recovery_surface_)
        gtk_widget_set_visible(self->recovery_surface_->widget(), FALSE);
}

void MainWindow::push_backup_progress(tesseract::BackupProgress progress) {
    maybe_show_recovery_banner();

    if (recovery_surface_ && recovery_shared_
        && gtk_widget_get_visible(recovery_surface_->widget())
        && recovery_shared_->state()
            == tesseract::views::RecoveryBanner::State::Importing
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        recovery_shared_->set_import_progress(progress.imported_keys);
        recovery_surface_->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !client_.needs_recovery()
        && recovery_surface_)
    {
        gtk_widget_set_visible(recovery_surface_->widget(), FALSE);
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
    if (recovery_surface_)
        gtk_widget_set_visible(recovery_surface_->widget(), FALSE);
    recovery_banner_dismissed_ = false;
    gtk_widget_set_visible(room_header_, FALSE);

    gtk_label_set_text(GTK_LABEL(status_bar_),
                       res ? "Signed out"
                           : ("Sign out failed: " + res.message).c_str());

    login_view_->reset();
    login_view_->set_status_message("");
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

// ---------------------------------------------------------------------------
// Emoji picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::EmojiPicker. The search row is a native
// GtkEntry overlaid by the Surface; selection routes back through the
// shared widget's on_selected callback.
// ---------------------------------------------------------------------------

void MainWindow::build_emoji_popover() {
    emoji_popover_ = gtk_popover_new();
    gtk_widget_set_parent(emoji_popover_, compose_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(emoji_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(emoji_popover_), TRUE);

    emoji_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(&client_);
    emoji_picker_shared_->on_selected =
        [this](const std::string& glyph) { emoji_selected(glyph); };
    emoji_picker_surface_->set_root(std::move(shared));

    // Native GtkEntry overlay for the search row. The shared widget paints
    // the affordance; the entry handles IME + selection natively.
    emoji_picker_search_field_ = emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder("Search emoji");
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q) {
            if (emoji_picker_shared_) emoji_picker_shared_->set_search_query(q);
            if (emoji_picker_surface_) emoji_picker_surface_->relayout();
        });
    emoji_picker_surface_->set_on_layout([this] {
        if (emoji_picker_search_field_ && emoji_picker_shared_) {
            emoji_picker_search_field_->set_rect(
                emoji_picker_shared_->search_field_rect());
        }
    });

    // The popover content area is the surface widget. Size to a sensible
    // default; the surface relayouts on resize.
    GtkWidget* surface_widget = emoji_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 320, 360);
    gtk_popover_set_child(GTK_POPOVER(emoji_popover_), surface_widget);
}

void MainWindow::toggle_emoji_picker() {
    if (!emoji_popover_) return;
    if (gtk_widget_get_visible(emoji_popover_)) {
        gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        return;
    }
    // Compose-bar path: ensure the popover is parented to the compose
    // surface and clear any prior `pointing_to` from a reaction popup.
    GtkWidget* desired_parent =
        compose_surface_ ? compose_surface_->widget() : nullptr;
    if (desired_parent && gtk_widget_get_parent(emoji_popover_) != desired_parent) {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), nullptr);
    if (emoji_picker_shared_) emoji_picker_shared_->refresh_frequents();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_text("");
    if (emoji_picker_shared_) emoji_picker_shared_->set_search_query("");
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
}

void MainWindow::popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect) {
    if (!emoji_popover_ || !parent) return;
    // Reparent the popover to the target widget so `pointing_to` is
    // interpreted in that widget's coordinate space.
    if (gtk_widget_get_parent(emoji_popover_) != parent) {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, parent);
    }
    GdkRectangle r{
        .x      = static_cast<int>(local_rect.x),
        .y      = static_cast<int>(local_rect.y),
        .width  = static_cast<int>(local_rect.w),
        .height = static_cast<int>(local_rect.h),
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    if (emoji_picker_shared_) emoji_picker_shared_->refresh_frequents();
    if (emoji_picker_search_field_) emoji_picker_search_field_->set_text("");
    if (emoji_picker_shared_) emoji_picker_shared_->set_search_query("");
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
}

void MainWindow::emoji_selected(const std::string& glyph) {
    // Reaction mode: a "+" chip set pending_reaction_event_id_ before
    // opening the picker. Route the glyph through send_reaction
    // (Rust-side toggle) and skip the compose insert.
    if (!pending_reaction_event_id_.empty()) {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty()) {
            client_.send_reaction(current_room_id_, ev, glyph);
        }
        if (emoji_popover_) gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        return;
    }
    if (!compose_text_area_) return;
    std::string cur = compose_text_area_->text();
    cur += glyph;
    compose_text_area_->set_text(cur);
    if (compose_shared_) compose_shared_->set_current_text(cur);
    compose_text_area_->set_focused(true);
    // The shared picker already calls recent_emoji_bump before invoking
    // this callback. Keep the popover open so users can pick several.
}

} // namespace gtk4
