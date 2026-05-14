#include "MainWindow.h"
#include "LoginView.h"

#include "tk/canvas_cairo.h"
#include "tk/theme.h"
#include "views/markdown.h"

#include <cairo.h>
#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
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
// g_idle_add helpers (for async workers that are NOT part of EventHandlerBase)
// ---------------------------------------------------------------------------

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
// EventHandlerBase UI-thread hook implementations (GTK4)
// ---------------------------------------------------------------------------

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    push_timeline_reset(std::move(room_id), std::move(snapshot));
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    push_message_inserted(std::move(room_id), index, std::move(ev));
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    push_message_updated(std::move(room_id), index, std::move(ev));
}

void MainWindow::handle_message_removed_ui_(
    std::string room_id, std::size_t index)
{
    push_message_removed(std::move(room_id), index);
}

void MainWindow::handle_sync_error_ui_(
    std::string context, std::string user_id,
    std::string description, bool soft_logout)
{
    if (context == "sync_reconnect")
        handle_reconnect(user_id);
    else if (context == "sync_auth_error")
        handle_auth_error(soft_logout);
    else
        push_error(std::move(description));
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    push_backup_progress(std::move(progress));
}

void MainWindow::handle_image_packs_updated_ui_()
{
    push_image_packs_updated();
}

void MainWindow::handle_account_prefs_updated_ui_(
    std::string /*user_id*/, std::string json)
{
    push_account_prefs_updated(json);
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id,
    std::string room_name, std::string sender,
    std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes)
{
    push_notification(user_id, room_id, room_name, sender, body, is_mention,
                      std::move(avatar_bytes));
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
}

void MainWindow::update_typing_bar_(const std::string& text, bool visible)
{
    if (typing_bar_) {
        gtk_label_set_text(GTK_LABEL(typing_bar_), text.c_str());
        gtk_widget_set_visible(typing_bar_, visible);
    }
}

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!verif_surface_) return;
    GtkWidget* w = verif_surface_->widget();
    if (is_verified) {
        gtk_widget_set_visible(w, FALSE);
        return;
    }
    if (verification_banner_dismissed_) return;
    if (!gtk_widget_get_visible(w)) {
        active_verification_flow_id_.clear();
        if (verif_shared_)
            verif_shared_->set_state(
                tesseract::views::VerificationBanner::State::Prompt);
        gtk_widget_set_size_request(w, -1, 48);
        gtk_widget_set_visible(w, TRUE);
        verif_surface_->relayout();
    }
}

void MainWindow::handle_verification_request_ui_(
    std::string flow_id, std::string /*user_id*/,
    std::string /*device_id*/, bool incoming)
{
    if (!verif_surface_ || !verif_shared_) return;
    active_verification_flow_id_ = flow_id;
    if (incoming) {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    } else {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_) client_->start_sas(flow_id);
    }
    GtkWidget* w = verif_surface_->widget();
    gtk_widget_set_size_request(w, -1, 48);
    gtk_widget_set_visible(w, TRUE);
    verif_surface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!verif_surface_ || !verif_shared_) return;
    verif_shared_->set_emojis(emojis);
    GtkWidget* w = verif_surface_->widget();
    gtk_widget_set_size_request(w, -1, 124);
    gtk_widget_set_visible(w, TRUE);
    verif_surface_->relayout();
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    if (!verif_surface_ || !verif_shared_) return;
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Done);
    GtkWidget* w = verif_surface_->widget();
    gtk_widget_set_size_request(w, -1, 48);
    verif_surface_->relayout();
    // Hide after 1.5 s.
    g_timeout_add(1500, [](gpointer data) -> gboolean {
        auto* self = static_cast<MainWindow*>(data);
        if (self->verif_shared_ && self->verif_shared_->on_done)
            self->verif_shared_->on_done();
        return G_SOURCE_REMOVE;
    }, this);
}

void MainWindow::handle_verification_cancelled_ui_(
    std::string /*flow_id*/, std::string reason)
{
    if (!verif_surface_ || !verif_shared_) return;
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Cancelled);
    verif_shared_->set_cancel_reason(std::move(reason));
    GtkWidget* w = verif_surface_->widget();
    gtk_widget_set_size_request(w, -1, 48);
    gtk_widget_set_visible(w, TRUE);
    verif_surface_->relayout();
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

    login_view_ = std::make_unique<LoginView>();
    login_view_->set_on_success([this]() { on_login_succeeded(); });
    login_view_->set_on_cancel([this]() { on_login_cancelled(); });
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
    room_list_view_->on_scroll = [this] {
        if (scroll_debounce_id_) {
            g_source_remove(scroll_debounce_id_);
            scroll_debounce_id_ = 0;
        }
        scroll_debounce_id_ = g_timeout_add(300, [](gpointer ud) -> gboolean {
            auto* self = static_cast<MainWindow*>(ud);
            self->scroll_debounce_id_ = 0;
            if (!self->room_list_view_ || !self->client_) return G_SOURCE_REMOVE;
            auto ids = self->room_list_view_->visible_room_ids();
            self->client_->stop_background_backfill();
            self->client_->start_background_backfill(ids);
            return G_SOURCE_REMOVE;
        }, this);
    };
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
            self->refresh_room_list();
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
    room_list_view_->on_search_clear = [this] {
        if (search_debounce_id_) {
            g_source_remove(search_debounce_id_);
            search_debounce_id_ = 0;
        }
        search_pending_text_.clear();
        room_search_field_->set_text("");
        room_list_view_->set_search_text("");
        refresh_room_list();
    };

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

        // Name + Matrix ID stacked vertically.
        GtkWidget* name_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(name_vbox, TRUE);

        user_name_lbl_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(user_name_lbl_), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(user_name_lbl_), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(name_vbox), user_name_lbl_);

        user_id_lbl_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(user_id_lbl_), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(user_id_lbl_), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(user_id_lbl_, "timestamp");  // smaller, muted
        gtk_box_append(GTK_BOX(name_vbox), user_id_lbl_);

        gtk_box_append(GTK_BOX(user_strip_), name_vbox);

        // Left-click → account picker (only when ≥2 accounts).
        GtkGesture* lclick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lclick), GDK_BUTTON_PRIMARY);
        g_signal_connect(lclick, "pressed",
                         G_CALLBACK(on_user_strip_left_click_), this);
        gtk_widget_add_controller(user_strip_, GTK_EVENT_CONTROLLER(lclick));

        // Right-click gesture → popover menu.
        GtkGesture* gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(on_user_strip_right_click_), this);
        gtk_widget_add_controller(user_strip_, GTK_EVENT_CONTROLLER(gesture));

        // Build the GMenu model + GSimpleActionGroup once.
        GMenu* menu = g_menu_new();
        g_menu_append(menu, _("Add Account\xe2\x80\xa6"), "user.add_account");
        g_menu_append(menu, _("Log Out"), "user.logout");
        user_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        gtk_widget_set_parent(user_popover_, user_strip_);
        gtk_popover_set_has_arrow(GTK_POPOVER(user_popover_), FALSE);
        g_object_unref(menu);

        GSimpleActionGroup* group = g_simple_action_group_new();
        {
            GSimpleAction* act = g_simple_action_new("add_account", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_add_account_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("logout", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_logout_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
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

    // Verification banner — inline strip shown when the device is unverified.
    verif_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::VerificationBanner>();
        verif_shared_ = banner.get();
        verif_shared_->on_verify = [this] {
            if (client_) client_->request_self_verification();
        };
        verif_shared_->on_accept = [this] {
            if (client_ && !active_verification_flow_id_.empty()) {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_match = [this] {
            if (client_ && !active_verification_flow_id_.empty()) {
                if (verif_shared_)
                    verif_shared_->set_state(
                        tesseract::views::VerificationBanner::State::Confirming);
                verif_surface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_mismatch = [this] {
            if (client_ && !active_verification_flow_id_.empty())
                client_->cancel_verification(active_verification_flow_id_);
        };
        verif_shared_->on_cancel = [this] {
            if (client_ && !active_verification_flow_id_.empty())
                client_->cancel_verification(active_verification_flow_id_);
        };
        verif_shared_->on_dismiss = [this] {
            verification_banner_dismissed_ = true;
            if (verif_surface_)
                gtk_widget_set_visible(verif_surface_->widget(), FALSE);
        };
        verif_shared_->on_done = [this] {
            if (verif_surface_)
                gtk_widget_set_visible(verif_surface_->widget(), FALSE);
        };
        verif_surface_->set_root(std::move(banner));
    }
    {
        GtkWidget* w = verif_surface_->widget();
        gtk_widget_set_size_request(w, -1, 48);
        gtk_widget_set_visible(w, FALSE);
        gtk_box_append(GTK_BOX(vbox), w);
    }

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
            // Animated entries first; static cache is the second hop.
            if (const auto* f = anim_cache_.current_frame(mxc)) return f;
            auto it = tk_images_.find(mxc);
            return it == tk_images_.end() ? nullptr : it->second.get();
        });
    message_list_view_->set_preview_provider(
        [this](const std::string& url) -> const tesseract::views::UrlPreviewData* {
            auto it = url_preview_data_.find(url);
            if (it == url_preview_data_.end()) return nullptr;
            if (!it->second.image_mxc.empty()
                && !tk_images_.count(it->second.image_mxc)
                && !anim_cache_.has(it->second.image_mxc))
                ensure_media_image_(it->second.image_mxc, 64, 64);
            return &it->second;
        });
    message_list_view_->on_link_clicked = [](const std::string& url) {
        tesseract::Client::open_in_browser(url);
    };
    // Voice (MSC3245) playback — GStreamer playbin via tk::gtk4::Host.
    if (auto player = msg_surface_->host().make_audio_player()) {
        message_list_view_->set_audio_player(std::move(player));
    }
    message_list_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return client_->fetch_source_bytes(source_json);
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

    typing_bar_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(typing_bar_), 0.0f);
    gtk_widget_set_margin_start(typing_bar_, 8);
    gtk_widget_set_size_request(typing_bar_, -1, 20);
    gtk_box_append(GTK_BOX(vbox), typing_bar_);

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
        handle_compose_text_changed_(s);
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
        const auto limit = client_->media_upload_limit();
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
        auto md = tesseract::views::markdown_to_html(trimmed);
        auto res = client_->send_message(current_room_id_, trimmed, md.formatted_body);
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
        auto res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
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
        auto res = client_->send_file(current_room_id_, bytes, mime,
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
        auto md = tesseract::views::markdown_to_html(body);
        auto res = client_->send_reply(current_room_id_, reply_event_id, body, md.formatted_body);
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
            client_->send_reaction(current_room_id_, event_id, key);
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
        auto md = tesseract::views::markdown_to_html(new_body);
        auto res = client_->send_edit(current_room_id_, event_id, new_body, md.formatted_body);
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

    message_list_view_->on_receipt_needed = [this](const std::string& eid) {
        maybe_send_read_receipt_(current_room_id_, eid);
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
                if (const auto* f = anim_cache_.current_frame(url)) return f;
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
                             hit.duration_ms, hit.natural_w, hit.natural_h,
                             hit.autoplay, hit.loop, hit.no_audio, hit.hide_controls);
            gtk_widget_set_visible(vid_viewer_surface_->widget(), TRUE);
            gtk_widget_grab_focus(vid_viewer_surface_->widget());
            // Async byte fetch on a detached thread.
            std::string src = hit.source_json;
            run_async_([this, src = std::move(src)]() mutable {
                auto bytes = client_->fetch_source_bytes(src);
                struct Ctx { MainWindow* self; std::vector<uint8_t> bytes; };
                auto* ctx = new Ctx{ this, std::move(bytes) };
                g_idle_add([](gpointer p) -> gboolean {
                    auto* c = static_cast<Ctx*>(p);
                    if (c->self->vid_viewer_)
                        c->self->vid_viewer_->load_bytes(c->bytes.data(), c->bytes.size());
                    delete c;
                    return G_SOURCE_REMOVE;
                }, ctx);
            });
        };

    message_list_view_->set_video_player_factory(
        [this]() { return msg_surface_->host().make_video_player(); });
    message_list_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready) {
            run_async_([this, src, on_ready = std::move(on_ready)]() mutable {
                auto bytes = client_->fetch_source_bytes(src);
                struct Ctx {
                    std::function<void(std::vector<std::uint8_t>)> cb;
                    std::vector<std::uint8_t> bytes;
                };
                auto* ctx = new Ctx{ std::move(on_ready), std::move(bytes) };
                g_idle_add([](gpointer p) -> gboolean {
                    auto* c = static_cast<Ctx*>(p);
                    c->cb(std::move(c->bytes));
                    delete c;
                    return G_SOURCE_REMOVE;
                }, ctx);
            });
        });

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

    // Notifiers are created per-account in do_login / on_login_succeeded.

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
    if (scroll_debounce_id_) {
        g_source_remove(scroll_debounce_id_);
        scroll_debounce_id_ = 0;
    }
    // Drain background workers BEFORE tearing the client down. Each
    // worker calls `client_->fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    // Order: flip the flag → wait (bounded) for in-flight ones →
    // only then stop_sync + destroy members.
    shutting_down_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                              [this]{ return workers_in_flight_ == 0; });
    }
    // Stop sync on all accounts before any client is destroyed.
    for (auto& sess : accounts_)
        if (sess->sync_started) sess->client->stop_sync();
    // login_view_ holds pending_login_client_* — cancel + join its worker
    // before we destroy pending_login_client_ and the accounts vector.
    login_view_.reset();
    pending_login_client_.reset();
}

// ---------------------------------------------------------------------------

void MainWindow::do_login() {
    tesseract::SessionStore::migrate_legacy_layout();

    auto index = tesseract::SessionStore::load_index();
    if (!index.user_ids.empty()) {
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Restoring session\xe2\x80\xa6"));
        int first_active = -1;
        for (const auto& uid : index.user_ids) {
            auto saved = tesseract::SessionStore::load_account(uid);
            if (!saved) continue;

            auto sess = std::make_unique<tesseract::AccountSession>();
            sess->client = std::make_unique<tesseract::Client>();
            sess->client->set_data_dir(
                tesseract::SessionStore::sdk_store_dir(uid).string());
            auto res = sess->client->restore_session(*saved);
            if (!res) {
                tesseract::SessionStore::clear_account(uid);
                continue;
            }
            sess->user_id      = sess->client->get_user_id();
            sess->display_name = sess->client->get_display_name();
            sess->avatar_url   = sess->client->get_avatar_url();
            sess->last_room    =
                tesseract::Prefs::parse(
                    sess->client->load_prefs_json()).last_room;

            auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
            bridge->set_user_id(sess->user_id);
            sess->client->start_sync(bridge.get());
            sess->bridge       = std::move(bridge);
            sess->sync_started = true;

            // Per-account notifier: click switches to this account then navigates.
            const std::string notif_uid = sess->user_id;
            sess->notifier = std::make_unique<LinuxNotifierGtk>(
                [this, notif_uid](std::string room_id) {
                    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                        if (accounts_[i]->user_id == notif_uid) {
                            switch_active_account(i); break;
                        }
                    }
                    navigate_to_room(std::move(room_id));
                });

            // Per-account UnifiedPush connector.
            {
                auto up = std::make_unique<LinuxUpConnectorGtk>();
                up->start(sess->client.get(), sess->user_id);
                sess->up_connector = std::move(up);
            }

            int idx = static_cast<int>(accounts_.size());
            if (uid == index.active_user_id) first_active = idx;
            accounts_.push_back(std::move(sess));
        }

        if (!accounts_.empty()) {
            if (first_active < 0) first_active = 0;
            switch_active_account(first_active);
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            maybe_show_recovery_banner();
            start_tray_if_needed_();
            return;
        }
    }

    // No accounts: fresh install or all restores failed → show login view.
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pending_login_temp_dir_ =
        tesseract::SessionStore::account_dir("pending-" + std::to_string(ts));
    pending_login_client_ = std::make_unique<tesseract::Client>();
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());
    pending_login_is_add_account_ = false;

    login_view_->set_client(pending_login_client_.get());
    login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
    login_view_->reset();
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Not logged in"));
}

void MainWindow::on_login_succeeded() {
    // Export session before dropping the in-flight client.
    std::string uid      = pending_login_client_->get_user_id();
    std::string exported = pending_login_client_->export_session();

    // Drop the in-flight client to release SQLite handles before rename.
    pending_login_client_.reset();

    // Rename temp dir → final per-account dir.
    auto target = tesseract::SessionStore::account_dir(uid);
    std::error_code ec;
    std::filesystem::rename(pending_login_temp_dir_, target, ec);
    if (ec) {
        std::filesystem::copy(pending_login_temp_dir_, target,
            std::filesystem::copy_options::recursive, ec);
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
    }
    pending_login_temp_dir_.clear();

    tesseract::SessionStore::save_account(uid, exported);

    // Reopen the store at the final path.
    auto sess = std::make_unique<tesseract::AccountSession>();
    sess->client = std::make_unique<tesseract::Client>();
    sess->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(uid).string());
    auto res = sess->client->restore_session(exported);
    if (!res) {
        gtk_label_set_text(GTK_LABEL(status_bar_),
            (std::string(_("Login error: ")) + res.message).c_str());
        return;
    }
    sess->user_id      = sess->client->get_user_id();
    sess->display_name = sess->client->get_display_name();
    sess->avatar_url   = sess->client->get_avatar_url();
    sess->last_room    =
        tesseract::Prefs::parse(sess->client->load_prefs_json()).last_room;

    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(sess->user_id);
    sess->client->start_sync(bridge.get());
    sess->bridge       = std::move(bridge);
    sess->sync_started = true;

    // Per-account notifier: click switches to this account then navigates.
    const std::string notif_uid = sess->user_id;
    sess->notifier = std::make_unique<LinuxNotifierGtk>(
        [this, notif_uid](std::string room_id) {
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                if (accounts_[i]->user_id == notif_uid) {
                    switch_active_account(i); break;
                }
            }
            navigate_to_room(std::move(room_id));
        });

    // Per-account UnifiedPush connector.
    {
        auto up = std::make_unique<LinuxUpConnectorGtk>();
        up->start(sess->client.get(), sess->user_id);
        sess->up_connector = std::move(up);
    }

    int new_idx = static_cast<int>(accounts_.size());
    accounts_.push_back(std::move(sess));

    // Update accounts.json index.
    auto index = tesseract::SessionStore::load_index();
    if (std::find(index.user_ids.begin(), index.user_ids.end(), uid)
            == index.user_ids.end())
        index.user_ids.push_back(uid);
    index.active_user_id = uid;
    tesseract::SessionStore::save_index(index);

    switch_active_account(new_idx);
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

    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id)
        client_->unsubscribe_room(current_room_id_);

    current_room_id_ = room_id;
    mark_room_read_(current_room_id_);
    update_typing_bar_({}, false);
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (compose_shared_) {
        compose_shared_->clear_reply();
        compose_shared_->clear_editing();
    }
    if (compose_text_area_) compose_text_area_->set_text("");
    if (compose_shared_)    compose_shared_->set_current_text({});

    for (const auto& r : rooms_)
        if (r.id == current_room_id_) { update_room_header(r); break; }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the GTK main loop stays responsive.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    std::string sub_room = current_room_id_;
    run_async_([this, sub_room, visible_ids = std::move(visible_ids)]{
        auto res = client_->subscribe_room(sub_room);
        bool reached = false;
        if (res) {
            auto pr = client_->paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            client_->start_background_backfill(visible_ids);
        }
        auto* d = new IdleSubscribeResult{this, sub_room, reached};
        g_idle_add([](gpointer data) -> gboolean {
            auto* dd = static_cast<IdleSubscribeResult*>(data);
            dd->window->push_subscribe_result(std::move(dd->room_id),
                                               dd->reached_start);
            delete dd;
            return G_SOURCE_REMOVE;
        }, d);
    });
}

void MainWindow::push_paginate_result(std::string room_id, bool reached_start) {
    bool is_current = (room_id == current_room_id_);
    push_paginate_result_(std::move(room_id), reached_start);
    if (is_current && message_list_view_)
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
    run_async_([this, room_id]{
        auto pr = client_->paginate_back_with_status(room_id, kPaginationBatch);
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
    });
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
    ensure_row_media_(*ev);
    ensure_reply_details_(ev->in_reply_to_id);
    message_list_view_->insert_message(index, tesseract::views::make_row_data(*ev, my_user_id_));
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
    ensure_row_media_(*ev);
    ensure_reply_details_(ev->in_reply_to_id);
    message_list_view_->update_message(index, tesseract::views::make_row_data(*ev, my_user_id_));
    msg_surface_->relayout();
}

void MainWindow::push_message_removed(std::string room_id, std::size_t index) {
    if (room_id != current_room_id_) return;
    message_list_view_->remove_message(index);
    msg_surface_->relayout();
}

void MainWindow::push_rooms(std::string user_id,
                            std::vector<tesseract::RoomInfo> rooms) {
    push_rooms_(std::move(user_id), std::move(rooms));
}

void MainWindow::on_rooms_updated_() {
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

void MainWindow::handle_reconnect(const std::string& user_id) {
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Sync error: reconnecting\xe2\x80\xa6"));
    for (auto& sess : accounts_) {
        if (sess->user_id == user_id && sess->client) {
            sess->client->stop_sync();
            sess->sync_started = false;
            break;
        }
    }
    // Restart the affected account's sync after a short delay.  do not
    // call do_login() (which rebuilds all sessions), as that causes a tight
    // loop when the server rejects key uploads on every new session.
    struct DelayData { MainWindow* w; std::string uid; };
    auto* dd = new DelayData{ this, user_id };
    g_timeout_add(5000, [](gpointer data) -> gboolean {
        auto* d = static_cast<DelayData*>(data);
        for (auto& s : d->w->accounts_) {
            if (s->user_id == d->uid && !s->sync_started && s->client) {
                s->sync_started = true;
                s->client->start_sync(s->bridge.get());
            }
        }
        delete d;
        return G_SOURCE_REMOVE;
    }, dd);
}

void MainWindow::handle_auth_error(bool soft_logout) {
    if (soft_logout && active_account_index_ >= 0) {
        const std::string& uid = accounts_[active_account_index_]->user_id;
        if (auto saved = tesseract::SessionStore::load_account(uid)) {
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Reconnecting session\xe2\x80\xa6"));
            if (client_->restore_session(*saved)) {
                my_user_id_       = client_->get_user_id();
                my_display_name_  = client_->get_display_name();
                my_avatar_url_    = client_->get_avatar_url();
                populate_user_strip();
                client_->start_sync(event_handler_);
                gtk_label_set_text(GTK_LABEL(status_bar_), _("Reconnected"));
                maybe_show_recovery_banner();
                return;
            }
        }
    }
    if (active_account_index_ >= 0)
        tesseract::SessionStore::clear_account(
            accounts_[active_account_index_]->user_id);
    if (client_) client_->stop_sync();
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
        ensure_row_media_(*ev);
        ensure_reply_details_(ev->in_reply_to_id);
        rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
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
            avatar_cache_[info.avatar_url] = client_->fetch_avatar_bytes(info.id);
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

void MainWindow::start_anim_tick_if_needed_() {
    if (tk_anim_tick_id_ != 0) return;
    if (anim_cache_.empty()) return;
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
    if (self->anim_cache_.empty()) {
        self->tk_anim_tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    const std::int64_t now_ms = g_get_monotonic_time() / 1000;
    if (self->anim_cache_.advance(now_ms))
        self->invalidate_anim_consumers_();
    return G_SOURCE_CONTINUE;
}

void MainWindow::ensure_sticker_image_async(std::string url) {
    if (url.empty() || tk_images_.count(url) || anim_cache_.has(url))
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
    run_async_([this, url]() mutable {
        auto bytes = client_->fetch_source_bytes(url);
        auto* data = new IdleData{ this, std::move(url), std::move(bytes) };
        g_idle_add([](gpointer p) -> gboolean {
            auto* d = static_cast<IdleData*>(p);
            d->self->sticker_fetches_in_flight_.erase(d->url);
            if (!d->bytes.empty()
                && !d->self->tk_images_.count(d->url)
                && !d->self->anim_cache_.has(d->url))
            {
                // Probe for animation first; animated stickers ride the
                // frame-tick loop. Static formats fall through to tk_images_.
                if (auto anim = decode_animation(d->bytes)) {
                    std::vector<std::unique_ptr<tk::Image>> frames;
                    frames.reserve(anim->frames.size());
                    for (cairo_surface_t* s : anim->frames) {
                        frames.push_back(tk::cairo_pango::make_image(s));
                        cairo_surface_destroy(s);
                    }
                    if (!frames.empty()) {
                        const gint64 now_ms = g_get_monotonic_time() / 1000;
                        d->self->anim_cache_.store(d->url, std::move(frames),
                                                   std::move(anim->delays_ms),
                                                   now_ms);
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
    });
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
    for (const auto& r : sorted) ensure_room_avatar_(r);

    room_list_view_->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
        room_list_view_->set_selected_room(current_room_id_);
    room_surface_->relayout();
}

void MainWindow::refresh_room_list() {
    if (space_stack_.empty()) {
        if (!search_pending_text_.empty()) {
            show_rooms(rooms_);
            gtk_widget_set_visible(room_nav_bar_, FALSE);
            return;
        }
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_) {
            if (!r.is_space) continue;
            for (const auto& id : client_->space_children(r.id))
                in_space.insert(id);
        }
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
        for (const auto& r : rooms_)
            if ( r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
        show_rooms(filtered);
        gtk_widget_set_visible(room_nav_bar_, FALSE);
    } else {
        const std::string& space_id = space_stack_.back();
        auto child_ids = client_->space_children(space_id);
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
//  GTK4-specific ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::post_to_ui_(std::function<void()> fn) {
    struct Data { std::function<void()> fn; };
    auto* d = new Data{ std::move(fn) };
    g_idle_add([](gpointer p) -> gboolean {
        auto* data = static_cast<Data*>(p);
        data->fn();
        delete data;
        return G_SOURCE_REMOVE;
    }, d);
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                       MediaKind kind,
                                       std::vector<uint8_t> bytes) {
    if (bytes.empty()) return;
    if (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar) {
        if (tk_avatars_.count(cache_key)) return;
        if (cairo_surface_t* surface = decode_image_to_cairo_surface(bytes)) {
            auto img = tk::cairo_pango::make_image(surface);
            cairo_surface_destroy(surface);
            tk_avatars_.emplace(cache_key, std::move(img));
            if (kind == MediaKind::RoomAvatar && room_surface_)
                room_surface_->relayout();
            else if (msg_surface_)
                msg_surface_->relayout();
        }
    } else { // MediaImage
        if (tk_images_.count(cache_key) || anim_cache_.has(cache_key)) return;
        if (auto anim = decode_animation(bytes)) {
            std::vector<std::unique_ptr<tk::Image>> frames;
            frames.reserve(anim->frames.size());
            for (cairo_surface_t* s : anim->frames) {
                frames.push_back(tk::cairo_pango::make_image(s));
                cairo_surface_destroy(s);
            }
            if (!frames.empty()) {
                const gint64 now_ms = g_get_monotonic_time() / 1000;
                anim_cache_.store(cache_key, std::move(frames),
                                  std::move(anim->delays_ms), now_ms);
                start_anim_tick_if_needed_();
                if (msg_surface_) msg_surface_->relayout();
            }
        } else if (cairo_surface_t* surface = decode_image_to_cairo_surface(bytes)) {
            auto img = tk::cairo_pango::make_image(surface);
            cairo_surface_destroy(surface);
            tk_images_.emplace(cache_key, std::move(img));
            if (msg_surface_) msg_surface_->relayout();
        }
    }
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& video_url) {
    const std::string eid = event_id;
    run_async_([this, eid, src = video_url]() mutable {
        auto bytes = client_->fetch_source_bytes(src);
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
        GstBuffer* buf   = gst_sample_get_buffer(sample);
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
    });
}

void MainWindow::on_url_preview_ready_(const std::string& url,
                                        const tesseract::Client::UrlPreview& preview) {
    tesseract::views::UrlPreviewData d;
    d.title       = preview.title;
    d.description = preview.description;
    d.image_mxc   = preview.image_mxc;
    d.image_w     = preview.image_w;
    d.image_h     = preview.image_h;
    url_preview_data_.emplace(url, std::move(d));

    if (!preview.image_mxc.empty())
        ensure_media_image_(preview.image_mxc, 64, 64);

    if (message_list_view_) message_list_view_->invalidate_data();
    if (msg_surface_) msg_surface_->relayout();
}

// ---------------------------------------------------------------------------

void MainWindow::maybe_show_recovery_banner() {
    if (recovery_banner_dismissed_) return;
    if (!client_->needs_recovery()) return;
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
    self->run_async_([self, key]() {
        auto res = self->client_->recover(key);
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
    });
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
        const std::string& user_id,
        const std::string& room_id, const std::string& room_name,
        const std::string& sender, const std::string& body, bool is_mention,
        std::vector<uint8_t> avatar_bytes)
{
    handle_notification(user_id, room_id, room_name, sender, body,
                        is_mention, std::move(avatar_bytes));
}

void MainWindow::handle_notification(
        const std::string& user_id,
        const std::string& room_id, const std::string& room_name,
        const std::string& sender,  const std::string& body, bool is_mention,
        std::vector<uint8_t> avatar_bytes)
{
    for (auto& sess : accounts_) {
        if (sess->user_id != user_id) continue;
        // Suppress only when this account is active and its room is already open.
        if (gtk_window_is_active(GTK_WINDOW(window_))
                && active_account_index_ >= 0
                && accounts_[active_account_index_]->user_id == user_id
                && current_room_id_ == room_id)
            return;
        if (sess->notifier) {
            tesseract::Notification n;
            n.room_id      = room_id;
            n.room_name    = room_name;
            n.sender       = sender;
            n.body         = body;
            n.is_mention   = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            sess->notifier->notify(n);
        }
        return;
    }
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
        && !client_->needs_recovery()
        && recovery_surface_)
    {
        gtk_widget_set_visible(recovery_surface_->widget(), FALSE);
    }

    last_backup_state_  = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refresh_sync_status();
}

void MainWindow::push_room_list_state(tesseract::RoomListState state) {
    push_room_list_state_(state);
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
    gtk_label_set_text(GTK_LABEL(user_id_lbl_), my_user_id_.c_str());

    bool has_avatar = false;
    if (!my_avatar_url_.empty() && client_) {
        auto bytes = client_->fetch_media_bytes(my_avatar_url_);
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

void MainWindow::on_add_account_activate_(GSimpleAction* /*action*/,
                                          GVariant* /*parameter*/,
                                          gpointer user_data) {
    gtk_popover_popdown(GTK_POPOVER(
        static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->begin_add_account();
}

void MainWindow::on_logout_activate_(GSimpleAction* /*action*/,
                                     GVariant* /*parameter*/,
                                     gpointer user_data) {
    gtk_popover_popdown(GTK_POPOVER(
        static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->logout_active_account();
}

void MainWindow::do_logout() {
    logout_active_account();
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
    emoji_picker_shared_->set_client(client_);
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
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img) {
            if (current_room_id_.empty()) return;
            std::string body = img.body.empty() ? img.shortcode : img.body;
            client_->send_sticker(current_room_id_, body, img.url, img.info_json);
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
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
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
    if (self->client_->user_pack_has_sticker(hit->mxc_url)) return;

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
    self->client_->save_sticker_to_user_pack(
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
            client_->send_reaction(current_room_id_, ev, glyph);
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

// ---------------------------------------------------------------------------
// Multi-account management
// ---------------------------------------------------------------------------

void MainWindow::switch_active_account(int new_idx) {
    active_account_index_ = new_idx;
    auto& sess = *accounts_[new_idx];

    client_        = sess.client.get();
    event_handler_ = sess.bridge.get();

    my_user_id_      = sess.user_id;
    my_display_name_ = sess.display_name;
    my_avatar_url_   = sess.avatar_url;
    pending_restore_room_ = sess.last_room;

    populate_user_strip();

    if (emoji_picker_shared_)   emoji_picker_shared_->set_client(client_);
    if (sticker_picker_shared_) sticker_picker_shared_->set_client(client_);

    // Load room snapshot for this account.
    auto it = per_account_rooms_.find(my_user_id_);
    if (it != per_account_rooms_.end()) {
        rooms_ = it->second;
        refresh_room_list();
    } else {
        rooms_.clear();
        refresh_room_list();
    }

    // Rewrite accounts.json active pointer.
    auto index = tesseract::SessionStore::load_index();
    index.active_user_id = my_user_id_;
    tesseract::SessionStore::save_index(index);

    rebuild_account_picker();
}

void MainWindow::begin_add_account() {
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;

    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pending_login_temp_dir_ =
        tesseract::SessionStore::account_dir("pending-" + std::to_string(ts));
    pending_login_client_ = std::make_unique<tesseract::Client>();
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());

    login_view_->set_client(pending_login_client_.get());
    login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    login_view_->reset();
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

void MainWindow::logout_active_account() {
    if (active_account_index_ < 0) return;

    auto& sess = *accounts_[active_account_index_];

    if (!current_room_id_.empty()) {
        client_->unsubscribe_room(current_room_id_);
        current_room_id_.clear();
    }

    if (sess.up_connector) sess.up_connector->logout();
    auto res = client_->logout();
    client_->stop_sync();

    tesseract::SessionStore::clear_account(sess.user_id);

    // Remove from the accounts vector.
    accounts_.erase(accounts_.begin() + active_account_index_);

    // Reset UI state.
    clear_messages();
    rooms_.clear();
    refresh_room_list();
    gtk_widget_set_visible(room_header_, FALSE);
    if (recovery_surface_)
        gtk_widget_set_visible(recovery_surface_->widget(), FALSE);
    recovery_banner_dismissed_ = false;

    gtk_label_set_text(GTK_LABEL(status_bar_),
                       res ? _("Signed out")
                           : (std::string(_("Sign out failed: ")) + res.message).c_str());

    if (accounts_.empty()) {
        client_        = nullptr;
        event_handler_ = nullptr;
        active_account_index_ = -1;
        my_user_id_.clear();
        my_display_name_.clear();
        my_avatar_url_.clear();
        gtk_widget_set_visible(user_strip_, FALSE);

        // Update accounts.json.
        tesseract::SessionStore::AccountIndex idx;
        tesseract::SessionStore::save_index(idx);

        login_view_->set_client(nullptr);
        login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
        login_view_->reset();
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    } else {
        // Switch to the closest remaining account.
        int next = std::min(static_cast<int>(accounts_.size()) - 1,
                            active_account_index_);
        active_account_index_ = -1;  // reset so switch_active_account does full rebind
        switch_active_account(next);

        auto idx = tesseract::SessionStore::load_index();
        idx.active_user_id = my_user_id_;
        // Remove logged-out uid from index.
        idx.user_ids.erase(
            std::remove(idx.user_ids.begin(), idx.user_ids.end(), sess.user_id),
            idx.user_ids.end());
        tesseract::SessionStore::save_index(idx);
    }
}

void MainWindow::on_login_cancelled() {
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty()) {
        std::filesystem::remove_all(pending_login_temp_dir_);
        pending_login_temp_dir_.clear();
    }

    if (pending_login_is_add_account_ && add_account_return_idx_ >= 0) {
        switch_active_account(add_account_return_idx_);
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    }
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
}

void MainWindow::rebuild_account_picker() {
    if (!account_picker_) return;
    std::vector<tesseract::views::AccountEntry> entries;
    entries.reserve(accounts_.size());
    for (const auto& sess : accounts_) {
        tesseract::views::AccountEntry e;
        e.user_id      = sess->user_id;
        e.display_name = sess->display_name;
        e.avatar_url   = sess->avatar_url;
        e.active       = (sess->user_id == my_user_id_);
        entries.push_back(std::move(e));
    }
    account_picker_->set_entries(std::move(entries));
    if (account_picker_surface_) account_picker_surface_->relayout();
}

void MainWindow::open_account_picker(double /*ax*/, double /*ay*/) {
    if (accounts_.size() < 2) return;

    if (!account_picker_popover_) {
        // Build once; a GtkPopover parented to the user strip.
        account_picker_surface_ =
            std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        account_picker_->on_select = [this](const std::string& uid) {
            if (account_picker_popover_)
                gtk_popover_popdown(GTK_POPOVER(account_picker_popover_));
            // Find the index of this account.
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                if (accounts_[i]->user_id == uid) {
                    switch_active_account(i);
                    break;
                }
            }
        };
        account_picker_surface_->set_root(std::move(picker));

        account_picker_popover_ = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(account_picker_popover_),
                              account_picker_surface_->widget());
        gtk_widget_set_parent(account_picker_popover_, user_strip_);
        gtk_popover_set_position(GTK_POPOVER(account_picker_popover_),
                                 GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(account_picker_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(account_picker_popover_), TRUE);

        // Size to fit rows.
        const int row_h = 48;
        gtk_widget_set_size_request(account_picker_surface_->widget(),
                                    240,
                                    row_h * static_cast<int>(accounts_.size()));
    }

    rebuild_account_picker();
    gtk_popover_popup(GTK_POPOVER(account_picker_popover_));
}

void MainWindow::on_user_strip_left_click_(GtkGestureClick* gesture,
                                            int /*n_press*/,
                                            double x, double y,
                                            gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    self->open_account_picker(x, y);
}

} // namespace gtk4
