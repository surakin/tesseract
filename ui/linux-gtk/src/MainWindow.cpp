#include "MainWindow.h"
#include "LoginView.h"

#include "tk/canvas_cairo.h"
#include "tk/theme.h"

#include <cairo.h>
#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_set>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <libintl.h>
#define _(s) gettext(s)

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
    std::vector<std::unique_ptr<tesseract::Event>> snapshot;
};

struct IdleMessageOp {
    MainWindow*                       window;
    std::string                       room_id;
    std::size_t                       index;
    std::unique_ptr<tesseract::Event> ev;   // null for remove
};

struct IdlePaginateResult {
    MainWindow* window;
    std::string room_id;
    bool        reached_start;
};

struct IdleSubscribeResult {
    MainWindow* window;
    std::string room_id;
    bool        reached_start;
};

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------

static MainWindow* cpp_window_for(GtkWindow* w) {
    return reinterpret_cast<MainWindow*>(
        g_object_get_data(G_OBJECT(w), "cpp_window"));
}

void EventHandler::on_timeline_reset(
    const std::string& room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    auto* p = new IdleTimelineReset{
        cpp_window_for(window_),
        room_id,
        std::move(snapshot),
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleTimelineReset*>(data);
        d->window->push_timeline_reset(std::move(d->room_id),
                                         std::move(d->snapshot));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_message_inserted(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    auto* p = new IdleMessageOp{
        cpp_window_for(window_), room_id, index, std::move(ev),
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleMessageOp*>(data);
        d->window->push_message_inserted(std::move(d->room_id),
                                           d->index, std::move(d->ev));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_message_updated(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    auto* p = new IdleMessageOp{
        cpp_window_for(window_), room_id, index, std::move(ev),
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleMessageOp*>(data);
        d->window->push_message_updated(std::move(d->room_id),
                                          d->index, std::move(d->ev));
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_message_removed(
    const std::string& room_id,
    std::size_t index)
{
    auto* p = new IdleMessageOp{
        cpp_window_for(window_), room_id, index, nullptr,
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleMessageOp*>(data);
        d->window->push_message_removed(std::move(d->room_id), d->index);
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

namespace {
struct IdleRoomListState {
    MainWindow*              window;
    tesseract::RoomListState state;
};
} // namespace

void EventHandler::on_room_list_state(tesseract::RoomListState state) {
    auto* p = new IdleRoomListState{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        state
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleRoomListState*>(data);
        d->window->push_room_list_state(d->state);
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

void EventHandler::on_image_packs_updated() {
    // Marshal onto the GTK main loop; the picker reads the cache via
    // Client APIs that aren't safe to call from a worker thread alongside
    // a GTK widget repaint.
    auto* w = reinterpret_cast<MainWindow*>(
        g_object_get_data(G_OBJECT(window_), "cpp_window"));
    g_idle_add([](gpointer data) -> gboolean {
        static_cast<MainWindow*>(data)->push_image_packs_updated();
        return G_SOURCE_REMOVE;
    }, w);
}

namespace {
struct IdleAccountPrefs {
    MainWindow* window;
    std::string json;
};
} // namespace (anonymous, extends the one above)

void EventHandler::on_account_prefs_updated(const std::string& json) {
    auto* p = new IdleAccountPrefs{
        reinterpret_cast<MainWindow*>(
            g_object_get_data(G_OBJECT(window_), "cpp_window")),
        json
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleAccountPrefs*>(data);
        d->window->push_account_prefs_updated(d->json);
        delete d;
        return G_SOURCE_REMOVE;
    }, p);
}

namespace {
struct IdleNotif {
    MainWindow* window;
    std::string room_id, room_name, sender, body;
    bool        is_mention;
};
} // namespace

void EventHandler::on_notification(
    const std::string& room_id, const std::string& room_name,
    const std::string& sender,  const std::string& body, bool is_mention)
{
    auto* p = new IdleNotif{
        cpp_window_for(window_),
        room_id, room_name, sender, body, is_mention
    };
    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<IdleNotif*>(data);
        d->window->push_notification(d->room_id, d->room_name,
                                      d->sender, d->body, d->is_mention);
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
    GtkWidget* main_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(main_overlay), hbox);
    gtk_stack_add_named(GTK_STACK(content_stack_), main_overlay, "main");

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

    // Search field — host-overlaid NativeTextField (a GtkEntry under
    // the hood) shown only when the list overflows the viewport; the
    // RoomListView itself decides visibility in its arrange() pass.
    room_search_field_ = room_surface_->host().make_text_field();
    room_search_field_->set_placeholder("Search rooms");
    room_search_field_->set_visible(false);
    room_search_field_->set_on_changed([this](const std::string& q) {
        search_pending_text_ = q;
        if (search_debounce_id_) {
            g_source_remove(search_debounce_id_);
            search_debounce_id_ = 0;
        }
        search_debounce_id_ = g_timeout_add(500, [](gpointer ud) -> gboolean {
            auto* self = static_cast<MainWindow*>(ud);
            self->search_debounce_id_ = 0;
            if (self->room_list_view_)
                self->room_list_view_->set_search_text(self->search_pending_text_);
            return G_SOURCE_REMOVE;
        }, this);
    });
    room_surface_->set_on_layout([this] {
        if (!room_list_view_ || !room_search_field_) return;
        bool visible = room_list_view_->search_field_visible();
        room_search_field_->set_visible(visible);
        if (visible) {
            room_search_field_->set_rect(
                room_list_view_->search_field_rect());
        }
    });

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
        g_menu_append(menu, _("Logout"), "user.logout");
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
        recovery_key_field_->set_placeholder(_("Recovery key or passphrase"));
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
            // Animated entries first — `on_tk_anim_tick_` keeps
            // `current` valid; static cache is the second hop.
            auto ait = tk_anim_images_.find(mxc);
            if (ait != tk_anim_images_.end() && !ait->second.frames.empty()) {
                return ait->second.frames[ait->second.current].get();
            }
            auto it = tk_images_.find(mxc);
            return it == tk_images_.end() ? nullptr : it->second.get();
        });
    // Voice (MSC3245) playback — GStreamer playbin via tk::gtk4::Host.
    if (auto player = msg_surface_->host().make_audio_player()) {
        message_list_view_->set_audio_player(std::move(player));
    }
    message_list_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return client_.fetch_source_bytes(source_json);
        });
    {
        tk::gtk4::Surface* sfp = msg_surface_.get();
        message_list_view_->set_repaint_requester([sfp]() {
            if (sfp) gtk_widget_queue_draw(sfp->widget());
        });
    }
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
    compose_text_area_->set_placeholder(_("Message\xe2\x80\xa6"));
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

    // Drag-and-drop: dropping any file on either the message list or the
    // composer parks it in the compose bar — images go to the preview
    // band, everything else to the file chip.
    auto on_file_drop = [this](std::vector<std::uint8_t> bytes,
                               std::string mime,
                               std::string filename) {
        if (!compose_shared_) return;
        const auto limit = client_.media_upload_limit();
        if (limit > 0 && bytes.size() > limit) {
            if (status_bar_) {
                std::string msg = std::string(_("File exceeds server limit ("))
                                + tesseract::views::format_size(limit) + ")";
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
            return;
        }
        if (mime.rfind("image/", 0) == 0) {
            compose_shared_->set_pending_image(std::move(bytes),
                                               std::move(mime),
                                               std::move(filename));
        } else {
            compose_shared_->set_pending_file(std::move(bytes),
                                              std::move(mime),
                                              std::move(filename));
        }
    };
    compose_surface_->set_on_file_drop(on_file_drop);
    if (msg_surface_) msg_surface_->set_on_file_drop(on_file_drop);
    compose_surface_->set_on_layout([this] {
        if (compose_shared_ && compose_text_area_)
            compose_text_area_->set_rect(compose_shared_->text_area_rect());
    });

    compose_shared_->on_send  = [this](const std::string& body) {
        if (current_room_id_.empty()) return;
        auto l = body.find_first_not_of(" \t\n\r");
        auto r = body.find_last_not_of(" \t\n\r");
        if (l == std::string::npos) return;
        std::string trimmed = body.substr(l, r - l + 1);
        if (trimmed.empty()) return;
        auto res = client_.send_message(current_room_id_, trimmed);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            compose_shared_->set_current_text({});
        }
    };
    compose_shared_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                              std::string mime,
                                              std::string filename,
                                              std::string caption,
                                              std::uint32_t /*src_w*/,
                                              std::uint32_t /*src_h*/,
                                              std::string reply_event_id) {
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
                                        enc.width, enc.height,
                                        reply_event_id);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        }
    };
    compose_shared_->on_send_file = [this](std::vector<std::uint8_t> bytes,
                                             std::string mime,
                                             std::string filename,
                                             std::string caption,
                                             std::string reply_event_id) {
        if (current_room_id_.empty()) return;
        auto res = client_.send_file(current_room_id_, bytes, mime,
                                      filename, caption, reply_event_id);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        } else if (status_bar_) {
            std::string msg = std::string(_("Send file failed: ")) + res.message;
            gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        }
    };
    compose_shared_->on_size_changed = [this] {
        if (!compose_shared_ || !compose_surface_) return;
        gtk_widget_set_size_request(compose_surface_->widget(), -1,
            static_cast<int>(compose_shared_->natural_height()));
        compose_surface_->relayout();
    };
    compose_shared_->on_emoji   = [this] { toggle_emoji_picker(); };
    compose_shared_->on_sticker = [this] { toggle_sticker_picker(); };
    compose_shared_->on_send_reply = [this](const std::string& reply_event_id,
                                             const std::string& body) {
        if (body.empty() || current_room_id_.empty()) return;
        auto res = client_.send_reply(current_room_id_, reply_event_id, body);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        } else if (status_bar_) {
            std::string msg = std::string(_("Send reply failed: ")) + res.message;
            gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        }
    };

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

    // "↩" hover button → enter reply mode in the compose bar.
    message_list_view_->on_reply_requested =
        [this](const std::string& event_id,
               const std::string& sender_name,
               const std::string& body_preview) {
            if (!compose_shared_) return;
            compose_shared_->set_reply_to(event_id, sender_name, body_preview);
            if (compose_text_area_) compose_text_area_->set_focused(true);
        };

    // "✏" hover button → enter edit mode in the compose bar.
    message_list_view_->on_edit_requested =
        [this](const std::string& event_id, const std::string& current_body) {
            if (!compose_shared_) return;
            compose_shared_->set_editing(event_id);
            if (compose_text_area_) {
                compose_text_area_->set_text(current_body);
                compose_shared_->set_current_text(current_body);
                compose_text_area_->set_focused(true);
            }
        };

    compose_shared_->on_send_edit = [this](const std::string& event_id,
                                            const std::string& new_body) {
        if (new_body.empty() || current_room_id_.empty()) return;
        auto res = client_.send_edit(current_room_id_, event_id, new_body);
        if (res) {
            if (compose_text_area_) compose_text_area_->set_text("");
            if (compose_shared_)    compose_shared_->set_current_text({});
        } else if (status_bar_) {
            std::string msg = std::string(_("Edit failed: ")) + res.message;
            gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        }
    };

    compose_shared_->on_edit_cancelled = [this] {
        if (compose_text_area_) compose_text_area_->set_text("");
        if (compose_shared_)    compose_shared_->set_current_text({});
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
    build_sticker_popover();
    build_sticker_context_menu();

    // Right-click on the message surface: hit-test sticker rects and
    // pop the context menu. Gesture is added after msg_surface_ +
    // sticker_ctx_menu_ both exist.
    {
        GtkGesture* gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                       GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(on_msg_right_click_), this);
        gtk_widget_add_controller(msg_surface_->widget(),
                                   GTK_EVENT_CONTROLLER(gesture));
    }

    // Image / sticker lightbox overlay — an extra GtkOverlay child that
    // paints a dark backdrop + the selected image over the entire main area.
    // Shown on `on_image_clicked`, hidden on `on_close` or Escape.
    {
        img_viewer_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
        auto img_viewer_owner = std::make_unique<tesseract::views::ImageViewerOverlay>();
        img_viewer_ = img_viewer_owner.get();
        img_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto ait = tk_anim_images_.find(url);
                if (ait != tk_anim_images_.end() && !ait->second.frames.empty())
                    return ait->second.frames[ait->second.current].get();
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        img_viewer_->on_close = [this] {
            if (img_viewer_surface_)
                gtk_widget_set_visible(img_viewer_surface_->widget(), FALSE);
        };
        img_viewer_surface_->set_root(std::move(img_viewer_owner));

        GtkWidget* overlay_widget = img_viewer_surface_->widget();
        gtk_widget_set_hexpand(overlay_widget, TRUE);
        gtk_widget_set_vexpand(overlay_widget, TRUE);
        gtk_overlay_add_overlay(GTK_OVERLAY(main_overlay), overlay_widget);
        gtk_widget_set_visible(overlay_widget, FALSE);
    }

    message_list_view_->on_image_clicked =
        [this](const tesseract::views::MessageListView::ImageHit& hit) {
            if (!img_viewer_ || !img_viewer_surface_) return;
            img_viewer_->open(hit.media_url, hit.body, hit.natural_w, hit.natural_h);
            gtk_widget_set_visible(img_viewer_surface_->widget(), TRUE);
            gtk_widget_grab_focus(img_viewer_surface_->widget());
        };

    // Video lightbox overlay — full-window surface for m.video playback.
    {
        vid_viewer_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
        auto vid_viewer_owner = std::make_unique<tesseract::views::VideoViewerOverlay>();
        vid_viewer_ = vid_viewer_owner.get();
        vid_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        vid_viewer_->set_video_player(msg_surface_->host().make_video_player());
        vid_viewer_->set_repaint_requester([this] {
            if (vid_viewer_surface_) vid_viewer_surface_->relayout();
        });
        vid_viewer_->on_close = [this] {
            if (vid_viewer_surface_)
                gtk_widget_set_visible(vid_viewer_surface_->widget(), FALSE);
        };
        vid_viewer_surface_->set_root(std::move(vid_viewer_owner));

        GtkWidget* vid_overlay_widget = vid_viewer_surface_->widget();
        gtk_widget_set_hexpand(vid_overlay_widget, TRUE);
        gtk_widget_set_vexpand(vid_overlay_widget, TRUE);
        gtk_overlay_add_overlay(GTK_OVERLAY(main_overlay), vid_overlay_widget);
        gtk_widget_set_visible(vid_overlay_widget, FALSE);
    }

    message_list_view_->on_video_clicked =
        [this](const tesseract::views::MessageListView::VideoHit& hit) {
            if (!vid_viewer_ || !vid_viewer_surface_) return;
            vid_viewer_->open(hit.source_json, hit.thumbnail_url, hit.mime_type,
                             hit.duration_ms, hit.natural_w, hit.natural_h);
            gtk_widget_set_visible(vid_viewer_surface_->widget(), TRUE);
            gtk_widget_grab_focus(vid_viewer_surface_->widget());
            // Async byte fetch on a detached thread.
            std::string src = hit.source_json;
            std::thread([this, src = std::move(src)]() mutable {
                auto bytes = client_.fetch_source_bytes(src);
                struct Ctx { MainWindow* self; std::vector<uint8_t> bytes; };
                auto* ctx = new Ctx{ this, std::move(bytes) };
                g_idle_add([](gpointer p) -> gboolean {
                    auto* c = static_cast<Ctx*>(p);
                    if (c->self->vid_viewer_)
                        c->self->vid_viewer_->load_bytes(c->bytes.data(), c->bytes.size());
                    delete c;
                    return G_SOURCE_REMOVE;
                }, ctx);
            }).detach();
        };

    // Escape key: close the image viewer if it's open. Attached to the
    // window so it fires regardless of which widget holds focus.
    {
        GtkEventController* key_ctl = gtk_event_controller_key_new();
        g_signal_connect(key_ctl, "key-pressed",
                         G_CALLBACK(on_window_key_pressed_), this);
        gtk_widget_add_controller(window_, key_ctl);
    }

    status_bar_ = gtk_label_new(_("Not logged in"));
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_widget_set_margin_bottom(status_bar_, 2);
    gtk_box_append(GTK_BOX(vbox), status_bar_);

    gtk_widget_set_visible(window_, TRUE);

    notifier_ = std::make_unique<LinuxNotifierGtk>(
        [this](std::string room_id) { navigate_to_room(std::move(room_id)); });

    g_signal_connect(window_, "close-request",
                     G_CALLBACK(&MainWindow::on_window_close_request_), this);

    g_idle_add([](gpointer data) -> gboolean {
        static_cast<MainWindow*>(data)->do_login();
        return G_SOURCE_REMOVE;
    }, this);
}

void MainWindow::start_tray_if_needed_() {
    if (tray_) return;
    tray_ = std::make_unique<LinuxGtkTrayIcon>(
        [this]{
            gtk_window_present(GTK_WINDOW(window_));
        },
        [this]{
            // Real quit: drop the tray so close-request falls through to
            // the default (window destroyed → app holds nothing → quits).
            tray_.reset();
            g_application_quit(G_APPLICATION(app_));
        });
    if (tray_->is_available()) {
        // Keep the GApplication alive when the window is hidden.
        g_application_hold(G_APPLICATION(app_));
    } else {
        tray_.reset();
    }
}

gboolean MainWindow::on_window_close_request_(GtkWindow* /*window*/, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->tray_ && self->tray_->is_available()) {
        gtk_widget_set_visible(self->window_, FALSE);
        return TRUE;  // stop default destruction
    }
    return FALSE;
}

MainWindow::~MainWindow() {
    if (search_debounce_id_) {
        g_source_remove(search_debounce_id_);
        search_debounce_id_ = 0;
    }
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
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Restoring session\xe2\x80\xa6"));
        auto res = client_.restore_session(*saved);
        if (res) {
            my_user_id_           = client_.get_user_id();
            my_display_name_      = client_.get_display_name();
            my_avatar_url_        = client_.get_avatar_url();
            pending_restore_room_ = tesseract::Prefs::parse(client_.load_prefs_json()).last_room;
            populate_user_strip();
            event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
            client_.start_sync(event_handler_.get());
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            maybe_show_recovery_banner();
            start_tray_if_needed_();
            return;
        }
        tesseract::SessionStore::clear();
        status_msg = std::string(_("Saved session expired: ")) + res.message;
    }

    login_view_->reset();
    login_view_->set_status_message(status_msg);
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Not logged in"));
}

void MainWindow::on_login_succeeded() {
    my_user_id_           = client_.get_user_id();
    my_display_name_      = client_.get_display_name();
    my_avatar_url_        = client_.get_avatar_url();
    pending_restore_room_ = tesseract::Prefs::parse(client_.load_prefs_json()).last_room;
    populate_user_strip();
    tesseract::SessionStore::save(client_.export_session());
    event_handler_ = std::make_unique<EventHandler>(GTK_WINDOW(window_));
    client_.start_sync(event_handler_.get());
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    maybe_show_recovery_banner();
    start_tray_if_needed_();
}

void MainWindow::on_send_clicked() {
    if (compose_shared_) compose_shared_->trigger_send();
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
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_.load_prefs_json());
        prefs.last_room = room_id;
        client_.save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (compose_shared_) {
        compose_shared_->clear_reply();
        compose_shared_->clear_editing();
    }
    for (const auto& r : rooms_)
        if (r.id == current_room_id_) { update_room_header(r); break; }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the GTK main loop stays responsive.
    std::string sub_room = current_room_id_;
    std::thread([this, sub_room]{
        auto res = client_.subscribe_room(sub_room);
        bool reached = false;
        if (res) {
            auto pr = client_.paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            client_.start_background_backfill();
        }
        auto* d = new IdleSubscribeResult{this, sub_room, reached};
        g_idle_add([](gpointer data) -> gboolean {
            auto* dd = static_cast<IdleSubscribeResult*>(data);
            dd->window->push_subscribe_result(std::move(dd->room_id),
                                               dd->reached_start);
            delete dd;
            return G_SOURCE_REMOVE;
        }, d);
    }).detach();
}

void MainWindow::push_paginate_result(std::string room_id, bool reached_start) {
    auto it = pagination_.find(room_id);
    if (it == pagination_.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached_start;
    if (room_id == current_room_id_ && message_list_view_)
        message_list_view_->reset_near_top_latch();
}

void MainWindow::push_subscribe_result(std::string room_id, bool reached_start) {
    if (room_id != current_room_id_) return;
    auto& state = pagination_[room_id];
    state.in_flight     = false;
    state.reached_start = reached_start;
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

void MainWindow::push_message_inserted(
    std::string room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev) return;
    if (room_id != current_room_id_) return;
    if (ev->type == tesseract::EventType::Unhandled) return;
    ensure_row_media(*ev);
    ensure_reply_details(ev->in_reply_to_id);
    message_list_view_->insert_message(index, to_row_data(*ev));
    msg_surface_->relayout();
}

void MainWindow::push_message_updated(
    std::string room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev) return;
    if (room_id != current_room_id_) return;
    if (ev->type == tesseract::EventType::Unhandled) return;
    ensure_row_media(*ev);
    ensure_reply_details(ev->in_reply_to_id);
    message_list_view_->update_message(index, to_row_data(*ev));
    msg_surface_->relayout();
}

void MainWindow::push_message_removed(std::string room_id, std::size_t index) {
    if (room_id != current_room_id_) return;
    message_list_view_->remove_message(index);
    msg_surface_->relayout();
}

void MainWindow::push_rooms(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    refresh_room_list();
    if (!current_room_id_.empty()) {
        for (const auto& r : rooms_)
            if (r.id == current_room_id_) { update_room_header(r); break; }
    } else if (!pending_restore_room_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == pending_restore_room_ && !r.is_space) {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                on_room_selected(target);
                break;
            }
        }
    }
}

void MainWindow::handle_reconnect() {
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Sync error: reconnecting\xe2\x80\xa6"));
    client_.stop_sync();
    do_login();
}

void MainWindow::handle_auth_error(bool soft_logout) {
    if (soft_logout) {
        if (auto saved = tesseract::SessionStore::load()) {
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Reconnecting session\xe2\x80\xa6"));
            if (client_.restore_session(*saved)) {
                my_user_id_       = client_.get_user_id();
                my_display_name_  = client_.get_display_name();
                my_avatar_url_    = client_.get_avatar_url();
                populate_user_strip();
                client_.start_sync(event_handler_.get());
                gtk_label_set_text(GTK_LABEL(status_bar_), _("Reconnected"));
                maybe_show_recovery_banner();
                return;
            }
        }
    }
    tesseract::SessionStore::clear();
    client_.stop_sync();
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Session expired; please log in again."));
    do_login();
}

void MainWindow::push_error(std::string description) {
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}

void MainWindow::push_timeline_reset(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    if (room_id != current_room_id_) return;
    std::vector<tesseract::views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto& ev : snapshot) {
        if (!ev) continue;
        ensure_row_media(*ev);
        ensure_reply_details(ev->in_reply_to_id);
        rows.push_back(to_row_data(*ev));
    }
    message_list_view_->set_messages(std::move(rows));
    msg_surface_->relayout();
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
//
// Inner helper: convert an already-decoded GdkPixbuf into a premultiplied
// ARGB32 cairo surface. Reused by both the static decoder and the
// animated-frame iterator below.
cairo_surface_t* pixbuf_to_premultiplied_argb32(GdkPixbuf* pb) {
    if (!pb) return nullptr;
    int  w        = gdk_pixbuf_get_width (pb);
    int  h        = gdk_pixbuf_get_height(pb);
    int  channels = gdk_pixbuf_get_n_channels(pb);
    int  in_stride = gdk_pixbuf_get_rowstride(pb);
    const guchar* pixels = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
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
    return surface;
}

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
    cairo_surface_t* surface = pixbuf_to_premultiplied_argb32(pb);
    g_object_unref(loader);
    return surface;
}

// Decode an animated GIF / WebP / APNG into a list of premultiplied
// ARGB32 cairo surfaces + a per-frame delay (ms). Returns nullopt for
// non-animated payloads — callers should fall back to the static path.
//
// Termination: walks the GdkPixbufAnimationIter forwards with a
// synthesised clock advanced by each frame's reported delay. Capped at
// `kMaxFrames` to keep runaway / never-ending GIFs from blowing memory.
// Most animated stickers ship ≤ 30 frames.
struct DecodedAnimation {
    std::vector<cairo_surface_t*> frames;       // caller owns each
    std::vector<int>              delays_ms;
};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
std::optional<DecodedAnimation> decode_animation(
    const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return std::nullopt;
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err)) {
        if (err) g_error_free(err);
        g_object_unref(loader);
        return std::nullopt;
    }
    if (!gdk_pixbuf_loader_close(loader, &err)) {
        if (err) g_error_free(err);
        g_object_unref(loader);
        return std::nullopt;
    }
    GdkPixbufAnimation* anim = gdk_pixbuf_loader_get_animation(loader);
    if (!anim || gdk_pixbuf_animation_is_static_image(anim)) {
        g_object_unref(loader);
        return std::nullopt;
    }

    GTimeVal t = { 0, 0 };
    GdkPixbufAnimationIter* iter =
        gdk_pixbuf_animation_get_iter(anim, &t);
    if (!iter) {
        g_object_unref(loader);
        return std::nullopt;
    }

    DecodedAnimation out;
    constexpr int kMaxFrames = 200;
    for (int i = 0; i < kMaxFrames; ++i) {
        GdkPixbuf* pb = gdk_pixbuf_animation_iter_get_pixbuf(iter);
        if (!pb) break;
        cairo_surface_t* surf = pixbuf_to_premultiplied_argb32(pb);
        if (!surf) break;
        int delay = gdk_pixbuf_animation_iter_get_delay_time(iter);
        // -1 means there's no upcoming frame (last frame of a
        // non-looping animation). Capture this final frame and stop.
        if (delay < 0) {
            out.frames.push_back(surf);
            out.delays_ms.push_back(100); // arbitrary tail-hold
            break;
        }
        if (delay < 20) delay = 20;
        out.frames.push_back(surf);
        out.delays_ms.push_back(delay);

        // Advance the synthesised clock by the just-captured delay.
        t.tv_usec += delay * 1000;
        while (t.tv_usec >= G_USEC_PER_SEC) {
            t.tv_sec  += 1;
            t.tv_usec -= G_USEC_PER_SEC;
        }
        if (!gdk_pixbuf_animation_iter_advance(iter, &t)) {
            // Iterator decided no new frame would be shown — we'd
            // duplicate the same pixbuf on the next iteration. Stop.
            break;
        }
    }
    g_object_unref(iter);
    g_object_unref(loader);
    if (out.frames.empty()) return std::nullopt;
    return out;
}
G_GNUC_END_IGNORE_DEPRECATIONS

} // namespace

// These three helpers used to call the synchronous Rust FFI on the UI
// thread. `fetch_avatar_bytes` / `fetch_media_bytes` do a
// `tokio::block_on` inside; on first sync of an account with many rooms
// `show_rooms` froze the GTK main loop for minutes (one network
// round-trip per room avatar, serialised on the UI thread). Decode +
// cache now happens after a worker thread lands the bytes back via
// `g_idle_add`; the call sites return immediately and the views paint
// initials placeholders until the bytes arrive.
void MainWindow::ensure_room_avatar(const tesseract::RoomInfo& r) {
    request_room_avatar_async(r.id, r.avatar_url);
}

void MainWindow::ensure_user_avatar(const std::string& mxc) {
    request_user_avatar_async(mxc);
}

void MainWindow::ensure_media_image(const std::string& url,
                                      int /*max_w*/, int /*max_h*/) {
    request_media_image_async(url);
}

void MainWindow::ensure_reply_details(const std::string& event_id) {
    if (event_id.empty() || current_room_id_.empty()) return;
    if (!reply_details_requested_.insert(event_id).second) return;
    client_.fetch_reply_details(current_room_id_, event_id);
}

void MainWindow::start_anim_tick_if_needed_() {
    if (tk_anim_tick_id_ != 0) return;
    if (tk_anim_images_.empty()) return;
    tk_anim_tick_id_ = g_timeout_add(16, on_tk_anim_tick_, this);
}

void MainWindow::invalidate_anim_consumers_() {
    if (msg_surface_) msg_surface_->relayout();
    if (sticker_picker_shared_)
        sticker_picker_shared_->invalidate_image_cache();
    if (sticker_picker_surface_) sticker_picker_surface_->relayout();
}

gboolean MainWindow::on_tk_anim_tick_(gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->tk_anim_images_.empty()) {
        self->tk_anim_tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    const std::int64_t now_ms = g_get_monotonic_time() / 1000;
    bool any_changed = false;
    for (auto& [_, entry] : self->tk_anim_images_) {
        if (entry.frames.size() <= 1) continue;
        std::size_t steps = 0;
        while (now_ms >= entry.next_advance_ms
                && steps < entry.frames.size())
        {
            entry.current =
                (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms +=
                entry.delays_ms[entry.current];
            ++steps;
        }
        if (steps > 0) any_changed = true;
    }
    if (any_changed) self->invalidate_anim_consumers_();
    return G_SOURCE_CONTINUE;
}

void MainWindow::request_room_avatar_async(const std::string& room_id,
                                              const std::string& mxc) {
    if (room_id.empty() || mxc.empty() || tk_avatars_.count(mxc)) return;
    if (!media_fetches_in_flight_.insert(mxc).second) return;

    struct IdleData {
        MainWindow*           self;
        std::string           mxc;
        std::vector<uint8_t>  bytes;
    };

    std::thread([this, room_id, mxc]() mutable {
        auto bytes = client_.fetch_avatar_bytes(room_id);
        auto* data = new IdleData{ this, std::move(mxc), std::move(bytes) };
        g_idle_add([](gpointer p) -> gboolean {
            auto* d = static_cast<IdleData*>(p);
            d->self->media_fetches_in_flight_.erase(d->mxc);
            if (!d->bytes.empty() && !d->self->tk_avatars_.count(d->mxc)) {
                if (cairo_surface_t* surface =
                        decode_image_to_cairo_surface(d->bytes))
                {
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    d->self->tk_avatars_.emplace(d->mxc, std::move(img));
                    if (d->self->room_surface_)
                        d->self->room_surface_->relayout();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        }, data);
    }).detach();
}

void MainWindow::request_user_avatar_async(const std::string& mxc) {
    if (mxc.empty() || tk_avatars_.count(mxc)) return;
    if (!media_fetches_in_flight_.insert(mxc).second) return;

    struct IdleData {
        MainWindow*           self;
        std::string           mxc;
        std::vector<uint8_t>  bytes;
    };

    std::thread([this, mxc]() mutable {
        auto bytes = client_.fetch_media_bytes(mxc);
        auto* data = new IdleData{ this, std::move(mxc), std::move(bytes) };
        g_idle_add([](gpointer p) -> gboolean {
            auto* d = static_cast<IdleData*>(p);
            d->self->media_fetches_in_flight_.erase(d->mxc);
            if (!d->bytes.empty() && !d->self->tk_avatars_.count(d->mxc)) {
                if (cairo_surface_t* surface =
                        decode_image_to_cairo_surface(d->bytes))
                {
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    d->self->tk_avatars_.emplace(d->mxc, std::move(img));
                    if (d->self->msg_surface_)
                        d->self->msg_surface_->relayout();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        }, data);
    }).detach();
}

void MainWindow::request_media_image_async(const std::string& url) {
    if (url.empty()) return;
    if (tk_images_.count(url) || tk_anim_images_.count(url)) return;
    if (!media_fetches_in_flight_.insert(url).second) return;

    struct IdleData {
        MainWindow*           self;
        std::string           url;
        std::vector<uint8_t>  bytes;
    };

    std::thread([this, url]() mutable {
        // `url` may be plain mxc (plain images/stickers) or a JSON
        // MediaSource (encrypted images/stickers + reaction sources).
        // `fetch_source_bytes` handles both shapes; `fetch_media_bytes`
        // only handles plain mxc and would return empty for encrypted.
        auto bytes = client_.fetch_source_bytes(url);
        auto* data = new IdleData{ this, std::move(url), std::move(bytes) };
        g_idle_add([](gpointer p) -> gboolean {
            auto* d = static_cast<IdleData*>(p);
            d->self->media_fetches_in_flight_.erase(d->url);
            if (!d->bytes.empty()
                && !d->self->tk_images_.count(d->url)
                && !d->self->tk_anim_images_.count(d->url))
            {
                // Animated probe first; static fallback decodes via
                // decode_image_to_cairo_surface.
                if (auto anim = decode_animation(d->bytes)) {
                    AnimatedImage entry;
                    entry.frames.reserve(anim->frames.size());
                    entry.delays_ms = std::move(anim->delays_ms);
                    for (cairo_surface_t* s : anim->frames) {
                        entry.frames.push_back(
                            tk::cairo_pango::make_image(s));
                        cairo_surface_destroy(s);
                    }
                    if (!entry.frames.empty()) {
                        entry.current         = 0;
                        const gint64 now_ms   = g_get_monotonic_time() / 1000;
                        entry.next_advance_ms = now_ms + entry.delays_ms[0];
                        d->self->tk_anim_images_.emplace(
                            d->url, std::move(entry));
                        d->self->start_anim_tick_if_needed_();
                        if (d->self->msg_surface_)
                            d->self->msg_surface_->relayout();
                    }
                } else if (cairo_surface_t* surface =
                              decode_image_to_cairo_surface(d->bytes))
                {
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    d->self->tk_images_.emplace(d->url, std::move(img));
                    if (d->self->msg_surface_)
                        d->self->msg_surface_->relayout();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        }, data);
    }).detach();
}

void MainWindow::ensure_sticker_image_async(std::string url) {
    if (url.empty() || tk_images_.count(url) || tk_anim_images_.count(url))
        return;
    if (!sticker_fetches_in_flight_.insert(url).second) return;

    struct IdleData {
        MainWindow*           self;
        std::string           url;
        std::vector<uint8_t>  bytes;
    };

    // Detached worker — `client_` is thread-safe (the SDK runs on its own
    // tokio runtime; FFI calls are sync wrappers). The picker outlives
    // any in-flight fetch.
    std::thread([this, url]() mutable {
        auto bytes = client_.fetch_source_bytes(url);
        auto* data = new IdleData{ this, std::move(url), std::move(bytes) };
        g_idle_add([](gpointer p) -> gboolean {
            auto* d = static_cast<IdleData*>(p);
            d->self->sticker_fetches_in_flight_.erase(d->url);
            if (!d->bytes.empty()
                && !d->self->tk_images_.count(d->url)
                && !d->self->tk_anim_images_.count(d->url))
            {
                // Probe for animation first; animated stickers
                // (GIF / animated WebP / APNG) land in tk_anim_images_
                // and ride the frame-tick loop. Static formats fall
                // through to the existing tk_images_ path.
                if (auto anim = decode_animation(d->bytes)) {
                    AnimatedImage entry;
                    entry.frames.reserve(anim->frames.size());
                    entry.delays_ms = std::move(anim->delays_ms);
                    for (cairo_surface_t* s : anim->frames) {
                        entry.frames.push_back(
                            tk::cairo_pango::make_image(s));
                        cairo_surface_destroy(s);
                    }
                    if (!entry.frames.empty()) {
                        entry.current         = 0;
                        gint64 now_ms =
                            g_get_monotonic_time() / 1000;
                        entry.next_advance_ms =
                            now_ms + entry.delays_ms[0];
                        d->self->tk_anim_images_.emplace(
                            d->url, std::move(entry));
                        d->self->start_anim_tick_if_needed_();
                        d->self->invalidate_anim_consumers_();
                    }
                } else if (cairo_surface_t* surface =
                              decode_image_to_cairo_surface(d->bytes))
                {
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    d->self->tk_images_.emplace(d->url, std::move(img));
                    if (d->self->sticker_picker_shared_)
                        d->self->sticker_picker_shared_->invalidate_image_cache();
                    if (d->self->sticker_picker_surface_)
                        d->self->sticker_picker_surface_->relayout();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        }, data);
    }).detach();
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
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_) {
            if (!r.is_space) continue;
            for (const auto& id : client_.space_children(r.id))
                in_space.insert(id);
        }
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
        for (const auto& r : rooms_)
            if ( r.is_space) filtered.push_back(r);
        show_rooms(filtered);
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
    row.read_receipts     = ev.read_receipts;

    row.in_reply_to_id          = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    row.in_reply_to_body        = ev.in_reply_to_body;
    row.is_edited               = ev.is_edited;

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
        case tesseract::EventType::Voice: {
            row.kind = Kind::Voice;
            const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
            row.audio_source = v.audio_source;
            row.audio_mime   = v.mime_type;
            row.duration_ms  = v.duration_ms;
            row.waveform     = v.waveform;
            break;
        }
        case tesseract::EventType::Video: {
            row.kind = Kind::Video;
            const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
            row.media_url         = vid.video_url;
            row.video_thumb_url   = vid.thumbnail_url.empty()
                                    ? ("thumb::" + ev.event_id)
                                    : vid.thumbnail_url;
            row.media_w           = static_cast<int>(vid.width);
            row.media_h           = static_cast<int>(vid.height);
            row.duration_ms       = vid.duration_ms;
            row.has_filename_caption = !vid.filename.empty();
            break;
        }
        case tesseract::EventType::Redacted:  row.kind = Kind::Redacted;  break;
        case tesseract::EventType::Unhandled: row.kind = Kind::Unhandled; break;
    }
    return row;
}

void MainWindow::ensure_row_media(const tesseract::Event& ev) {
    // Pre-fetch any media this row will reference. The shared view's
    // provider lambdas look up tk_avatars_ / tk_images_ on each paint.
    ensure_user_avatar(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts) {
        ensure_user_avatar(rr.avatar_url);
    }
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
    } else if (ev.type == tesseract::EventType::Voice) {
        const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            voice_prefetched_.insert(v.audio_source).second) {
            // Background-prime the SDK media cache so the first play tap
            // is instant. We discard the bytes — the view's synchronous
            // fetch on click reads them straight out of the cache.
            std::thread([this, src = v.audio_source]() mutable {
                (void)client_.fetch_source_bytes(src);
            }).detach();
        }
    } else if (ev.type == tesseract::EventType::Video) {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
            ensure_media_image(vid.thumbnail_url,
                                tesseract::visual::kMaxInlineImageWidth,
                                tesseract::visual::kMaxInlineImageHeight);
        // Client-side first-frame generation when no server thumbnail.
        if (vid.thumbnail_url.empty() && !vid.video_url.empty() &&
            video_thumb_in_flight_.insert(ev.event_id).second) {
            const std::string eid = ev.event_id;
            std::thread([this, eid, src = vid.video_url]() mutable {
                auto bytes = client_.fetch_source_bytes(src);
                if (bytes.empty()) return;
                // Extract first frame via GStreamer appsink.
                GstElement* pipe  = gst_pipeline_new(nullptr);
                GstElement* gsrc  = gst_element_factory_make("giostreamsrc",  nullptr);
                GstElement* dec   = gst_element_factory_make("decodebin",     nullptr);
                GstElement* vconv = gst_element_factory_make("videoconvert",  nullptr);
                GstElement* vsink = gst_element_factory_make("appsink",       nullptr);
                if (!pipe || !gsrc || !dec || !vconv || !vsink) {
                    if (pipe)  gst_object_unref(pipe);
                    if (gsrc)  gst_object_unref(gsrc);
                    if (dec)   gst_object_unref(dec);
                    if (vconv) gst_object_unref(vconv);
                    if (vsink) gst_object_unref(vsink);
                    return;
                }
                GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
                gst_app_sink_set_caps(GST_APP_SINK(vsink), caps);
                gst_caps_unref(caps);
                gst_app_sink_set_drop(GST_APP_SINK(vsink), FALSE);
                gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 1);
                GInputStream* mem_stream = g_memory_input_stream_new_from_data(
                    bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
                g_object_set(gsrc, "stream", mem_stream, nullptr);
                g_object_unref(mem_stream);
                gst_bin_add_many(GST_BIN(pipe), gsrc, dec, vconv, vsink, nullptr);
                gst_element_link(gsrc, dec);
                gst_element_link(vconv, vsink);
                struct PadCtx { GstElement* vconv; };
                auto* pad_ctx = new PadCtx{ vconv };
                g_signal_connect(dec, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer ud){
                    auto* pc = static_cast<PadCtx*>(ud);
                    GstCaps* c2 = gst_pad_get_current_caps(pad);
                    if (!c2) c2 = gst_pad_query_caps(pad, nullptr);
                    GstStructure* st = gst_caps_get_structure(c2, 0);
                    if (g_str_has_prefix(gst_structure_get_name(st), "video")) {
                        GstPad* sp = gst_element_get_static_pad(pc->vconv, "sink");
                        if (sp && !gst_pad_is_linked(sp)) gst_pad_link(pad, sp);
                        if (sp) gst_object_unref(sp);
                    }
                    gst_caps_unref(c2);
                }), pad_ctx);
                gst_element_set_state(pipe, GST_STATE_PLAYING);
                // Pull exactly one preroll frame.
                GstSample* sample = gst_app_sink_pull_preroll(GST_APP_SINK(vsink));
                gst_element_set_state(pipe, GST_STATE_NULL);
                delete pad_ctx;
                gst_object_unref(pipe);
                if (!sample) return;
                GstBuffer* buf  = gst_sample_get_buffer(sample);
                GstCaps*   scaps = gst_sample_get_caps(sample);
                int w = 0, h = 0;
                if (scaps) {
                    GstStructure* st = gst_caps_get_structure(scaps, 0);
                    gst_structure_get_int(st, "width",  &w);
                    gst_structure_get_int(st, "height", &h);
                }
                if (!buf || w <= 0 || h <= 0) { gst_sample_unref(sample); return; }
                GstMapInfo map;
                if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
                    gst_sample_unref(sample); return;
                }
                std::vector<uint8_t> frame_bytes(map.data, map.data + map.size);
                gst_buffer_unmap(buf, &map);
                gst_sample_unref(sample);
                // BGRA → cairo surface on the main thread.
                struct Ctx {
                    MainWindow*          self;
                    std::string          key;
                    std::vector<uint8_t> pixels;
                    int w, h;
                };
                auto* ctx = new Ctx{ this, "thumb::" + eid, std::move(frame_bytes), w, h };
                g_idle_add([](gpointer p) -> gboolean {
                    auto* c = static_cast<Ctx*>(p);
                    if (!c->self->tk_images_.count(c->key)) {
                        // Create an owned cairo surface and blit the BGRA pixels in.
                        cairo_surface_t* surf =
                            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, c->w, c->h);
                        if (surf && cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                            int dst_stride = cairo_image_surface_get_stride(surf);
                            unsigned char* dst = cairo_image_surface_get_data(surf);
                            int src_stride = c->w * 4;
                            for (int row = 0; row < c->h; ++row) {
                                std::memcpy(dst + row * dst_stride,
                                             c->pixels.data() + row * src_stride,
                                             static_cast<std::size_t>(src_stride));
                            }
                            cairo_surface_mark_dirty(surf);
                            c->self->tk_images_.emplace(
                                c->key, tk::cairo_pango::make_image(surf));
                            cairo_surface_destroy(surf);
                            if (c->self->msg_surface_) c->self->msg_surface_->relayout();
                        } else if (surf) {
                            cairo_surface_destroy(surf);
                        }
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                }, ctx);
            }).detach();
        }
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            ensure_media_image(r.source_json, 20, 20);
    }
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
                _("Please enter a recovery key or passphrase."));
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

void MainWindow::push_image_packs_updated() {
    apply_image_packs_updated();
}

void MainWindow::push_account_prefs_updated(const std::string& json) {
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty() && pending_restore_room_.empty() && current_room_id_.empty())
        pending_restore_room_ = prefs.last_room;
}

void MainWindow::push_notification(
        const std::string& room_id, const std::string& room_name,
        const std::string& sender, const std::string& body, bool is_mention)
{
    handle_notification(room_id, room_name, sender, body, is_mention);
}

void MainWindow::handle_notification(
        const std::string& room_id, const std::string& room_name,
        const std::string& sender,  const std::string& body, bool is_mention)
{
    if (gtk_window_is_active(GTK_WINDOW(window_)) && current_room_id_ == room_id)
        return;
    if (notifier_)
        notifier_->notify({ room_id, room_name, sender, body, is_mention });
}

void MainWindow::navigate_to_room(const std::string& room_id) {
    if (room_id.empty()) return;
    if (room_list_view_) room_list_view_->set_selected_room(room_id);
    on_room_selected(room_id);
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::apply_image_packs_updated() {
    if (sticker_picker_shared_) sticker_picker_shared_->refresh_packs();
    if (sticker_picker_surface_) sticker_picker_surface_->relayout();
    if (emoji_picker_shared_) emoji_picker_shared_->refresh_emoticon_packs();
    if (emoji_picker_surface_) emoji_picker_surface_->relayout();
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

    last_backup_state_  = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refresh_sync_status();
}

void MainWindow::push_room_list_state(tesseract::RoomListState state) {
    last_room_list_state_ = state;
    refresh_sync_status();
}

gboolean MainWindow::on_sync_status_debounce_(gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->sync_status_debounce_id_ = 0;
    using RLS = tesseract::RoomListState;
    if (self->status_bar_
     && (self->last_room_list_state_ == RLS::Init
      || self->last_room_list_state_ == RLS::SettingUp))
    {
        self->sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(self->status_bar_),
                           _("Syncing rooms\xe2\x80\xa6"));
    }
    return G_SOURCE_REMOVE;
}

void MainWindow::refresh_sync_status() {
    if (!status_bar_) return;
    using RLS = tesseract::RoomListState;
    using BS  = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init
                         || last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy) {
        if (!sync_progress_shown_ && sync_status_debounce_id_ == 0)
            sync_status_debounce_id_ = g_timeout_add(300, on_sync_status_debounce_, this);
        else if (sync_progress_shown_)
            gtk_label_set_text(GTK_LABEL(status_bar_),
                               _("Syncing rooms\xe2\x80\xa6"));
        return;
    }

    if (sync_status_debounce_id_ != 0) {
        g_source_remove(sync_status_debounce_id_);
        sync_status_debounce_id_ = 0;
    }

    if (reconnecting) {
        sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(status_bar_),
                           _("Reconnecting\xe2\x80\xa6"));
        return;
    }
    if (keys_busy) {
        sync_progress_shown_ = true;
        std::string msg = std::string(_("Downloading encryption keys ("))
                        + std::to_string(last_imported_keys_)
                        + ")\xe2\x80\xa6";
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        return;
    }
    if (sync_progress_shown_) {
        sync_progress_shown_ = false;
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
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
                       res ? _("Signed out")
                           : (std::string(_("Sign out failed: ")) + res.message).c_str());

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
    emoji_picker_search_field_->set_placeholder(_("Search emoji"));
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

// ---------------------------------------------------------------------------
// Sticker picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::StickerPicker. Mirrors the emoji popover.
// ---------------------------------------------------------------------------

void MainWindow::build_sticker_popover() {
    sticker_popover_ = gtk_popover_new();
    gtk_widget_set_parent(sticker_popover_, compose_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(sticker_popover_), TRUE);

    sticker_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(&client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img) {
            if (current_room_id_.empty()) return;
            std::string body = img.body.empty() ? img.shortcode : img.body;
            client_.send_sticker(current_room_id_, body, img.url, img.info_json);
            if (sticker_popover_)
                gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        };
    // Share the same caches the message list reads from. Animated
    // entries take priority; static entries are the second hop; on
    // miss kick off an async fetch via `ensure_sticker_image_async`
    // so the next paint after the worker posts back finds the bitmap.
    sticker_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            auto ait = tk_anim_images_.find(cache_key);
            if (ait != tk_anim_images_.end() && !ait->second.frames.empty()) {
                return ait->second.frames[ait->second.current].get();
            }
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_sticker_image_async(cache_key);
            return nullptr;
        });
    sticker_picker_surface_->set_root(std::move(shared));

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder(_("Search stickers"));
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q) {
            if (sticker_picker_shared_) sticker_picker_shared_->set_search_query(q);
            if (sticker_picker_surface_) sticker_picker_surface_->relayout();
        });
    sticker_picker_surface_->set_on_layout([this] {
        if (sticker_picker_search_field_ && sticker_picker_shared_) {
            sticker_picker_search_field_->set_rect(
                sticker_picker_shared_->search_field_rect());
        }
    });

    GtkWidget* surface_widget = sticker_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 360, 420);
    gtk_popover_set_child(GTK_POPOVER(sticker_popover_), surface_widget);
}

void MainWindow::toggle_sticker_picker() {
    if (!sticker_popover_) return;
    if (gtk_widget_get_visible(sticker_popover_)) {
        gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        return;
    }
    GtkWidget* desired_parent =
        compose_surface_ ? compose_surface_->widget() : nullptr;
    if (desired_parent &&
        gtk_widget_get_parent(sticker_popover_) != desired_parent) {
        gtk_widget_unparent(sticker_popover_);
        gtk_widget_set_parent(sticker_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), nullptr);
    if (sticker_picker_shared_) sticker_picker_shared_->refresh_packs();
    if (sticker_picker_search_field_) sticker_picker_search_field_->set_text("");
    if (sticker_picker_shared_) sticker_picker_shared_->set_search_query("");
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_) sticker_picker_surface_->relayout();
}

// ---------------------------------------------------------------------------
// Sticker context menu — right-click on a sticker row offers
// "Add to Saved Stickers" (suppressed for stickers already saved).
// ---------------------------------------------------------------------------

void MainWindow::build_sticker_context_menu() {
    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Add to Saved Stickers"), "sticker.save");

    sticker_ctx_menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_ctx_menu_), FALSE);
    gtk_widget_set_parent(sticker_ctx_menu_, msg_surface_->widget());
    g_object_unref(menu);

    sticker_ctx_actions_ = g_simple_action_group_new();
    GSimpleAction* save = g_simple_action_new("save", nullptr);
    g_signal_connect(save, "activate",
                     G_CALLBACK(on_sticker_save_activate_), this);
    g_action_map_add_action(G_ACTION_MAP(sticker_ctx_actions_),
                            G_ACTION(save));
    g_object_unref(save);
    gtk_widget_insert_action_group(msg_surface_->widget(), "sticker",
                                    G_ACTION_GROUP(sticker_ctx_actions_));
}

void MainWindow::on_msg_right_click_(GtkGestureClick* gesture,
                                      int /*n_press*/,
                                      double x, double y,
                                      gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->message_list_view_ || !self->sticker_ctx_menu_) return;

    // Surface coordinates equal MessageListView-local coordinates because
    // the view is the surface's root widget.
    auto hit = self->message_list_view_->sticker_hit_at(
        tk::Point{ static_cast<float>(x), static_cast<float>(y) });
    if (!hit) return;
    if (self->client_.user_pack_has_sticker(hit->mxc_url)) return;

    // Claim the gesture so the underlying surface doesn't also process it
    // (e.g. as a drag-start or text-selection event).
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Capture sticker fields for the action handler. The hit_at result
    // points into MessageListView's per-frame sticker_geom_ map and would
    // dangle by the time the action fires.
    self->ctx_sticker_event_id_ = hit->event_id;
    self->ctx_sticker_mxc_url_  = hit->mxc_url;
    self->ctx_sticker_body_     = hit->body;

    GdkRectangle r{
        .x      = static_cast<int>(x),
        .y      = static_cast<int>(y),
        .width  = 1,
        .height = 1
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->sticker_ctx_menu_), &r);
    gtk_popover_popup(GTK_POPOVER(self->sticker_ctx_menu_));
}

gboolean MainWindow::on_window_key_pressed_(GtkEventControllerKey*,
                                              guint keyval, guint,
                                              GdkModifierType,
                                              gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (keyval == GDK_KEY_Escape) {
        if (self->vid_viewer_ && self->vid_viewer_->is_open()) {
            self->vid_viewer_->close(); return TRUE;
        }
        if (self->img_viewer_ && self->img_viewer_->is_open()) {
            self->img_viewer_->close(); return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::on_sticker_save_activate_(GSimpleAction* /*action*/,
                                            GVariant* /*parameter*/,
                                            gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->ctx_sticker_mxc_url_.empty()) return;
    self->client_.save_sticker_to_user_pack(
        self->ctx_sticker_body_,    // shortcode hint (slugified by SDK)
        self->ctx_sticker_body_,    // body
        self->ctx_sticker_mxc_url_,
        "{}");                      // info_json: SDK preserves the original
                                    // event's info via the local pack cache
                                    // when the round-trip completes.
    self->ctx_sticker_event_id_.clear();
    self->ctx_sticker_mxc_url_.clear();
    self->ctx_sticker_body_.clear();
    if (self->sticker_ctx_menu_)
        gtk_popover_popdown(GTK_POPOVER(self->sticker_ctx_menu_));
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
