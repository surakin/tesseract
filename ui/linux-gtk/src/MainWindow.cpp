#include "MainWindow.h"
#include "CallWindow.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorGtk.h"
#include "LoginView.h"
#include "views/BrandView.h"
#include "SettingsWidget.h"
#include "LinuxAutostartGtk.h"
#include "LinuxScreenLockGtk.h"
#include "app/SlashCommands.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif

#include "tk/canvas_cairo.h"
#include "tk/inflight_dot.h"
#include "tk/theme.h"
#include "tk/video_decode.h"
#include "views/media_drop.h"
#include "views/text_util.h"

#include <cairo.h>

#include <tesseract/emoji.h>
#include <tesseract/mentions.h>
#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "gettext_shorthand.h"

namespace gtk4
{

// Forward decl (defined later in this file, same anonymous namespace) so the
// GIF preview image provider — installed during room setup — can decode a
// provider preview URL's bytes into a cairo surface off the UI thread.
namespace
{
cairo_surface_t*
decode_image_to_cairo_surface(const std::vector<std::uint8_t>& bytes);
}

// Single GNotification id used for the "window visible but unfocused"
// attention request (GTK4 has no urgency-hint API). Reusing one id means a
// newer message replaces the previous attention banner, and the window
// becoming active withdraws it — mirroring the one-shot urgency-hint
// semantics other backends get.
namespace
{
constexpr char kAttentionNotifId[] = "tesseract-attention";
}

// Posts an arbitrary callable onto the GTK main loop via g_idle_add/
// g_timeout_add, heap-allocating the callable's own concrete type directly
// rather than boxing it in a std::function first — a guarded() closure
// (weak_ptr<T> + the wrapped fn) is usually larger than std::function's
// small-object buffer, so wrapping it in one would add a second, internal
// heap allocation on top of the explicit `new` below. Each call site
// instantiates its own trampoline for its own closure type, at no extra
// source cost over hand-writing the same three lines.
namespace
{
template <typename F>
void gtk_post_idle(F&& fn)
{
    using Closure = std::decay_t<F>;
    auto* c = new Closure(std::forward<F>(fn));
    g_idle_add(
        [](gpointer p) -> gboolean
        {
            auto* closure = static_cast<Closure*>(p);
            (*closure)();
            delete closure;
            return G_SOURCE_REMOVE;
        },
        c);
}

template <typename F>
void gtk_post_timeout(guint ms, F&& fn)
{
    using Closure = std::decay_t<F>;
    auto* c = new Closure(std::forward<F>(fn));
    g_timeout_add(
        ms,
        [](gpointer p) -> gboolean
        {
            auto* closure = static_cast<Closure*>(p);
            (*closure)();
            delete closure;
            return G_SOURCE_REMOVE;
        },
        c);
}
} // namespace

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// EventHandlerBase UI-thread hook implementations (GTK4)
// ---------------------------------------------------------------------------

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                       std::string description,
                                       bool soft_logout)
{
    // Agnostic state machine lives in ShellBase; this shell only supplies the
    // native restart timer (post_to_ui_after_), status label, user strip
    // (refresh_user_strip_) and relogin (request_relogin_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::refresh_user_strip_()
{
    populate_user_strip();
}

void MainWindow::request_relogin_(const std::string& /*user_id*/)
{
    do_login();
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    push_backup_progress(std::move(progress));
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id, std::string room_name,
    std::string sender, std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes,
    std::string event_id)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }

    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);
    push_notification(user_id, room_id, room_name, sender, body, is_mention,
                      std::move(avatar_bytes), std::move(image_bytes),
                      std::move(event_id));
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
    on_inflight_ui_();
}

void MainWindow::on_launch_at_login_pref_ui_(bool enabled)
{
    if (settings_widget_)
        settings_widget_->settings_view()->set_launch_at_login_pref(enabled);
}

void MainWindow::on_inflight_ui_()
{
    if (!inflight_dot_)
        return;
    const auto   n  = inflight_total_();
    const size_t fp = pool_pending_count_();
    const size_t sp = mut_pool_pending_count_();
    const size_t mp = pending_media_count_();
    gtk_widget_queue_draw(inflight_dot_);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "%u request%s in flight\nmedia: %zu loading · fetch: %zu queued · send: %zu queued",
                  n, n == 1u ? "" : "s", mp, fp, sp);
    std::string tip(buf);
#ifndef NDEBUG
    if (!last_inflight_urls_.empty()) {
        tip += "\n── requests ──\n";
        tip += last_inflight_urls_;
    }
#endif
    gtk_widget_set_tooltip_text(inflight_dot_, tip.c_str());
    gtk_widget_trigger_tooltip_query(inflight_dot_);
}

void MainWindow::draw_inflight_dot_(cairo_t* cr)
{
    auto       cv      = tk::cairo_pango::make_canvas(cr);
    const auto dot_col = inflight_dot_color_();
    constexpr tk::Color ring_col = tk::Color::rgb(0xA0A0A6);
    const float c = tk::kInflightViewSize / 2.0f;
    tk::draw_inflight_indicator(*cv, {c, c},
                                tk::kInflightDotR, tk::kInflightOrbitR,
                                tk::kInflightRingDotR,
                                dot_col, ring_col,
                                inflight_spin_phase_(),
                                inflight_needs_anim_());
}

void MainWindow::on_server_info_ready_ui_()
{
    if (settings_widget_)
        settings_widget_->set_server_info(server_info_);
    if (main_app_ && main_app_->room_view())
        main_app_->room_view()->header()->set_jump_to_date_enabled(
            server_info_.supports_msc3030);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::on_own_extended_profile_ready_ui_()
{
    if (settings_widget_)
        settings_widget_->set_extended_profile(own_extended_profile_);
}

void MainWindow::on_profile_field_result_ui_(const std::string& key,
                                              bool ok,
                                              const std::string& error)
{
    if (!settings_widget_) return;
    settings_widget_->set_profile_field_busy(key, false);
    if (!ok)
        settings_widget_->set_profile_field_error(key, error);
}

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
}

void MainWindow::on_active_room_bot_commands_changed_ui_()
{
    // Only meaningful while the popup is actually showing an active-room
    // autocomplete/hint — re-run the same text-changed path it already
    // drives on every keystroke, so a bot posting/editing/retracting a
    // command description mid-typing (or mid-argument-entry) is reflected
    // without the user needing to type another character.
    if (!slash_controller_ || !slash_controller_->visible() || !room_text_area_)
        return;
    slash_controller_->on_text_changed(room_text_area_->text(),
                                       room_text_area_->cursor_byte_pos());
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    if (!status_bar_)
        return;
    const auto segs = parse_status_message_(msg); // opt-in gate (server text → plain)
    if (!tesseract::status_has_links(segs))
    {
        // set_text resets use-markup, so a later plain message cleanly
        // clears any prior link rendering.
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        return;
    }
    std::string markup;
    for (const auto& seg : segs)
    {
        char* text = g_markup_escape_text(seg.text.c_str(), -1);
        if (seg.url.empty())
        {
            markup += text;
        }
        else
        {
            char* href = g_markup_escape_text(seg.url.c_str(), -1);
            markup += "<a href=\"";
            markup += href;
            markup += "\">";
            markup += text;
            markup += "</a>";
            g_free(href);
        }
        g_free(text);
    }
    gtk_label_set_markup(GTK_LABEL(status_bar_), markup.c_str());
}

void MainWindow::on_restore_status_ui_()
{
    refresh_sync_status();
}

void MainWindow::on_startup_restore_progress_ui_(const std::string& status)
{
    if (branding_view_)
    {
        branding_view_->set_status(status);
    }
}

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    if (is_verified)
    {
        main_app_->show_verif_banner(false);
        main_app_surface_->relayout();
        return;
    }
    // Only prompt when there is actually an identity to verify against. On a
    // fresh/only device our own login-time bootstrap holds the cross-signing
    // keys, so "verify this device" is a dead end — check_encryption_setup_
    // drives the Fresh setup overlay instead.
    if (!foreign_cross_signing_identity_())
    {
        main_app_->show_verif_banner(false);
        main_app_surface_->relayout();
        return;
    }
    if (verification_banner_dismissed_)
    {
        return;
    }
    if (!main_app_->verif_banner()->visible())
    {
        active_verification_flow_id_.clear();
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Prompt);
        main_app_->show_verif_banner(true);
        main_app_surface_->relayout();
    }
}

void MainWindow::handle_verification_request_ui_(std::string flow_id,
                                                 std::string /*user_id*/,
                                                 std::string /*device_id*/,
                                                 bool incoming)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    active_verification_flow_id_ = flow_id;
    if (incoming)
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    }
    else
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_)
        {
            client_->start_sas(flow_id);
        }
    }
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_emojis(emojis);
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    dismiss_encryption_setup_after_verification_();
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Done);
    main_app_surface_->relayout();
    // Hide after 1.5 s. The payload is a guarded() closure — built here,
    // synchronously (`this` is definitely alive) — so it no-ops instead of
    // touching a freed window if this fires after the window is destroyed.
    gtk_post_timeout(1500, guarded([this]
    {
        if (verif_shared_ && verif_shared_->on_done)
        {
            verif_shared_->on_done();
        }
    }));
}

void MainWindow::handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                   std::string reason)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    verif_shared_->set_cancel_reason(std::move(reason));
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

// ---------------------------------------------------------------------------
// User context menu helpers — trampoline + cleanup for g_signal_connect_data
// ---------------------------------------------------------------------------

namespace {
struct UserMenuCtx {
    std::function<void()> cb;
};
void user_menu_activate_(GSimpleAction*, GVariant*, gpointer p)
{
    static_cast<UserMenuCtx*>(p)->cb();
}
void user_menu_ctx_free_(gpointer p, GClosure*)
{
    delete static_cast<UserMenuCtx*>(p);
}
} // namespace

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(tesseract::AccountManager& account_manager,
                       GtkApplication* app, bool start_hidden
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                       , std::filesystem::path screenshot_dir
#endif
                       )
    : ShellBase(account_manager)
    , app_(app)
    , start_hidden_(start_hidden)
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    , screenshot_dir_(std::move(screenshot_dir))
#endif
{
    set_screen_lock_(std::make_unique<LinuxScreenLockGtk>());
    set_autostart_(std::make_unique<LinuxAutostartGtk>());

    if (start_hidden_)
    {
        // Keep GApplication alive while the window is hidden and no tray
        // exists yet (tray is only created post-login in
        // start_tray_if_needed_(), which also holds — GLib's use-count has
        // no matching g_application_release() anywhere in this shell, real
        // quit always goes through the explicit g_application_quit() calls
        // below, so an extra hold() here is harmless, not a leak).
        g_application_hold(G_APPLICATION(app_));
    }

    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Tesseract");
    gtk_widget_set_size_request(
        GTK_WIDGET(window_),
        static_cast<int>(tesseract::visual::kMinWindowWidth), -1);
    // Size is applied after Settings load (see below); fall back to 1100×768.
    gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 768);

#ifdef TESSERACT_ICON_SEARCH_PATH
    gtk_icon_theme_add_search_path(
        gtk_icon_theme_get_for_display(gtk_widget_get_display(window_)),
        TESSERACT_ICON_SEARCH_PATH);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "tesseract");
#endif

    g_object_set_data(G_OBJECT(window_), "cpp_window", this);

    // ---- CSS ----
    theme_css_provider_ = gtk_css_provider_new();
    gtk_css_provider_load_from_string(theme_css_provider_, R"css(
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
        gdk_display_get_default(), GTK_STYLE_PROVIDER(theme_css_provider_),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ---- Layout ----
    content_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack_),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_window_set_child(GTK_WINDOW(window_), content_stack_);

    branding_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    // create_root_widget (not plain make_unique) so BrandView::host() is
    // valid from construction — Surface::set_root()/RootWidget::add_child()
    // adopt an already-built widget without ever fixing up its host_, so a
    // plain make_unique here leaves host_ permanently null and BrandView's
    // self-scheduling post_delayed() wireframe animation silently never
    // arms itself (confirmed on Qt: it depends entirely on incidental
    // repaints from elsewhere, freezing solid whenever nothing else
    // invalidates it — e.g. throughout the whole account-restore window).
    auto branding_owner =
        tk::create_root_widget<tesseract::views::BrandView>(&branding_surface_->host());
    branding_view_ = branding_owner.get();
    branding_surface_->set_root(std::move(branding_owner));
    gtk_stack_add_named(GTK_STACK(content_stack_),
                        branding_surface_->widget(), "branding");

    login_view_ = std::make_unique<LoginView>();
    // Route the homeserver-discovery debounce through the shell's worker
    // drain so a blocked discover_homeserver call can't outlive ~LoginView
    // and corrupt the heap (mirrors the SettingsController wiring below).
    login_view_->set_run_async(
        [this](std::function<void()> fn) { run_async_(std::move(fn)); });
    login_view_->set_on_success(
        [this]()
        {
            on_login_succeeded();
        });
    login_view_->set_on_cancel(
        [this]()
        {
            on_login_cancelled();
        });
    gtk_stack_add_named(GTK_STACK(content_stack_), login_view_->widget(),
                        "login");

    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // Apply saved window size (GTK4/Wayland: position is compositor-managed).
    {
        const auto& saved = tesseract::Settings::instance().main_window_geometry;
        if (saved.valid && saved.w > 0 && saved.h > 0)
            gtk_window_set_default_size(GTK_WINDOW(window_), saved.w, saved.h);
    }

    // Save window size to Settings on every resize (debounced 500 ms).
    // GTK4 fires notify::default-width/-height as the user resizes the window.
    g_signal_connect(
        window_, "notify::default-width",
        G_CALLBACK(+[](GObject* /*obj*/, GParamSpec* /*ps*/, gpointer data)
                   {
                       auto* self = static_cast<MainWindow*>(data);
                       auto& g    = tesseract::Settings::instance().main_window_geometry;
                       g.w        = gtk_widget_get_width(GTK_WIDGET(self->window_));
                       g.h        = gtk_widget_get_height(GTK_WIDGET(self->window_));
                       g.valid    = (g.w > 0 && g.h > 0);
                       self->save_settings_debounced_();
                   }),
        this);
    g_signal_connect(
        window_, "notify::default-height",
        G_CALLBACK(+[](GObject* /*obj*/, GParamSpec* /*ps*/, gpointer data)
                   {
                       auto* self = static_cast<MainWindow*>(data);
                       auto& g    = tesseract::Settings::instance().main_window_geometry;
                       g.w        = gtk_widget_get_width(GTK_WIDGET(self->window_));
                       g.h        = gtk_widget_get_height(GTK_WIDGET(self->window_));
                       g.valid    = (g.w > 0 && g.h > 0);
                       self->save_settings_debounced_();
                   }),
        this);

    // Single surface hosting the full main-app widget tree.
    main_app_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    // Feed pointer / wheel events into the PresenceTracker. Focus + tick are
    // wired separately via notify::is-active + g_timeout_add.
    main_app_surface_->host().set_on_user_activity(
        [this] { notify_user_activity_(); });
    // Track the display's current scale so thumbnail/avatar requests can be
    // sized for it — see ShellBase::set_current_scale_()'s doc comment. The
    // initial query may return the GTK default (1) if this widget isn't
    // realized yet; the live notify::scale-factor signal wired into
    // Surface::apply_scale_change() corrects it shortly after the window
    // is shown, same as any other live change.
    main_app_surface_->set_on_scale_changed(
        [this](float s) { set_current_scale_(s); });
    set_current_scale_(static_cast<float>(
        gtk_widget_get_scale_factor(main_app_surface_->widget())));
    {
        auto main_app_owner = tk::create_root_widget<tesseract::views::MainAppWidget>(
            &main_app_surface_->host());
        main_app_ = main_app_owner.get();
        room_list_view_ = main_app_->room_list_view();
        room_view_ = main_app_->room_view();
        verif_shared_ = main_app_->verif_banner();
        img_viewer_ = main_app_->image_viewer();
        vid_viewer_ = main_app_->video_viewer();
        room_media_view_ = main_app_->room_media_view();
        main_app_->on_quick_switch_shortcut = [this] { open_quick_switch_(); };
        main_app_->on_message_search_shortcut =
            [this] { open_message_search_(); };
        main_app_->on_find_in_room_shortcut = [this] { open_find_in_room_(); };
        main_app_->on_history_back_shortcut = [this] { navigate_history_back(); };
        main_app_->on_history_forward_shortcut =
            [this] { navigate_history_forward(); };

        // Wire TabBar callbacks.
        main_app_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            // Ctrl+click pops the room out into its own window (and closes the
            // tab); a plain click just switches to it. The GTK host captures
            // the modifier from the click gesture (tk pointer events don't
            // carry it).
            if (main_app_surface_ &&
                main_app_surface_->host().pointer_ctrl_held())
            {
                tab_popout_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        main_app_->tab_bar()->on_tab_closed = [this](const std::string& room_id)
        {
            tab_close(room_id);
        };

        // ---- Shared per-room pane (the sole source of truth for
        // room_view_'s image/video viewer callbacks — see
        // ShellBase::main_room_pane_'s doc comment) ----
        main_room_pane_ = std::make_unique<tesseract::RoomPane>(
            tesseract::RoomPane::Deps{
                .shell = this,
                .host = &main_app_surface_->host(),
                .repaint = [this] { request_repaint_(); },
                .relayout = [this] { request_relayout_(); },
                .grab_surface_focus = [this]
                {
                    if (main_app_surface_)
                        gtk_widget_grab_focus(main_app_surface_->widget());
                },
                .update_window_title = [this](const std::string& name)
                {
                    if (window_)
                    {
                        const std::string title = name.empty()
                            ? "Tesseract"
                            : "Tesseract - " + name;
                        gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
                    }
                },
                .on_left_room = [this](const std::string& room_id)
                {
                    if (current_room_id_ != room_id)
                        return;
                    current_room_id_.clear();
                    if (room_view_)
                        room_view_->clear_room();
                    if (main_app_)
                        main_app_->room_list_view()->set_selected_room("");
                    if (main_app_surface_)
                        main_app_surface_->relayout();
                },
            },
            current_room_id_);
        main_room_pane_->attach({
            .room_view = main_app_->room_view(),
            .img_viewer = main_app_->image_viewer(),
            .vid_viewer = main_app_->video_viewer(),
            .forward_picker = main_app_->forward_picker(),
            .room_media_view = main_app_->room_media_view(),
            .focus_forward_picker_field = [this] { focus_forward_picker_field_(); },
            .hide_forward_picker_field = [this] { hide_forward_picker_field_(); },
        });

        // Wire provider callbacks (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // Wire UserInfo callbacks (replaces native GTK user-strip gestures).
        main_app_->user_info()->on_primary = [this](tk::Point world)
        {
            open_account_picker(world.x, world.y);
        };
        main_app_->user_info()->on_secondary = [this](tk::Point world)
        {
            // Rebuild the popover with current state (display name, QR
            // support) so the logout label and QR item are always fresh.
            if (user_popover_)
            {
                gtk_widget_unparent(user_popover_);
                user_popover_ = nullptr;
            }

            const auto items = build_user_menu_items_(
                [this] { open_settings_(); },
                [this] { begin_add_account(); },
                [this] { start_qr_grant_overlay(); },
                [this] { logout_active_account(); },
                [this] { tray_.reset(); g_application_quit(G_APPLICATION(app_)); });

            GMenu* top = g_menu_new();
            GMenu* cur = g_menu_new();
            GSimpleActionGroup* grp = g_simple_action_group_new();
            int id = 0;

            for (const auto& item : items)
            {
                if (item.label.empty())
                {
                    g_menu_append_section(top, nullptr, G_MENU_MODEL(cur));
                    g_object_unref(cur);
                    cur = g_menu_new();
                    continue;
                }
                char name[32];
                std::snprintf(name, sizeof(name), "i%d", id);
                char full[48];
                std::snprintf(full, sizeof(full), "usr.i%d", id);

                g_menu_append(cur, item.label.c_str(), full);

                GSimpleAction* act = g_simple_action_new(name, nullptr);
                g_signal_connect_data(act, "activate",
                                      G_CALLBACK(user_menu_activate_),
                                      new UserMenuCtx{item.callback},
                                      user_menu_ctx_free_,
                                      static_cast<GConnectFlags>(0));
                g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
                g_object_unref(act);
                ++id;
            }
            g_menu_append_section(top, nullptr, G_MENU_MODEL(cur));
            g_object_unref(cur);

            user_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(top));
            g_object_unref(top);
            gtk_widget_set_parent(user_popover_, main_app_surface_->widget());
            gtk_popover_set_has_arrow(GTK_POPOVER(user_popover_), FALSE);
            gtk_widget_insert_action_group(user_popover_, "usr",
                                           G_ACTION_GROUP(grp));
            g_object_unref(grp);

            GdkRectangle r = {static_cast<int>(world.x),
                              static_cast<int>(world.y), 1, 1};
            gtk_popover_set_pointing_to(GTK_POPOVER(user_popover_), &r);
            gtk_popover_popup(GTK_POPOVER(user_popover_));
        };

        // Space nav back button.
        main_app_->on_space_back = [this]
        {
            if (!space_stack_.empty())
                space_stack_.pop_back();
            if (main_app_)
                main_app_->hide_room_preview();
            if (main_app_)
                main_app_->hide_space_root();
            refresh_room_list();
            if (!space_nav_frames_.empty())
            {
                space_nav_frames_.back().restore(room_list_view_);
                space_nav_frames_.pop_back();
            }
        };

        room_list_view_->on_room_selected = [this](const std::string& room_id)
        {
            // A space is not a room: clicking one drills into it rather than
            // opening it as the active room/tab (which would put the space
            // title in the room header).
            for (const auto& r : rooms_)
            {
                if (r.id == room_id && r.is_space)
                {
                    space_nav_frames_.push_back(SpaceNavFrame::capture(room_list_view_));
                    space_stack_.push_back(room_id);
                    refresh_room_list();
                    SpaceNavFrame::enter(room_list_view_);
                    return;
                }
            }
            // Ctrl+click opens the room in a new tab; a plain click switches
            // the active tab. The GTK host captures the modifier from the
            // click gesture (tk pointer events don't carry it).
            if (main_app_surface_ &&
                main_app_surface_->host().pointer_ctrl_held())
            {
                tab_open_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        room_list_view_->set_room_open_in_tab_provider(
            [this](const std::string& rid) { return room_open_in_tab(rid); });
        room_list_view_->set_room_open_in_window_provider(
            [this](const std::string& rid) { return room_open_in_window(rid); });
        room_list_view_->on_open_in_tab_requested =
            [this](const std::string& rid) { tab_open_room(rid); };
        room_list_view_->on_open_in_window_requested =
            [this](const std::string& rid) { open_room_in_new_window(rid); };
        room_list_view_->on_leave_room_requested =
            [this](const std::string& rid) { confirm_leave_room_(rid); };
        room_list_view_->on_scroll = [this]
        {
            if (scroll_debounce_id_)
            {
                g_source_remove(scroll_debounce_id_);
                scroll_debounce_id_ = 0;
            }
            scroll_debounce_id_ = g_timeout_add(
                300,
                [](gpointer ud) -> gboolean
                {
                    auto* self = static_cast<MainWindow*>(ud);
                    self->scroll_debounce_id_ = 0;
                    if (!self->room_list_view_ || !self->active_account_)
                    {
                        return G_SOURCE_REMOVE;
                    }
                    auto ids  = self->room_list_view_->visible_room_ids();
                    auto sess = self->active_account_;
                    self->run_async_mut_([sess, ids = std::move(ids)]() mutable
                    {
                        if (sess && sess->client)
                        {
                            sess->client->stop_background_backfill();
                            sess->client->start_background_backfill(ids);
                        }
                    });
                    return G_SOURCE_REMOVE;
                },
                this);
        };
        room_list_view_->on_search_clear = [this]
        {
            cancel_debounce_(DebounceSlot::RoomSearch);
            search_pending_text_.clear();
            if (auto* sf = room_list_view_->search_field())
            {
                sf->set_text("");
            }
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
        room_list_view_->on_unjoined_room_selected =
            [this](const tesseract::RoomSummary& s)
        {
            if (!s.avatar_url.empty())
                ensure_media_thumbnail_(s.avatar_url, 64, 64, false);
            if (main_app_)
            {
                main_app_->show_room_preview(s, make_avatar_image_provider_());
                request_relayout_();
            }
        };
        if (auto* rp = main_app_->room_preview())
        {
            rp->on_avatar_needed = [this](const std::string& mxc)
            {
                ensure_media_thumbnail_(mxc, 64, 64, false);
            };
            rp->on_join = [this, rp](const std::string& room_id)
            {
                rp->set_state(tesseract::views::RoomPreviewView::State::Joining);
                join_room_command_(room_id);
            };
            rp->on_dismiss = [this]
            {
                if (main_app_)
                    main_app_->hide_room_preview();
            };
        }

        // Wire RoomView shortcode lookup (avatar/image/preview wired via
        // wire_main_app_widget_).
        room_view_->set_shortcode_provider(
            [this](const std::string& mxc) -> std::string
            {
                return shortcode_for_mxc_(mxc);
            });
        // Mention-pill avatar provider + on_fetch_room_members (which
        // populates the cache it reads) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), using
        // RoomPane's own cached_room_members_/cached_members_room_ instead
        // of this window's now-removed copy of those fields.
        if (auto player = main_app_surface_->host().make_audio_player())
        {
            room_view_->set_audio_player(std::move(player));
        }
        capture_ = main_app_surface_->host().make_audio_capture();
        {
            tk::gtk4::Surface* sfp = main_app_surface_.get();
            wire_voice_capture_(
                room_view_,
                [sfp]() { gtk_widget_queue_draw(sfp->widget()); },
                [this]() { return current_room_id_; },
                [this]() { room_view_->set_current_text({}); });
        }
        room_view_->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t>
            {
                // Non-blocking: warmed bytes or empty + async fetch (repaint on
                // arrival) so playback never freezes the UI thread.
                return voice_bytes_or_fetch_(source_json,
                                             [this] { request_relayout_(); });
            });
        {
            tk::gtk4::Surface* sfp = main_app_surface_.get();
            room_view_->set_repaint_requester(
                [sfp]()
                {
                    if (sfp)
                    {
                        gtk_widget_queue_draw(sfp->widget());
                    }
                });
            room_view_->set_post_delayed(
                [sfp](int ms, std::function<void()> fn)
                {
                    if (sfp)
                    {
                        sfp->host().post_delayed(ms, std::move(fn));
                    }
                });
        }

        // Compose text area (self-owned — see ComposeBar::text_area()).
        room_text_area_ = main_app_->room_view()->compose_bar()->text_area();
        // Harmless no-op on every backend except Windows' BetterText control —
        // see NativeTextArea::set_image_resolver's own doc comment. Wired
        // unconditionally here for symmetry with pop-out wiring (RoomWindowBase::
        // finish_init_() already does this unconditionally for every pop-out).
        room_text_area_->set_image_resolver(make_static_image_provider_with_fetch_(28, 28));
        // All four composer popups (gif > slash > shortcode > mention) are
        // driven through the shared ComposePopups dispatch; the controllers are
        // created just below (slash/shortcode/mention) and in the GIF block.
        room_text_area_->set_on_changed(
            [this](const std::string& s)
            {
                handle_compose_text_changed_(s);
                room_view_->set_current_text(s);
                tesseract::views::dispatch_compose_text_changed(
                    s, room_text_area_->cursor_byte_pos(),
                    gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get());
            });
        room_text_area_->set_on_submit(
            [this]
            {
                if (tesseract::views::dispatch_compose_submit(
                        gif_controller_.get(), slash_controller_.get(),
                        shortcode_controller_.get(),
                        mention_controller_.get()))
                {
                    return;
                }
                on_send_clicked();
            });
        room_text_area_->push_popup_nav(
            [this](tk::NavKey nk) -> bool
            {
                return tesseract::views::dispatch_compose_nav(
                    nk, gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get());
            });

        // ── /command autocomplete popup ──────────────────────────────────
        slash_popup_ = main_app_surface_->host().make_popup_surface();
        {
            auto w = std::make_unique<tesseract::views::SlashCommandPopup>();
            slash_popup_widget_ = w.get();
            slash_popup_->set_root(std::move(w));
        }
        {
            tesseract::views::SlashCommandController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_slash_popup_(cursor, rows); };
            sh.hide = [this] { hide_slash_popup_(); };
            sh.repaint = [this]
            {
                if (slash_popup_)
                    slash_popup_->request_repaint();
            };
            sh.room_id = [this] { return current_room_id_; };
            sh.client = [this]() -> tesseract::Client* { return client_; };
            sh.clear_composer = [this] { room_view_->clear_compose_text(); };
            sh.on_selfie = [this]
            {
                main_app_->is_call_active = [this] { return active_call() != nullptr; };
                main_app_->on_selfie_captured =
                    [this](std::vector<std::uint8_t> bgra,
                           std::uint32_t w, std::uint32_t h)
                    {
                        // GdkPixbuf expects RGBA; swap R and B channels.
                        std::vector<std::uint8_t> rgba(bgra.size());
                        for (std::size_t i = 0; i + 3 < bgra.size(); i += 4)
                        {
                            rgba[i + 0] = bgra[i + 2]; // R
                            rgba[i + 1] = bgra[i + 1]; // G
                            rgba[i + 2] = bgra[i + 0]; // B
                            rgba[i + 3] = bgra[i + 3]; // A
                        }
                        GdkPixbuf* pb = gdk_pixbuf_new_from_data(
                            rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8,
                            static_cast<int>(w), static_cast<int>(h),
                            static_cast<int>(w * 4), nullptr, nullptr);
                        if (pb)
                        {
                            GError* gerr = nullptr;
                            gchar*  data = nullptr;
                            gsize   sz   = 0;
                            if (gdk_pixbuf_save_to_buffer(
                                    pb, &data, &sz, "jpeg", &gerr,
                                    "quality", "90", nullptr))
                            {
                                if (room_view_->compose_bar())
                                {
                                    room_view_->compose_bar()->set_pending_image(
                                        std::vector<std::uint8_t>(
                                            reinterpret_cast<const std::uint8_t*>(data),
                                            reinterpret_cast<const std::uint8_t*>(data) + sz),
                                        "image/jpeg", "selfie.jpg");
                                }
                                g_free(data);
                            }
                            if (gerr) g_error_free(gerr);
                            g_object_unref(pb);
                        }
                    };
                main_app_->open_camera_overlay();
            };
            sh.on_location = [this] { send_current_location_(current_room_id_); };
            sh.bot_commands = [this]() -> std::vector<tesseract::CommandDescription>
            {
                return client_ ? client_->list_room_bot_commands(current_room_id_)
                               : std::vector<tesseract::CommandDescription>{};
            };
            slash_controller_ =
                std::make_unique<tesseract::views::SlashCommandController>(
                    room_text_area_, slash_popup_widget_, std::move(sh));
        }

        // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────
        shortcode_popup_ = main_app_surface_->host().make_popup_surface();
        {
            auto w = std::make_unique<tesseract::views::ShortcodePopup>();
            shortcode_popup_widget_ = w.get();
            shortcode_popup_widget_->set_image_provider(
                make_static_image_provider_with_fetch_(28, 28));
            shortcode_popup_->set_root(std::move(w));
        }
        {
            tesseract::views::ShortcodeController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_shortcode_popup_(cursor, rows); };
            sh.hide = [this] { hide_shortcode_popup_(); };
            sh.repaint = [this]
            {
                if (shortcode_popup_)
                    shortcode_popup_->request_repaint();
            };
            sh.emoticons =
                [this]() { return emoticons_for_room_(current_room_id_); };
            sh.fetch_image = [this](const std::string& url)
            { ensure_media_image_(url, 28, 28); };
            sh.resolve_image = make_static_image_provider_with_fetch_(28, 28);
            shortcode_controller_ =
                std::make_unique<tesseract::views::ShortcodeController>(
                    room_text_area_, shortcode_popup_widget_,
                    std::move(sh));
        }

        // ── @mention autocomplete popup ───────────────────────────────────
        mention_popup_ = main_app_surface_->host().make_popup_surface();
        {
            auto w = std::make_unique<tesseract::views::MentionPopup>();
            mention_popup_widget_ = w.get();
            mention_popup_widget_->set_image_provider(
                make_avatar_image_provider_());
            mention_popup_->set_root(std::move(w));
        }
        {
            tesseract::views::MentionController::Hooks mh;
            mh.show = [this](tk::Rect cursor, int rows)
            { show_mention_popup_(cursor, rows); };
            mh.hide = [this] { hide_mention_popup_(); };
            mh.repaint = [this]
            {
                if (mention_popup_)
                    mention_popup_->request_repaint();
            };
            mh.room_id = [this] { return current_room_id_; };
            mh.client = [this]() -> tesseract::Client* { return client_; };
            mh.fetch_avatar = [this](const std::string& mxc)
            { ensure_user_avatar_(mxc); };
            mh.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            mh.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            mention_controller_ =
                std::make_unique<tesseract::views::MentionController>(
                    room_text_area_, client_, mention_popup_widget_,
                    std::move(mh));
        }

        // ── GIF picker (/gif <query>) ────────────────────────────────────────
        gif_popup_ = main_app_surface_->host().make_popup_surface();
        {
            auto w = std::make_unique<tesseract::views::GifPopup>();
            gif_popup_widget_ = w.get();
            gif_popup_->set_root(std::move(w));
        }
        gif_popup_->set_anim_cache(&account_manager_.anim_cache());
        // Two-stage GIF strip cell provider, parameterised on a `repaint`
        // callback so the identical body serves the main window's strip and
        // every pop-out's (each passes a repaint targeting its own popup
        // surface, self-guarded by that window's liveness token). Stored as a
        // member; pop-outs reach it via the gif_strip_image_() override.
        gif_strip_provider_ =
            [this](const tesseract::GifResult& result,
                   const std::function<void()>& repaint) -> const tk::Image*
            {
                // The strip animates strip_url (WebP/GIF, native decode), keyed
                // in anim_cache_. Serving a cached frame means animated content
                // is on screen, so ensure the tick timer runs: re-shown searches
                // take this path without re-fetching.
                if (const tk::Image* f = account_manager_.anim_cache().current_frame(result.strip_url))
                {
                    start_anim_tick_if_needed_();
                    return f;
                }
                // NOTE: the static-preview fallback is returned at the *end* of
                // this lambda, AFTER the animated re-fetch is kicked below.
                // Returning it here would short-circuit re-animation on a
                // re-shown search whose anim_cache_ entry was evicted while its
                // static thumbnail lingers in gif_previews_.
                // Kick off static preview fetch only when not already cached.
                if (!gif_previews_.count(result.preview_url) &&
                    gif_preview_inflight_.insert(result.preview_url).second)
                {
                    auto url = result.preview_url;
                    // guarded() is called here, synchronously (this image
                    // provider runs on the UI thread — `this` is definitely
                    // alive). on_bytes_decoded already carries its own weak
                    // token; the completion callback below (which may run on
                    // a worker thread) just calls it, never re-derives it.
                    auto on_bytes_decoded = guarded(
                        [this, url, repaint](const std::vector<std::uint8_t>& bytes) mutable
                        {
                            using CW = tesseract::views::GifPopup;
                            DecodedImage d = decode_image_(
                                bytes, int(CW::kCellW) * 2,
                                int(CW::kCellH) * 2);
                            if (d.still)
                                gif_previews_[url] = std::move(d.still);
                            repaint();
                        });
                    {
                        const std::string disk_key = gif_src_disk_key_(url);
                        auto req_id = begin_media_req_(0,
                            [this, url, disk_key, on_bytes_decoded](
                                std::vector<std::uint8_t> bytes) mutable
                            {
                                gif_preview_inflight_.erase(url);
                                if (bytes.empty()) return;
                                run_async_(
                                    [this, disk_key, bytes]() mutable
                                    {
                                        account_manager_.media_disk_cache().store(
                                            disk_key, std::move(bytes));
                                    });
                                on_bytes_decoded(bytes);
                            });
                        run_async_(
                            [this, req_id, url, disk_key]()
                            {
                                auto bytes =
                                    account_manager_.media_disk_cache().load(disk_key);
                                if (!bytes.empty())
                                {
                                    post_to_ui_(
                                        [this, req_id, bytes = std::move(bytes)]() mutable
                                        {
                                            handle_media_ready_ui_(req_id, std::move(bytes));
                                        });
                                    return;
                                }
                                if (client_)
                                    client_->fetch_url_async(req_id, 0, url);
                            });
                    }
                }
                // Kick off the strip-display fetch (strip_url: WebP/GIF) — decode
                // on the worker thread. The MP4 send form is fetched at send time.
                if (gif_anim_inflight_.insert(result.strip_url).second)
                {
                    auto anim_url = result.strip_url;
                    auto anim_mime = result.strip_mime;
                    // guarded() is called here, synchronously (this image
                    // provider runs on the UI thread — `this` is definitely
                    // alive). Both on_*_decoded closures already carry their
                    // own weak token; the deeply-nested worker/post_to_ui_
                    // callbacks below (running on other threads) just call
                    // them, never re-derive one of their own.
                    auto on_mp4_decoded = guarded(
                        [this, anim_url, repaint](
                            std::shared_ptr<std::vector<std::unique_ptr<tk::Image>>> imgs,
                            std::vector<int> delays) mutable
                        {
                            if (!imgs->empty())
                            {
                                account_manager_.anim_cache().store(
                                    anim_url, std::move(*imgs), std::move(delays),
                                    g_get_monotonic_time() / 1000);
                                start_anim_tick_if_needed_();
                            }
                            repaint();
                        });
                    auto on_image_decoded = guarded(
                        [this, anim_url, repaint](std::shared_ptr<DecodedImage> d) mutable
                        {
                            if (!d->frames.empty())
                            {
                                account_manager_.anim_cache().store(
                                    anim_url, std::move(d->frames), std::move(d->delays_ms),
                                    g_get_monotonic_time() / 1000);
                                start_anim_tick_if_needed_();
                            }
                            else if (d->still)
                            {
                                gif_previews_[anim_url] = std::move(d->still);
                            }
                            repaint();
                        });
                    {
                        const std::string disk_key = gif_src_disk_key_(anim_url);
                        auto req_id = begin_media_req_(0,
                            [this, anim_url, anim_mime, disk_key, on_mp4_decoded,
                             on_image_decoded](
                                std::vector<std::uint8_t> bytes) mutable
                            {
                                gif_anim_inflight_.erase(anim_url);
                                if (bytes.empty()) return;
                                run_async_(
                                    [this, anim_url, anim_mime, disk_key,
                                     on_mp4_decoded, on_image_decoded,
                                     bytes = std::move(bytes)]() mutable
                                    {
                                        account_manager_.media_disk_cache().store(
                                            disk_key, bytes);
                                        using CW = tesseract::views::GifPopup;
                                        if (anim_mime == "video/mp4")
                                        {
                                            tk::DecodedVideoFrames dvf =
                                                tk::decode_video_frames(
                                                    bytes.data(), bytes.size(),
                                                    int(CW::kCellW) * 2,
                                                    int(CW::kCellH) * 2);
                                            auto imgs = std::make_shared<
                                                std::vector<std::unique_ptr<tk::Image>>>();
                                            std::vector<int> delays;
                                            for (auto& f : dvf.frames)
                                            {
                                                cairo_surface_t* surf =
                                                    cairo_image_surface_create(
                                                        CAIRO_FORMAT_ARGB32, f.w, f.h);
                                                if (surf &&
                                                    cairo_surface_status(surf) ==
                                                        CAIRO_STATUS_SUCCESS)
                                                {
                                                    unsigned char* dst =
                                                        cairo_image_surface_get_data(surf);
                                                    const int dst_stride =
                                                        cairo_image_surface_get_stride(surf);
                                                    const int src_stride = f.w * 4;
                                                    for (int y = 0; y < f.h; ++y)
                                                    {
                                                        std::memcpy(
                                                            dst + y * dst_stride,
                                                            f.bgra.data() + y * src_stride,
                                                            std::min(src_stride,
                                                                     dst_stride));
                                                    }
                                                    cairo_surface_mark_dirty(surf);
                                                    imgs->push_back(
                                                        tk::cairo_pango::make_image(surf));
                                                    delays.push_back(f.delay_ms);
                                                }
                                                if (surf)
                                                    cairo_surface_destroy(surf);
                                            }
                                            post_to_ui_(
                                                [on_mp4_decoded, imgs,
                                                 delays = std::move(delays)]() mutable
                                                {
                                                    on_mp4_decoded(imgs, std::move(delays));
                                                });
                                        }
                                        else
                                        {
                                            auto d = std::make_shared<DecodedImage>(
                                                decode_image_(bytes,
                                                              int(CW::kCellW) * 2,
                                                              int(CW::kCellH) * 2));
                                            post_to_ui_(
                                                [on_image_decoded, d]() mutable
                                                {
                                                    on_image_decoded(d);
                                                });
                                        }
                                    });
                            });
                        run_async_(
                            [this, req_id, anim_url, disk_key]()
                            {
                                auto bytes =
                                    account_manager_.media_disk_cache().load(disk_key);
                                if (!bytes.empty())
                                {
                                    post_to_ui_(
                                        [this, req_id, bytes = std::move(bytes)]() mutable
                                        {
                                            handle_media_ready_ui_(req_id, std::move(bytes));
                                        });
                                    return;
                                }
                                if (client_)
                                    client_->fetch_url_async(req_id, 0, anim_url);
                            });
                    }
                }
                // Static JPEG preview shown while the animation decodes (or as
                // the permanent fallback for a non-animated result).
                if (auto it = gif_previews_.find(result.preview_url);
                    it != gif_previews_.end())
                    return it->second.get();
                return nullptr;
            };
        // The main window's own strip repaints its own surface.
        gif_popup_widget_->set_image_provider(
            [this](const tesseract::GifResult& result) -> const tk::Image*
            {
                return gif_strip_provider_(result,
                                           [this]
                                           {
                                               if (gif_popup_)
                                                   gif_popup_->request_repaint();
                                           });
            });
        {
            tesseract::views::GifController::Hooks gh;
            gh.show = [this] { show_gif_popup_(); };
            gh.hide = [this] { hide_gif_popup_(); };
            gh.repaint = [this]
            {
                if (gif_popup_)
                    gif_popup_->request_repaint();
            };
            gh.room_id = [this] { return current_room_id_; };
            gh.client = [this]() -> tesseract::Client* { return client_; };
            gh.run_async = [this](std::function<void()> fn)
            { run_async_(std::move(fn)); };
            gh.post_to_ui = [this](std::function<void()> fn)
            { post_to_ui_(std::move(fn)); };
            gh.post_delayed = [this](int ms, std::function<void()> fn)
            {
                if (main_app_surface_)
                    main_app_surface_->host().post_delayed(ms, std::move(fn));
            };
            gh.api_key = []() -> std::string
            { return tesseract::Settings::instance().gif_api_key; };
            gh.client_key = []() -> std::string { return "tesseract"; };
            gh.clear_composer = [this]
            {
                if (room_text_area_)
                    room_text_area_->set_text("");
                if (room_view_)
                    room_view_->clear_compose_text();
            };
            gh.get_cached_gif_bytes =
                [this](const std::string& url) -> std::vector<std::uint8_t>
            {
                // Reuse the source bytes the strip persisted to disk on fetch.
                return account_manager_.media_disk_cache().load(gif_src_disk_key_(url));
            };
            gif_controller_ = std::make_unique<tesseract::views::GifController>(
                room_text_area_, gif_popup_widget_, std::move(gh));
        }

        room_text_area_->set_on_edit_last(
            [this]
            {
                return room_view_ && room_view_->edit_last_own();
            });
        // Auto-grow (set_on_height_changed) and image-paste
        // (set_on_image_paste) are wired internally by ComposeBar's own
        // constructor now — see ComposeBar::ComposeBar()'s text_area_ setup.

        // The topic field, the new-pack-name/shortcode/rename fields, and the
        // paste-catcher are all self-owned by each RoomSettingsView instance
        // (room_view_'s and the space-root's, independently) — see
        // RoomSettingsView::name_field()/topic_field() and
        // ImagePackEditorView::new_pack_name_field()/shortcode_field()/
        // pack_name_field()/paste_catcher() — so no shell-side wiring is
        // needed for any of them.

        // Drop-into-compose-bar wiring for RoomView::on_file_drop (the
        // tree-dispatched catch-all reached when a drop doesn't land on
        // anything more specific, e.g. the room's image-pack grid).
        room_view_->media_upload_limit_provider = [this]() -> std::uint64_t
        {
            return client_ ? client_->media_upload_limit() : 0;
        };
        room_view_->media_info_extractor =
            [this](std::uint32_t gen, std::vector<std::uint8_t> b, std::string m)
        {
            extract_drop_media_(gen, std::move(b), std::move(m));
        };
        room_view_->on_file_drop_outcome =
            [this](tesseract::views::FileDropOutcome outcome)
        {
            if (outcome == tesseract::views::FileDropOutcome::TooLarge)
                show_status_message_(_("File exceeds the upload limit"));
        };
        main_app_surface_->set_on_file_drop_error(
            [this](std::string reason)
            {
                show_status_message_(std::move(reason));
            });

        room_view_->on_layout_changed = [this]
        {
            main_app_surface_->relayout();
        };
        main_app_->space_root()->on_layout_changed = [this]
        {
            if (main_app_surface_)
                main_app_surface_->relayout();
        };

        // on_send / on_send_reply / on_send_edit / on_send_image /
        // on_send_video / on_send_audio / on_send_file already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), a
        // verbatim port of this window's old on_send body including the
        // composer mention-draft-to-markdown conversion. on_send_reply/
        // on_send_edit now clear the composer immediately and report
        // failures via an async status-bar message (RoomPane::send_reply_/
        // send_edit_), rather than this window's old behavior of blocking
        // the UI thread on the synchronous SDK call and only clearing the
        // composer on success. on_send's own local error branch never
        // actually fired: ShellBase::dispatch_room_send_'s outcome always
        // reports success for the non-slash-command path (the actual send
        // happens asynchronously in a background task with the result
        // discarded).
        // on_send_image / on_send_video / on_send_audio / on_send_file
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_). Deliberate behavior change as part of
        // this refactor: the main window's sync client_->send_image()/etc.
        // (blocked the UI thread, showed an inline status-bar error on
        // failure) is replaced by RoomPane's async
        // client_->send_image_async()/etc. (matches what pop-outs always
        // did). Error reporting is NOT lost — ShellBase::handle_upload_complete_ui_
        // already shows a generic "Upload failed: ..." status message on
        // IEventHandler::on_upload_complete for every async send regardless
        // of which window/pane started it, routed through the same
        // on_show_status_message_ui_ -> status_bar_ label this block used to
        // update inline.
        room_view_->on_edit_cancelled = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->clear_compose_text();
        };
        room_view_->on_edit_prefill = [this](const std::string& body)
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(body);
                room_view_->set_current_text(body);
                room_text_area_->set_focused(true);
            }
        };
        // on_delete_requested / on_copy_event_source_requested /
        // on_reaction_toggled already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_) — equivalent bodies, so the
        // duplicate assignments that used to be here are gone. See
        // RoomPane.cpp's delete_event_/copy_event_source_to_clipboard_/
        // toggle_reaction_ for the shared implementation (toggle_reaction_
        // includes the same MSC4027 shortcode lookup this block used to do
        // inline).
        setup_link_clicked_(room_view_);
        room_view_->on_set_clipboard = [this](std::string_view t)
        {
            if (main_app_surface_)
                main_app_surface_->host().set_clipboard_text(t);
        };
        room_view_->on_selection_started = [this]()
        {
            if (main_app_surface_)
                main_app_surface_->host().release_focus_to_canvas();
        };
        main_app_->space_root()->on_copy_to_clipboard = [this](std::string t)
        {
            if (main_app_surface_)
                main_app_surface_->host().set_clipboard_text(t);
        };
        room_view_->message_list()->on_show_copy_menu = [this]()
        {
            if (!copy_ctx_menu_)
                build_copy_context_menu_();
            if (!copy_ctx_menu_)
                return;
            GtkWidget* w = main_app_surface_->widget();
            GdkDisplay* dpy = gtk_widget_get_display(w);
            GdkSeat* seat = gdk_display_get_default_seat(dpy);
            GdkDevice* ptr = gdk_seat_get_pointer(seat);
            GdkSurface* surf = gtk_native_get_surface(
                GTK_NATIVE(gtk_widget_get_native(w)));
            double sx = 0, sy = 0;
            if (surf)
                gdk_surface_get_device_position(surf, ptr, &sx, &sy, nullptr);
            graphene_point_t pt_in{static_cast<float>(sx), static_cast<float>(sy)};
            graphene_point_t pt_out{};
            if (!gtk_widget_compute_point(
                    GTK_WIDGET(gtk_widget_get_native(w)), w, &pt_in, &pt_out))
                pt_out = {};
            GdkRectangle r{static_cast<int>(pt_out.x), static_cast<int>(pt_out.y), 1, 1};
            gtk_popover_set_pointing_to(GTK_POPOVER(copy_ctx_menu_), &r);
            gtk_popover_popup(GTK_POPOVER(copy_ctx_menu_));
        };
        room_view_->on_link_hovered = [this](const std::string& url)
        {
            GtkWidget* w = main_app_surface_->widget();
            gtk_widget_set_cursor_from_name(w, url.empty() ? "default"
                                                           : "pointer");
        };
        // on_receipt_needed / on_member_pronoun_needed / on_near_top /
        // on_near_bottom / on_return_to_live / on_scroll_to_original already
        // provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_ + RoomPane::request_pagination_back_,
        // which now also calls set_paginating(true)/reset_near_top_latch()
        // to match this window's old request_more_history spinner behavior
        // — see RoomPane.cpp).
        room_view_->message_list()->on_tile_needed = [this](int z, int x, int y)
        {
            ensure_tile_async(z, x, y);
        };
        room_view_->on_date_jump = [this](std::uint64_t ts_ms)
        {
            handle_date_jump_(ts_ms);
        };
        room_view_->on_threads_button_clicked = [this]
        {
            on_threads_button_clicked();
        };
        // on_pin_requested / on_unpin_requested already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ ->
        // pin_event_/unpin_event_, equivalent to ShellBase::on_pin_requested/
        // on_unpin_requested's bodies).
        room_view_->on_thread_open_requested =
            [this](const std::string& root)
        {
            on_thread_open_requested(root);
        };
        room_view_->on_thread_close_requested = [this]
        {
            on_thread_close_requested();
        };
        room_view_->on_thread_send =
            [this](const std::string& body, const std::string& /*formatted*/)
        {
            // RoomView has no access to the native text area's mention/
            // emoticon draft, so it always passes an empty `formatted` here —
            // rebuild it the same way on_send does so thread sends keep
            // mentions and MSC2545 custom emoji instead of plain shortcode
            // text.
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->composer_draft()
                                : std::vector<tesseract::MentionSeg>{};
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            on_thread_send_requested(msg.body, msg.formatted_body);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_thread_send_reply =
            [this](const std::string& reply_id,
                   const std::string& body,
                   const std::string& /*formatted*/)
        {
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->composer_draft()
                                : std::vector<tesseract::MentionSeg>{};
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            on_thread_send_reply_requested(reply_id, msg.body,
                                           msg.formatted_body);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        // wire_room_view_picker_ (set_client/emoji+sticker picker image
        // providers/on_sticker_picked) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_'s
        // picker tail) — this call was fully redundant with it.
        // on_fetch_room_members / on_save_topic / on_leave_room /
        // on_room_settings_opened / on_room_settings_avatar_upload_requested
        // / room_settings_view()->on_accept already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ +
        // Deps.on_left_room, constructed above). on_save_topic now reports
        // failures via an async status message (RoomPane::wire_room_view_'s
        // on_save_topic), which this window never had before.
        room_view_->room_settings_view()->set_image_pack_provider(
            make_static_image_provider_with_fetch_(96, 96));
        room_view_->room_settings_view()->on_image_pack_images_needed =
            [this](std::string pack_id)
        { handle_image_pack_images_needed_(pack_id, room_view_->room_settings_view()); };
        room_view_->room_settings_view()->on_image_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        { handle_image_pack_pending_image_added_(local_id, bytes, mime, room_view_->room_settings_view()); };
        // Space-root settings (wrench icon on SpaceRootView): the same
        // per-room-id permission gating / accept / avatar-upload plumbing
        // above works unchanged for a space's room id — including image
        // packs, which are ordinary room state so a space can host its own
        // (only the Media tab is skipped, since it has no meaning for a
        // space).
        main_app_->space_root()->on_settings_opened = [this](std::string room_id)
        {
            auto* v = main_app_->space_root()->settings_view();
            if (!v) return;
            if (!client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                seed_image_pack_tab_(room_id, v);
                return;
            }
            v->set_field_permissions(client_->can_set_room_name(room_id),
                                     client_->can_set_room_topic(room_id),
                                     client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                client_->can_set_room_encryption(room_id),
                client_->can_set_room_join_rules(room_id),
                client_->can_set_room_guest_access(room_id),
                client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(client_->room_power_levels(room_id));
            v->set_own_power_level(client_->room_own_power_level(room_id));
            fetch_room_security_state_(room_id);
            seed_image_pack_tab_(room_id, v);
        };
        main_app_->space_root()->on_settings_avatar_upload_requested =
            [this](std::string room_id)
        {
            stage_room_settings_avatar_upload_(
                room_id, main_app_->space_root()->settings_view());
        };
        main_app_->space_root()->settings_view()->on_accept =
            [this](std::string room_id, tesseract::views::RoomSettingsChanges changes)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_(
                [this, c, room_id = std::move(room_id),
                 changes = std::move(changes)]() mutable
                {
                    auto outcome = ShellBase::apply_room_settings_(c, room_id, changes);
                    post_to_ui_([this, outcome, room_id,
                                 media_override = changes.media_override]() mutable
                    {
                        if (!main_app_) return;
                        if (auto* v = main_app_->space_root()->settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            commit_room_media_preview_override_(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
                });
        };
        main_app_->space_root()->settings_view()->set_image_pack_provider(
            make_static_image_provider_with_fetch_(96, 96));
        main_app_->space_root()->settings_view()->on_image_pack_images_needed =
            [this](std::string pack_id)
        {
            handle_image_pack_images_needed_(
                pack_id, main_app_->space_root()->settings_view());
        };
        main_app_->space_root()->settings_view()->on_image_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        {
            handle_image_pack_pending_image_added_(
                local_id, bytes, mime, main_app_->space_root()->settings_view());
        };
        setup_dm_callbacks();
        // on_ignore_user already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_).

        // Image + video viewers: providers / repaint / on_close come from
        // RoomPane::wire_room_view_ via main_room_pane_->attach() above; only
        // the video player is shell-specific (needs this window's Host),
        // same as every pop-out wires it directly in its own constructor.
        main_app_->video_viewer()->set_video_player(
            main_app_surface_->host().make_video_player());

        // on_image_clicked / on_avatar_clicked already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), which
        // uses this window's own Deps.grab_surface_focus
        // (gtk_widget_grab_focus) in place of the direct call this window
        // used to make.

        img_viewer_->on_save =
            [this](std::string source_url, std::string filename_hint)
        {
            std::string suggested = filename_hint.empty() ? "image" : filename_hint;
            GtkFileDialog* dlg = gtk_file_dialog_new();
            gtk_file_dialog_set_title(dlg, "Save image");
            gtk_file_dialog_set_initial_name(dlg, suggested.c_str());
            struct ImgSaveCtx
            {
                MainWindow* self;
                std::string source_url;
            };
            auto* ctx = new ImgSaveCtx{this, std::move(source_url)};
            gtk_file_dialog_save(dlg,
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                nullptr,
                +[](GObject* dialog_obj, GAsyncResult* res, gpointer p)
                {
                    auto* c = static_cast<ImgSaveCtx*>(p);
                    GError* err = nullptr;
                    GFile* gf = gtk_file_dialog_save_finish(
                        GTK_FILE_DIALOG(dialog_obj), res, &err);
                    if (gf)
                    {
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string url = std::move(c->source_url);
                        if (c->self->client_)
                        {
                            auto req_id = c->self->begin_media_req_(0,
                                [dest](std::vector<uint8_t> bytes) mutable
                                {
                                    if (!bytes.empty())
                                    {
                                        std::ofstream f(dest, std::ios::binary);
                                        f.write(
                                            reinterpret_cast<const char*>(
                                                bytes.data()),
                                            static_cast<std::streamsize>(
                                                bytes.size()));
                                    }
                                });
                            c->self->client_->fetch_source_bytes_async(req_id, url);
                        }
                    }
                    if (err) g_error_free(err);
                    delete c;
                },
                ctx);
            g_object_unref(dlg);
        };

        // on_video_clicked (both room_view_'s and room_media_view_'s
        // gallery-reuse alias) already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_, which aliases
        // room_media_view()->on_image_clicked/on_video_clicked to the same
        // handlers it installs on room_view()).

        vid_viewer_->on_save =
            [this](std::string source_json, std::string mime_type)
        {
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            GtkFileDialog* dlg = gtk_file_dialog_new();
            gtk_file_dialog_set_title(dlg, "Save video");
            gtk_file_dialog_set_initial_name(dlg, ("video" + ext).c_str());
            struct VidSaveCtx
            {
                MainWindow* self;
                std::string source_json;
            };
            auto* ctx = new VidSaveCtx{this, std::move(source_json)};
            gtk_file_dialog_save(dlg,
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                nullptr,
                +[](GObject* dialog_obj, GAsyncResult* res, gpointer p)
                {
                    auto* c = static_cast<VidSaveCtx*>(p);
                    GError* err = nullptr;
                    GFile* gf = gtk_file_dialog_save_finish(
                        GTK_FILE_DIALOG(dialog_obj), res, &err);
                    if (gf)
                    {
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string json_src = std::move(c->source_json);
                        if (c->self->client_)
                        {
                            auto req_id = c->self->begin_media_req_(0,
                                [dest](std::vector<uint8_t> bytes) mutable
                                {
                                    if (!bytes.empty())
                                    {
                                        std::ofstream f(dest, std::ios::binary);
                                        f.write(
                                            reinterpret_cast<const char*>(
                                                bytes.data()),
                                            static_cast<std::streamsize>(
                                                bytes.size()));
                                    }
                                });
                            c->self->client_->fetch_source_bytes_async(
                                req_id, json_src);
                        }
                    }
                    if (err) g_error_free(err);
                    delete c;
                },
                ctx);
            g_object_unref(dlg);
        };

        room_view_->on_file_clicked =
            [this](const tesseract::views::MessageListView::FileHit& hit)
        {
            std::string suggested =
                hit.file_name.empty() ? "download" : hit.file_name;
            GtkFileDialog* dlg = gtk_file_dialog_new();
            gtk_file_dialog_set_title(dlg, "Save file");
            gtk_file_dialog_set_initial_name(dlg, suggested.c_str());
            struct FileSaveCtx
            {
                MainWindow* self;
                std::string fetch_tok;
            };
            auto* ctx = new FileSaveCtx{this, hit.source ? hit.source->fetch_token() : std::string{}};
            gtk_file_dialog_save(dlg,
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                nullptr,
                +[](GObject* dialog_obj, GAsyncResult* res, gpointer p)
                {
                    auto* c = static_cast<FileSaveCtx*>(p);
                    GError* err = nullptr;
                    GFile* gf = gtk_file_dialog_save_finish(
                        GTK_FILE_DIALOG(dialog_obj), res, &err);
                    if (gf)
                    {
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string url = std::move(c->fetch_tok);
                        if (c->self->client_)
                        {
                            auto req_id = c->self->begin_media_req_(0,
                                [dest](std::vector<uint8_t> bytes) mutable
                                {
                                    if (!bytes.empty())
                                    {
                                        std::ofstream f(dest, std::ios::binary);
                                        f.write(
                                            reinterpret_cast<const char*>(
                                                bytes.data()),
                                            static_cast<std::streamsize>(
                                                bytes.size()));
                                    }
                                });
                            c->self->client_->fetch_source_bytes_async(req_id, url);
                        }
                    }
                    if (err) g_error_free(err);
                    delete c;
                },
                ctx);
            g_object_unref(dlg);
        };

        room_view_->set_video_player_factory(
            [this]()
            {
                return main_app_surface_->host().make_video_player();
            });
        room_view_->set_video_fetch_provider(
            [this](const std::string& src,
                   std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                if (client_)
                {
                    auto req_id = begin_media_req_(0,
                        [on_ready = std::move(on_ready)](
                            std::vector<std::uint8_t> bytes) mutable
                        {
                            on_ready(std::move(bytes));
                        });
                    client_->fetch_source_bytes_async(req_id, src);
                }
            });

        // Verification banner callbacks.
        verif_shared_->on_verify = [this]
        {
            if (client_)
            {
                client_->request_self_verification();
            }
        };
        verif_shared_->on_accept = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_match = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                if (verif_shared_)
                {
                    verif_shared_->set_state(
                        tesseract::views::VerificationBanner::State::
                            Confirming);
                }
                main_app_surface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_mismatch = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_cancel = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_dismiss = [this]
        {
            verification_banner_dismissed_ = true;
            main_app_->show_verif_banner(false);
            main_app_surface_->relayout();
        };
        verif_shared_->on_done = [this]
        {
            main_app_->show_verif_banner(false);
            main_app_surface_->relayout();
        };
        verif_shared_->on_use_recovery_key = [this]
        {
            main_app_->show_verif_banner(false);
            // The recovery-key entry path now lives in the encryption-setup
            // overlay (Recover mode); the old inline RecoveryBanner was removed.
            show_encryption_setup_overlay_(
                tesseract::views::EncryptionSetupOverlay::Mode::Recover);
        };

        // Room search field.
        if (auto* sf = main_app_->room_list_view()->search_field())
        {
            sf->set_on_changed(
                [this](const std::string& q)
                {
                    search_pending_text_ = q;
                    debounce_(DebounceSlot::RoomSearch,
                              tesseract::views::RoomListView::kSearchDebounceMs,
                              [this]
                              {
                                  if (room_list_view_)
                                  {
                                      room_list_view_->set_search_text(
                                          search_pending_text_);
                                  }
                                  refresh_room_list();
                              });
                });
        }

        // Quick switcher (Ctrl+K) — search field is self-owned; only the
        // shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* qsf = main_app_->quick_switcher()->search_field())
        {
            qsf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* qs = main_app_ ? main_app_->quick_switcher() : nullptr;
                    if (!qs || !qs->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        qs->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        qs->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_quick_switch_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (main_app_ && main_app_->quick_switcher())
            main_app_->quick_switcher()->on_close = [this]
            { close_quick_switch_(); };

        // Message search (Ctrl+Shift+F) — search field is self-owned; only
        // the shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* msf = main_app_->message_search()->search_field())
        {
            msf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* ms = main_app_ ? main_app_->message_search() : nullptr;
                    if (!ms || !ms->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        ms->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        ms->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_message_search_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (main_app_ && main_app_->message_search())
            main_app_->message_search()->on_close = [this]
            { close_message_search_(); };

        // Forward room picker — search field is self-owned; only the
        // shell-level Up/Down/Escape nav and on_close need wiring here.
        if (auto* fpf = main_app_->forward_picker()->search_field())
        {
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    auto* fp = main_app_ ? main_app_->forward_picker() : nullptr;
                    if (!fp || !fp->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        fp->move_selection(-1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        fp->move_selection(+1);
                        main_app_surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        close_forward_picker_();
                        return true;
                    default:
                        return false;
                    }
                });
        }
        if (main_app_ && main_app_->forward_picker())
            main_app_->forward_picker()->on_close = [this]
            { close_forward_picker_(); };

        // Per-room "find in conversation" (Ctrl+F) — search field is
        // self-owned; only the shell-level Up/Down/Escape nav and on_close
        // need wiring here.
        if (main_app_ && main_app_->room_view() && main_app_->room_view()->room_search_bar())
        {
            auto* bar = main_app_->room_view()->room_search_bar();
            if (auto* rif = bar->search_field())
            {
                rif->push_popup_nav(
                    [this](tk::NavKey nk) -> bool
                    {
                        auto* rv = main_app_ ? main_app_->room_view() : nullptr;
                        if (!rv || !rv->room_search_open())
                            return false;
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                            if (rv->on_room_search_navigate) rv->on_room_search_navigate(-1);
                            main_app_surface_->relayout();
                            return true;
                        case tk::NavKey::Down:
                            if (rv->on_room_search_navigate) rv->on_room_search_navigate(+1);
                            main_app_surface_->relayout();
                            return true;
                        case tk::NavKey::Escape:
                            close_find_in_room_();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
            bar->on_close = [this] { close_find_in_room_(); };
        }

        // room_text_area_ self-positions via ComposeBar's own arrange() and
        // is force-hidden by MainAppWidget::arrange() while any modal is
        // open — no set_on_layout wiring needed for it anymore.

        main_app_surface_->set_root(std::move(main_app_owner));
        // Wire the animation cache so note_image()/current_frame() work for
        // the main surface's own overlay-based anim repaints (see
        // repaint_anim_frame_ / host_gtk.cpp's live_overlays_). Previously
        // only the GIF popup surface needed this, since the main surface's
        // anim ticks used to go through a blind full request_repaint()
        // instead of the AnimDamageSink path.
        main_app_surface_->set_anim_cache(&account_manager_.anim_cache());
    }

    // Right-click on the chat area: hit-test sticker rects.
    {
        GtkGesture* gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed", G_CALLBACK(on_msg_right_click_),
                         this);
        gtk_widget_add_controller(main_app_surface_->widget(),
                                  GTK_EVENT_CONTROLLER(gesture));
    }

    GtkWidget* main_widget = main_app_surface_->widget();
    gtk_widget_set_hexpand(main_widget, TRUE);
    gtk_widget_set_vexpand(main_widget, TRUE);
    gtk_stack_add_named(GTK_STACK(content_stack_), main_widget, "main");

    // Settings page — populated on each open via open_settings_().
    {
        settings_widget_ = std::make_unique<gtk4::SettingsWidget>();
        GtkWidget* w = settings_widget_->widget();
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_vexpand(w, TRUE);
        gtk_stack_add_named(GTK_STACK(content_stack_), w, "settings");
        stats_settings_view_ = settings_widget_->settings_view();

        settings_widget_->on_close = [this]
        {
            stop_search_index_stats_poll_();
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
        };
        settings_widget_->on_logout = [this]
        {
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            logout_active_account();
        };
        settings_widget_->on_reset_identity = [this]
        {
            // The reset overlay lives on the main window — leave settings
            // first, then start the reset flow.
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            begin_crypto_identity_reset_();
        };
        settings_widget_->on_theme_changed =
            [this](tesseract::Settings::ThemePreference pref)
        {
            set_theme_preference_(pref);
        };
        settings_widget_->on_notifications_changed = [this](bool enabled)
        {
            if (settings_controller_)
                settings_controller_->set_notifications_enabled(enabled);
        };
        settings_widget_->on_launch_at_login_changed = [this](bool enabled)
        {
            handle_launch_at_login_toggle_(enabled);
        };
        settings_widget_->on_send_presence_changed = [this](bool enabled)
        {
            handle_send_presence_toggle_(enabled);
        };
        settings_widget_->on_index_messages_changed = [this](bool enabled)
        {
            handle_index_messages_toggle_(enabled);
        };
#ifdef TESSERACT_GITHUB_REPO
        settings_widget_->on_check_for_updates_changed = [this](bool enabled)
        {
            handle_check_for_updates_toggle_(enabled);
        };
#endif
        settings_widget_->on_msc2545_legacy_compat_changed = [this](bool enabled)
        {
            handle_msc2545_legacy_compat_toggle_(enabled);
        };
        settings_widget_->on_developer_mode_changed = [this](bool enabled)
        {
            handle_developer_mode_toggle_(enabled);
        };
#ifdef TESSERACT_CRASH_HANDLER_ENABLED
        settings_widget_->on_crash_reporting_changed = [this](bool enabled)
        {
            handle_crash_reporting_toggle_(enabled);
        };
#endif
        settings_widget_->on_send_maps_urls_as_location_changed = [this](bool enabled)
        {
            handle_send_maps_urls_as_location_toggle_(enabled);
        };
        settings_widget_->on_media_previews_changed =
            [this](tesseract::Settings::MediaPreviews mode)
        {
            apply_media_preview_config_(
                mode, tesseract::Settings::instance().invite_avatars);
        };
        settings_widget_->on_invite_avatars_changed = [this](bool enabled)
        {
            apply_media_preview_config_(
                tesseract::Settings::instance().media_previews, enabled);
        };
        settings_widget_->on_group_inactive_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_inactive_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_widget_->on_group_unread_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_unread_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_widget_->on_inactive_period_changed = [this](int days)
        {
            auto& s = tesseract::Settings::instance();
            s.inactive_room_threshold_days = days;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_widget_->on_autoscroll_unread_changed = [](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.autoscroll_unread_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
        };
        settings_widget_->on_show_membership_events_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.show_room_join_leave_events = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (client_) client_->set_show_membership_events(enabled);
            if (client_ && !current_room_id_.empty())
                client_->subscribe_room(current_room_id_);
        };
        settings_widget_->on_clear_caches = [this]
        {
            clear_all_caches_(
                [this](uint64_t local, uint64_t sdk, uint64_t memory,
                       uint64_t mh, uint64_t mm, uint64_t dh, uint64_t dm)
            {
                if (settings_widget_)
                    settings_widget_->set_cache_sizes(local, sdk, memory,
                                                      mh, mm, dh, dm);
            });
        };
        settings_widget_->on_local_avatar_changed =
            [this](std::string new_mxc)
        {
            my_avatar_url_ = new_mxc;
            if (active_account_)
            {
                active_account_->avatar_url = my_avatar_url_;
            }
            populate_user_strip();
        };
        settings_widget_->settings_view()->on_profile_field_changed =
            [this](std::string key, std::string value_json)
        {
            handle_profile_field_change_(key, value_json);
        };

        // Populate capture-device combos in the Media section.
        {
            auto& host = main_app_surface_->host();
            auto* sv   = settings_widget_->settings_view();
            sv->set_audio_input_devices(host.enumerate_audio_inputs());
            sv->set_audio_output_devices(host.enumerate_audio_outputs());
            sv->set_camera_devices(host.enumerate_cameras());
            sv->set_selected_audio_input(
                tesseract::Settings::instance().audio_input_device_id);
            sv->set_selected_audio_output(
                tesseract::Settings::instance().audio_output_device_id);
            sv->set_selected_camera(
                tesseract::Settings::instance().camera_device_id);
            sv->on_audio_input_changed = [this](std::string id)
            {
                tesseract::Settings::instance().audio_input_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            sv->on_audio_output_changed = [this](std::string id)
            {
                tesseract::Settings::instance().audio_output_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            sv->on_camera_changed = [this](std::string id)
            {
                tesseract::Settings::instance().camera_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
        }
    }

    // Escape key: close viewer overlays. Attached to the window so it fires
    // regardless of which widget holds focus.
    {
        GtkEventController* key_ctl = gtk_event_controller_key_new();
        g_signal_connect(key_ctl, "key-pressed",
                         G_CALLBACK(on_window_key_pressed_), this);
        gtk_widget_add_controller(window_, key_ctl);
    }

    // Ctrl+K opens the quick switcher. A global-scope GtkShortcutController
    // fires even while a native entry / text view holds focus — the bubble-
    // phase key controller above lets the focused widget swallow Ctrl+K first.
    {
        GtkEventController* sc = gtk_shortcut_controller_new();
        gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(sc),
                                          GTK_SHORTCUT_SCOPE_GLOBAL);
        GtkShortcut* shortcut = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_k, GDK_CONTROL_MASK),
            gtk_callback_action_new(on_quick_switch_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             shortcut);

        // Ctrl+Shift+F: open global message search. GTK normalizes a shifted
        // key event to the unshifted lowercase keyval before matching, so the
        // trigger keyval must be lowercase `f` (mirrors the Ctrl+K trigger).
        GtkShortcut* search_sc = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_f,
                                   GdkModifierType(GDK_CONTROL_MASK |
                                                   GDK_SHIFT_MASK)),
            gtk_callback_action_new(on_message_search_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             search_sc);

        // Ctrl+F: open per-room "find in conversation".
        GtkShortcut* fir_sc = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
            gtk_callback_action_new(on_find_in_room_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             fir_sc);

        GtkShortcut* back_sc = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_Left, GDK_ALT_MASK),
            gtk_callback_action_new(on_nav_back_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             back_sc);

        GtkShortcut* fwd_sc = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_Right, GDK_ALT_MASK),
            gtk_callback_action_new(on_nav_fwd_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             fwd_sc);

        gtk_widget_add_controller(window_, sc);
    }

    // Status bar floats below the main stack (outside the stack so it is
    // always visible regardless of which page is shown).
    status_bar_ = gtk_label_new(_("Not logged in"));
    gtk_widget_set_hexpand(status_bar_, TRUE);
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_widget_set_margin_bottom(status_bar_, 2);
    gtk_label_set_ellipsize(GTK_LABEL(status_bar_), PANGO_ELLIPSIZE_END);
    // Hyperlinked status messages (see app/status_links.h) render via Pango
    // markup; route activation through the shared opener instead of
    // gtk_show_uri so all platforms share one code path.
    g_signal_connect(status_bar_, "activate-link",
                     G_CALLBACK(+[](GtkLabel*, const char* uri,
                                    gpointer) -> gboolean
                                {
                                    tesseract::Client::open_in_browser(
                                        uri ? uri : "");
                                    return TRUE;
                                }),
                     nullptr);
    // Generic click (distinct from "activate-link" above, which only fires
    // for actual http(s) link segments). Safe unconditionally:
    // trigger_persistent_status_click_() no-ops unless something
    // (currently: an in-progress history export) claimed the persistent-
    // status slot.
    {
        GtkGesture* status_click = gtk_gesture_click_new();
        g_signal_connect(status_click, "released",
                         G_CALLBACK(+[](GtkGestureClick*, int, double, double,
                                        gpointer data)
                                    {
                                        static_cast<MainWindow*>(data)
                                            ->trigger_persistent_status_click_();
                                    }),
                         this);
        gtk_widget_add_controller(status_bar_,
                                  GTK_EVENT_CONTROLLER(status_click));
    }
    inflight_dot_ = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(inflight_dot_),
        [](GtkDrawingArea*, cairo_t* cr, int /*w*/, int /*h*/, gpointer data)
        {
            static_cast<MainWindow*>(data)->draw_inflight_dot_(cr);
        },
        this, nullptr);
    gtk_widget_set_size_request(inflight_dot_,
                               static_cast<int>(tk::kInflightViewSize),
                               static_cast<int>(tk::kInflightViewSize));
    gtk_widget_set_margin_end(inflight_dot_, 4);
    gtk_widget_set_margin_bottom(inflight_dot_, 2);
    {
        // Wrap content_stack_ + status row in an outer vbox so the status
        // bar stays below the stack on all pages.
        GtkWidget* status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append(GTK_BOX(status_row), status_bar_);
        gtk_box_append(GTK_BOX(status_row), inflight_dot_);
        GtkWidget* outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        // Reparent: the constructor already set content_stack_ as child of
        // window_; swap it out for the outer vbox.
        g_object_ref(content_stack_);
        gtk_window_set_child(GTK_WINDOW(window_), nullptr);
        gtk_box_append(GTK_BOX(outer_vbox), content_stack_);
        g_object_unref(content_stack_);
        gtk_box_append(GTK_BOX(outer_vbox), status_row);
        gtk_window_set_child(GTK_WINDOW(window_), outer_vbox);
        init_pool_callbacks_();
        on_inflight_ui_();
    }

    // The GdkSurface (and its GdkToplevel:state property, which is_main_window_visible_()
    // reads for the minimized bit) doesn't exist until the widget is realized —
    // catch that via "realize" rather than assuming it's already there.
    g_signal_connect(window_, "realize",
                     G_CALLBACK(+[](GtkWidget* w, gpointer data)
                                {
                                    auto* self = static_cast<MainWindow*>(data);
                                    auto* surf = gtk_native_get_surface(
                                        GTK_NATIVE(w));
                                    if (!surf)
                                        return;
                                    g_signal_connect(
                                        surf, "notify::state",
                                        G_CALLBACK(
                                            +[](GdkToplevel*, GParamSpec*,
                                               gpointer d)
                                            {
                                                static_cast<MainWindow*>(d)
                                                    ->update_video_playback_suspension_();
                                            }),
                                        self);
                                }),
                     this);

    if (!start_hidden_)
        gtk_widget_set_visible(window_, TRUE);

    // Notifiers are created per-account in do_login / on_login_succeeded.

    g_signal_connect(window_, "close-request",
                     G_CALLBACK(&MainWindow::on_window_close_request_), this);

    // GTK4 has no gtk_window_set_urgency_hint (removed, not deprecated), so
    // the "visible but unfocused" attention request is delivered as a
    // GNotification instead (see handle_notification). Withdraw it when the
    // user brings the window to the front, mirroring the urgency-hint clear
    // other backends do.
    g_signal_connect(window_, "notify::is-active",
                     G_CALLBACK(+[](GtkWindow* w, GParamSpec*, gpointer data)
                                {
                                    auto* self = static_cast<MainWindow*>(data);
                                    const bool active = gtk_window_is_active(w);
                                    if (active && self->app_)
                                    {
                                        g_application_withdraw_notification(
                                            G_APPLICATION(self->app_),
                                            kAttentionNotifId);
                                    }
                                    self->notify_window_active_(active);
                                    if (active)
                                        self->start_anim_tick_if_needed_();
                                }),
                     this);

    // 30 s periodic tick — granular enough for a 5 min idle threshold without
    // burning CPU. Returns G_SOURCE_CONTINUE to auto-reschedule; the source
    // id is stashed so ~MainWindow can g_source_remove() it (matching the
    // existing tk_anim_tick_id_ / mark_read_timer_id_ pattern).
    presence_tick_id_ = g_timeout_add_seconds(
        30,
        +[](gpointer data) -> gboolean
        {
            static_cast<MainWindow*>(data)->notify_presence_tick_();
            return G_SOURCE_CONTINUE;
        },
        this);

    // Session bus for the XDG Desktop Portal color-scheme query. A dedicated
    // connection (rather than sharing one with e.g. LinuxNotifierGtk) matches
    // the existing per-subsystem pattern in this backend (LinuxScreenLockGtk,
    // GtkSniTrayIcon, ...).
    portal_bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (portal_bus_)
    {
        portal_setting_changed_sub_ = g_dbus_connection_signal_subscribe(
            portal_bus_, "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Settings", "SettingChanged",
            "/org/freedesktop/portal/desktop", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            on_portal_setting_changed_, this, nullptr);
    }
    read_portal_color_scheme_();

    apply_current_theme_();

    // Re-apply when the OS dark-mode setting changes (System mode only).
    // Store the handler ID so apply_theme_ui_ can block it while writing
    // the same property, preventing a notify → apply → notify feedback loop.
    prefer_dark_notify_id_ = g_signal_connect(
        gtk_settings_get_default(), "notify::gtk-application-prefer-dark-theme",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data)
                   {
                       auto* self = static_cast<MainWindow*>(data);
                       if (tesseract::Settings::instance().theme_pref ==
                           tesseract::Settings::ThemePreference::System)
                       {
                           self->apply_current_theme_();
                       }
                   }),
        this);

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (screenshot_dir_.empty())
#endif
        gtk_post_idle(guarded([this] { do_login(); }));

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
bool MainWindow::save_screenshot_(const char* filename)
{
    GdkPaintable* paintable = gtk_widget_paintable_new(window_);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    const int width = gtk_widget_get_width(window_);
    const int height = gtk_widget_get_height(window_);
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot),
                           width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(window_));
    GdkTexture* texture = node && renderer
        ? gsk_renderer_render_texture(renderer, node, nullptr)
        : nullptr;

    const auto path = screenshot_dir_ / filename;
    const bool ok = texture && gdk_texture_save_to_png(texture, path.string().c_str());
    if (texture)
        g_object_unref(texture);
    if (node)
        gsk_render_node_unref(node);
    g_object_unref(paintable);
    if (!ok)
        g_printerr("Could not save screenshot: %s\n", path.string().c_str());
    return ok;
}

void MainWindow::start_screenshot_mode()
{
    auto fixture = tesseract::screenshot::make_fixture();
    my_user_id_ = std::move(fixture.user_id);
    my_display_name_ = std::move(fixture.display_name);
    my_avatar_url_ = std::move(fixture.avatar_url);
    rooms_ = std::move(fixture.rooms);
    current_room_id_ = std::move(fixture.selected_room_id);

    if (!tesseract::screenshot::install_avatar_assets(
            main_app_surface_->factory(), account_manager_.thumbnail_cache()))
    {
        g_printerr("Could not load screenshot avatar assets\n");
        g_application_quit(G_APPLICATION(app_));
        return;
    }

    main_app_->show_room();
    show_rooms(rooms_);
    for (const auto& room : rooms_)
        if (room.id == current_room_id_)
        {
            room_view_->set_room(room);
            break;
        }
    room_view_->set_messages(std::move(fixture.messages));
    populate_user_strip();
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 768);

    std::error_code ec;
    std::filesystem::create_directories(screenshot_dir_, ec);
    if (ec)
    {
        g_printerr("Could not create screenshot directory: %s\n",
                   screenshot_dir_.string().c_str());
        g_application_quit(G_APPLICATION(app_));
        return;
    }

    apply_theme_ui_(tk::Theme::light());
    main_app_surface_->relayout();
    gtk_window_present(GTK_WINDOW(window_));
    g_timeout_add(
        300,
        +[](gpointer data) -> gboolean
        {
            auto* self = static_cast<MainWindow*>(data);
            if (!self->save_screenshot_("gtk4-light.png"))
            {
                g_application_quit(G_APPLICATION(self->app_));
                return G_SOURCE_REMOVE;
            }
            self->apply_theme_ui_(tk::Theme::dark());
            self->main_app_surface_->relayout();
            g_timeout_add(
                300,
                +[](gpointer inner) -> gboolean
                {
                    auto* window = static_cast<MainWindow*>(inner);
                    window->save_screenshot_("gtk4-dark.png");
                    g_application_quit(G_APPLICATION(window->app_));
                    return G_SOURCE_REMOVE;
                },
                self);
            return G_SOURCE_REMOVE;
        },
        this);
}
#endif

void MainWindow::start_tray_if_needed_()
{
    if (tray_)
    {
        return;
    }
    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!account_manager_.claim_tray_owner(this))
    {
        return;
    }
    tray_ = std::make_unique<GtkSniTrayIcon>(
        [this]
        {
            // If the unread room is popped out, raise that window instead.
            if (focus_tray_unread_popout_())
                return;
            if (gtk_widget_get_visible(window_) &&
                gtk_window_is_active(GTK_WINDOW(window_)) &&
                !last_tray_unread_)
            {
                gtk_widget_set_visible(window_, FALSE);
                update_video_playback_suspension_();
                return;
            }
            gtk_window_present(GTK_WINDOW(window_));
            update_video_playback_suspension_();
            navigate_tray_unread_();
        },
        [this]
        {
            // Real quit: drop the tray so close-request falls through to
            // the default (window destroyed → app holds nothing → quits).
            tray_.reset();
            g_application_quit(G_APPLICATION(app_));
        });
    if (tray_->is_available())
    {
        // Keep the GApplication alive when the window is hidden.
        g_application_hold(G_APPLICATION(app_));
        // Seed the new tray with the current aggregate so an already-unread
        // state shows immediately rather than waiting for the next sync tick
        // to flip on_tray_unread_changed_.
        tray_->set_unread(last_tray_unread_, last_tray_highlight_);
    }
    else
    {
        tray_.reset();
        // No SNI host: relinquish ownership so another window may retry later.
        account_manager_.release_tray_owner(this);
    }
}

void MainWindow::start_search_provider_if_needed_()
{
    if (search_provider_)
    {
        return;
    }
    // Exactly one window owns the single app-wide search-provider D-Bus
    // object (multi-window), mirroring start_tray_if_needed_.
    if (!account_manager_.claim_search_provider_owner(this))
    {
        return;
    }
    search_provider_ = std::make_unique<GtkSearchProviderGtk>(account_manager_);
    if (!search_provider_->is_available())
    {
        search_provider_.reset();
        account_manager_.release_search_provider_owner(this);
    }
}

void MainWindow::start_mpris_if_needed_()
{
    if (mpris_)
    {
        return;
    }
    // Exactly one window owns the single app-wide MPRIS D-Bus object
    // (multi-window), mirroring start_tray_if_needed_.
    if (!account_manager_.claim_mpris_owner(this))
    {
        return;
    }
    mpris_ = std::make_unique<GtkMprisPlayer>(account_manager_);
    if (!mpris_->is_available())
    {
        mpris_.reset();
        account_manager_.release_mpris_owner(this);
    }
}

gboolean MainWindow::on_window_close_request_(GtkWindow* /*window*/,
                                              gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->tray_ && self->tray_->is_available())
    {
        gtk_widget_set_visible(self->window_, FALSE);
        self->update_video_playback_suspension_();
        return TRUE; // stop default destruction
    }
    // Hand this window's account bridge back to the primary, release its
    // dedicated mapping and tray ownership (multi-window), then unregister.
    self->on_window_closing_();
    self->account_manager_.unregister_window(self);
    if (self->account_manager_.window_count() == 0)
    {
        g_application_quit(G_APPLICATION(self->app_));
    }
    else
    {
        // Spawned window: free C++ resources now; GTK will destroy the widget
        // after this handler returns FALSE.
        delete self;
    }
    return FALSE; // let GTK destroy the GtkWidget
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
    if (portal_color_scheme_ != -1)
    {
        return portal_color_scheme_ == 1 ? tk::ThemeMode::Dark
                                         : tk::ThemeMode::Light;
    }
    // Portal unreachable (no xdg-desktop-portal running). Fall back to
    // whatever GtkSettings has — usually only meaningful if something like
    // kde-gtk-config populated it from ~/.config/gtk-4.0/settings.ini at
    // GTK init time, or if the app itself wrote it in apply_theme_ui_.
    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    return prefer_dark ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
}

void MainWindow::read_portal_color_scheme_()
{
    if (!portal_bus_)
    {
        return;
    }
    // ReadOne (portal interface v2+), not the deprecated Read: Read
    // double-wraps its return value in an extra D-Bus variant layer.
    GVariant* reply = g_dbus_connection_call_sync(
        portal_bus_, "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
        "ReadOne",
        g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    if (!reply)
    {
        return;
    }
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    if (value)
    {
        portal_color_scheme_ = g_variant_get_uint32(value);
        g_variant_unref(value);
    }
    g_variant_unref(reply);
}

void MainWindow::on_portal_setting_changed_(GDBusConnection*, const char*,
                                            const char*, const char*,
                                            const char*, GVariant* parameters,
                                            gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    const char* ns = nullptr;
    const char* key = nullptr;
    GVariant* value = nullptr;
    // SettingChanged signal format: (ssv) — namespace, key, new value.
    g_variant_get(parameters, "(&s&sv)", &ns, &key, &value);
    if (std::string(ns) != "org.freedesktop.appearance" ||
        std::string(key) != "color-scheme")
    {
        if (value)
            g_variant_unref(value);
        return;
    }
    self->portal_color_scheme_ = g_variant_get_uint32(value);
    g_variant_unref(value);
    if (tesseract::Settings::instance().theme_pref ==
        tesseract::Settings::ThemePreference::System)
    {
        self->apply_current_theme_();
    }
}

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    if (branding_surface_)
    {
        branding_surface_->set_theme(t);
        branding_surface_->root()->apply_theme(t);
    }
    if (main_app_surface_)
    {
        main_app_surface_->set_theme(t);
        main_app_surface_->root()->apply_theme(t);
    }
    if (account_picker_surface_)
    {
        account_picker_surface_->set_theme(t);
        account_picker_surface_->root()->apply_theme(t);
    }
    if (settings_widget_)
    {
        settings_widget_->set_theme(t);
    }
    if (slash_popup_)
    {
        slash_popup_->set_theme(t);
    }
    if (shortcode_popup_)
    {
        shortcode_popup_->set_theme(t);
    }
    if (mention_popup_)
    {
        mention_popup_->set_theme(t);
    }
    if (login_view_)
    {
        login_view_->set_theme(t);
    }

    // Pop-out room windows track the theme too.
    apply_theme_to_secondary_windows_(t);

    // Tell GTK itself about the dark preference so native chrome follows.
    // Block the notify handler while writing to prevent a feedback loop:
    // our own g_object_set would re-trigger apply_current_theme_ indefinitely.
    bool dark = (t.mode == tk::ThemeMode::Dark);
    if (prefer_dark_notify_id_)
        g_signal_handler_block(gtk_settings_get_default(), prefer_dark_notify_id_);
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", dark ? TRUE : FALSE,
                 nullptr);
    if (prefer_dark_notify_id_)
        g_signal_handler_unblock(gtk_settings_get_default(), prefer_dark_notify_id_);

    // Rebuild dynamic CSS rules. The compose-area rule is static but lives
    // here because load_from_string replaces all prior content in the provider.
    if (theme_css_provider_)
    {
        char css[512];
        std::snprintf(css, sizeof(css),
                      ".sidebar { background-color: #%02x%02x%02x; }\n"
                      ".sidebar-separator { background-color: #%02x%02x%02x; "
                      "min-width: 1px; }\n"
                      "textview.compose-area,"
                      "textview.compose-area text {"
                      " background: transparent; }\n",
                      t.palette.sidebar_bg.r, t.palette.sidebar_bg.g,
                      t.palette.sidebar_bg.b, t.palette.separator.r,
                      t.palette.separator.g, t.palette.separator.b);
        gtk_css_provider_load_from_string(theme_css_provider_, css);
    }
}

MainWindow::~MainWindow()
{
    // Invalidate all outstanding g_idle_add / g_timeout_add payloads that
    // captured a weak_flag() from this shell's inherited EnableWeakSelf
    // guard. Doing this first, before anything else (including
    // broadcast_rebuild_tray_ below and ~ShellBase's own later call — the
    // latter a harmless no-op by then), ensures no idle fires on a
    // half-destroyed `this`.
    invalidate_weak_self();

    // unregister_window is called in on_window_close_request_ so it is not
    // repeated here.  broadcast_rebuild_tray_ is still needed to refresh any
    // remaining windows' tray menus after this C++ shell is freed.
    broadcast_rebuild_tray_();

    if (theme_css_provider_)
    {
        g_object_unref(theme_css_provider_);
        theme_css_provider_ = nullptr;
    }
    if (portal_bus_)
    {
        if (portal_setting_changed_sub_)
        {
            g_dbus_connection_signal_unsubscribe(portal_bus_,
                                                 portal_setting_changed_sub_);
        }
        g_object_unref(portal_bus_);
        portal_bus_ = nullptr;
    }
    if (scroll_debounce_id_)
    {
        g_source_remove(scroll_debounce_id_);
        scroll_debounce_id_ = 0;
    }
    if (tk_anim_tick_id_)
    {
        g_source_remove(tk_anim_tick_id_);
        tk_anim_tick_id_ = 0;
    }
    if (presence_tick_id_)
    {
        g_source_remove(presence_tick_id_);
        presence_tick_id_ = 0;
    }
    if (sync_status_debounce_id_)
    {
        g_source_remove(sync_status_debounce_id_);
        sync_status_debounce_id_ = 0;
    }
    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the surviving
    // windows; it still drains its own per-window pools below.
    if (!is_pinned_window_)
    {
        // Signal every account first, in its own pass, before any stop_sync()
        // call below: request_stop() only needs a shared FFI lock, so it wins
        // the race against a concurrent send_message/subscribe_room on that
        // (or any other) account instead of queuing behind stop_sync()'s
        // exclusive lock — see Client::request_stop().
        for (auto& sess : account_manager_.accounts())
        {
            if (sess->sync_started)
                sess->client->request_stop();
        }
        for (auto& sess : account_manager_.accounts())
        {
            if (sess->sync_started)
                sess->client->stop_sync();
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->request_stop();
        pending_login_client_->stop_sync();
    }
    pool_.drain();
    mut_pool_.drain();
    // login_view_ holds pending_login_client_* — destroy it before
    // pending_login_client_ and the accounts vector.
    login_view_.reset();
    pending_login_client_.reset();
}

// ---------------------------------------------------------------------------

void MainWindow::finish_login_ui_(const std::string& uid)
{
    switch_active_account(uid);
    ensure_settings_controller_();
    ensure_history_export_controller_();
    wire_history_export_dialog_callbacks_();
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    start_tray_if_needed_();
    start_search_provider_if_needed_();
    start_mpris_if_needed_();
}

void MainWindow::do_login()
{
    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (is_secondary_window_startup_())
    {
        finish_login_ui_(active_account_->user_id);
        return;
    }

    gtk_label_set_text(GTK_LABEL(status_bar_), _("Restoring session\xe2\x80\xa6"));

    // Pre-flight OS-level connectivity check — see tk::Host::
    // is_network_available()'s doc comment. Computed here, on the UI
    // thread, and threaded through so the worker-thread restore loop below
    // never touches Host.
    const bool network_available = branding_surface_->host().is_network_available();

    // Migrate + restore every stored account (shared loop in ShellBase), off
    // the UI thread so the window stays responsive. The native per-account
    // notifier / UnifiedPush construction runs through
    // install_account_notifier_ / install_account_up_connector_ below.
    restore_all_accounts_async_(
        [this](RestoreResult restore)
        {
            if (restore.any_accounts)
            {
                finish_login_ui_(restore.active_uid);
                return;
            }

            // No accounts: fresh install or all restores failed → show login
            // view. Nothing to silently restore, so an autostart launch
            // can't stay hidden — the user needs to log in.
            if (start_hidden_)
            {
                start_hidden_ = false;
                gtk_widget_set_visible(window_, TRUE);
            }
            pending_login_is_add_account_ = false;
            pending_login_temp_dir_.clear();
            pending_login_client_ = std::make_unique<tesseract::Client>();
            login_view_->set_client(pending_login_client_.get());
            login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
            login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
            login_view_->reset();
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Not logged in"));
            if (restore.any_restore_failed)
            {
                if (restore.network_unavailable)
                    login_view_->show_offline_error([this] { do_login(); });
                else
                    login_view_->show_restore_error(restore.restore_error,
                                                    [this] { do_login(); });
            }
        },
        network_available);
}

std::unique_ptr<tesseract::IEventHandler>
MainWindow::make_account_bridge_(const std::string& uid)
{
    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(uid);
    return bridge;
}

void MainWindow::install_account_notifier_(tesseract::AccountSession& session)
{
    // Per-account notifier: click switches to this account then navigates.
    const std::string notif_uid = session.user_id;
    session.notifier = std::make_unique<LinuxNotifierGtk>(
        [this, notif_uid](std::string room_id, std::string token)
        {
            switch_active_account(notif_uid);
            // Set xdg_activation_v1 token (non-empty on modern Wayland)
            // before gtk_window_present so the compositor grants focus.
            if (!token.empty())
            {
                gtk_window_set_startup_id(GTK_WINDOW(window_), token.c_str());
            }
            navigate_to_room(std::move(room_id));
        },
        [this, notif_uid](std::string room_id, std::string event_id,
                          std::string reply_text)
        {
            // Deliberately does not switch account or navigate — matches
            // KDE's own reply UX of not raising the app on submit.
            send_notification_reply_(notif_uid, std::move(room_id),
                                     std::move(event_id),
                                     std::move(reply_text));
        });
}

void MainWindow::install_account_up_connector_(tesseract::AccountSession& session)
{
    // Per-account UnifiedPush connector.
    auto up = std::make_unique<LinuxUpConnectorGtk>();
    up->start(session.client.get(), session.user_id);
    session.up_connector = std::move(up);
}

std::unique_ptr<tk::AudioPlayback> MainWindow::make_call_audio_output_()
{
    return main_app_surface_ ? main_app_surface_->host().make_audio_playback() : nullptr;
}

tesseract::CallWindowBase* MainWindow::create_call_window_()
{
    return new gtk4::CallWindow(this);
}

void MainWindow::on_login_succeeded()
{
    if (!pending_login_client_)
    {
        return; // defensive
    }

    // The LoginView holds a raw alias to pending_login_client_; clear it before
    // finalize_login_async_ moves the client out from under us.
    login_view_->set_client(nullptr);

    // Agnostic add-account core — runs off the UI thread. See
    // ShellBase::finalize_login_async_.
    finalize_login_async_(
        [this](FinalizeLoginResult fin)
        {
            if (fin.rejected_duplicate)
            {
                gtk_label_set_text(
                    GTK_LABEL(status_bar_),
                    ("Already signed in as " + fin.user_id).c_str());
                if (pending_login_is_add_account_ && add_account_return_idx_ >= 0 &&
                    add_account_return_idx_ <
                        static_cast<int>(account_manager_.accounts().size()))
                {
                    switch_active_account(
                        account_manager_.accounts()[add_account_return_idx_]
                            ->user_id);
                    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_),
                                                     "main");
                }
                pending_login_is_add_account_ = false;
                add_account_return_idx_ = -1;
                return;
            }

            if (!fin.ok)
            {
                gtk_label_set_text(
                    GTK_LABEL(status_bar_),
                    (std::string(_("Login error: ")) + fin.error).c_str());
                return;
            }

            switch_active_account(fin.user_id);
            ensure_settings_controller_();
            ensure_history_export_controller_();
            wire_history_export_dialog_callbacks_();
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            start_tray_if_needed_();
            start_search_provider_if_needed_();
            start_mpris_if_needed_();
            pending_login_is_add_account_ = false;
            add_account_return_idx_ = -1;
        });
}

void MainWindow::bind_settings_controller_()
{
    // settings_controller_ is freshly constructed by
    // ShellBase::ensure_settings_controller_(); install the native key/file
    // dialog hooks and bind it to the native settings widget.
    wire_key_dialog_callbacks_();
    if (settings_widget_)
    {
        settings_widget_->set_controller(settings_controller_.get());
        if (!own_extended_profile_.pronouns.empty() ||
            !own_extended_profile_.tz.empty() ||
            !own_extended_profile_.biography.empty())
            settings_widget_->set_extended_profile(own_extended_profile_);
        settings_widget_->settings_view()->set_user_pack_image_provider(
            make_static_image_provider_with_fetch_(96, 96));
        settings_widget_->settings_view()->on_user_pack_pending_image_added =
            [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                  const std::string& mime)
        {
            handle_user_pack_pending_image_added_(
                local_id, bytes, mime,
                settings_widget_->settings_view()->user_pack_editor());
        };
    }
}

void MainWindow::wire_history_export_dialog_callbacks_()
{
    if (!history_export_controller_)
        return;
    history_export_controller_->show_save_folder_dialog =
        [this](std::string /*suggested_name*/, std::function<void(std::string)> cb)
    {
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Choose a folder for the exported history");

        struct FolderCtx { std::function<void(std::string)> cb; };
        auto* ctx = new FolderCtx{std::move(cb)};
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(window_), nullptr,
            +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
            {
                auto* c = static_cast<FolderCtx*>(data);
                GError* err = nullptr;
                GFile* file = gtk_file_dialog_select_folder_finish(
                    GTK_FILE_DIALOG(dialog_obj), res, &err);
                if (file)
                {
                    char* path = g_file_get_path(file);
                    if (path) { c->cb(std::string(path)); g_free(path); }
                    g_object_unref(file);
                }
                if (err) g_error_free(err);
                delete c;
            },
            ctx);
        g_object_unref(dlg);
    };
}

void MainWindow::wire_key_dialog_callbacks_()
{
    settings_controller_->show_passphrase_prompt =
        [this](std::string title, std::function<void(std::string)> cb)
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GtkWidget* dlg = gtk_dialog_new_with_buttons(
            title.c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
        GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        G_GNUC_END_IGNORE_DEPRECATIONS
        GtkWidget* entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Passphrase");
        gtk_widget_set_margin_start(entry, 12);
        gtk_widget_set_margin_end(entry, 12);
        gtk_widget_set_margin_top(entry, 8);
        gtk_widget_set_margin_bottom(entry, 8);
        gtk_box_append(GTK_BOX(content), entry);
        gtk_window_present(GTK_WINDOW(dlg));

        struct PassphraseCtx {
            std::function<void(std::string)> cb;
            GtkWidget* entry;
        };
        auto* ctx = new PassphraseCtx{std::move(cb), entry};
        g_signal_connect(
            dlg, "response",
            G_CALLBACK(+[](GtkDialog* d, int resp, gpointer data)
            {
                auto* c = static_cast<PassphraseCtx*>(data);
                if (resp == GTK_RESPONSE_OK)
                {
                    const char* text =
                        gtk_editable_get_text(GTK_EDITABLE(c->entry));
                    if (text && text[0] != '\0')
                        c->cb(std::string(text));
                }
                delete c;
                gtk_window_destroy(GTK_WINDOW(d));
            }),
            ctx);
    };

    settings_controller_->show_save_file_dialog =
        [this](std::string suggested_name, std::function<void(std::string)> cb)
    {
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Save room keys");
        gtk_file_dialog_set_initial_name(dlg, suggested_name.c_str());

        struct SaveCtx { std::function<void(std::string)> cb; };
        auto* ctx = new SaveCtx{std::move(cb)};
        gtk_file_dialog_save(dlg, GTK_WINDOW(window_), nullptr,
            +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
            {
                auto* c = static_cast<SaveCtx*>(data);
                GError* err = nullptr;
                GFile* file = gtk_file_dialog_save_finish(
                    GTK_FILE_DIALOG(dialog_obj), res, &err);
                if (file)
                {
                    char* path = g_file_get_path(file);
                    if (path) { c->cb(std::string(path)); g_free(path); }
                    g_object_unref(file);
                }
                if (err) g_error_free(err);
                delete c;
            },
            ctx);
        g_object_unref(dlg);
    };

    settings_controller_->show_open_file_dialog =
        [this](std::function<void(std::string)> cb)
    {
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Open room keys");

        struct OpenCtx { std::function<void(std::string)> cb; };
        auto* ctx = new OpenCtx{std::move(cb)};
        gtk_file_dialog_open(dlg, GTK_WINDOW(window_), nullptr,
            +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
            {
                auto* c = static_cast<OpenCtx*>(data);
                GError* err = nullptr;
                GFile* file = gtk_file_dialog_open_finish(
                    GTK_FILE_DIALOG(dialog_obj), res, &err);
                if (file)
                {
                    char* path = g_file_get_path(file);
                    if (path) { c->cb(std::string(path)); g_free(path); }
                    g_object_unref(file);
                }
                if (err) g_error_free(err);
                delete c;
            },
            ctx);
        g_object_unref(dlg);
    };

    settings_controller_->on_export_keys_result =
        [this](bool ok, std::string error)
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "%s", ok ? "Room keys exported successfully." : error.c_str());
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_signal_connect(dlg, "response",
                         G_CALLBACK(+[](GtkDialog* d, int, gpointer)
                         { gtk_window_destroy(GTK_WINDOW(d)); }),
                         nullptr);
        gtk_window_present(GTK_WINDOW(dlg));
    };

    settings_controller_->on_import_keys_result =
        [this](bool ok, std::string error)
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "%s", ok ? "Room keys imported successfully." : error.c_str());
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_signal_connect(dlg, "response",
                         G_CALLBACK(+[](GtkDialog* d, int, gpointer)
                         { gtk_window_destroy(GTK_WINDOW(d)); }),
                         nullptr);
        gtk_window_present(GTK_WINDOW(dlg));
    };
}

void MainWindow::on_send_clicked()
{
    if (room_view_)
    {
        room_view_->compose_bar()->trigger_send();
    }
}

void MainWindow::on_room_selected(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }

    // Drill into a space if the clicked row is one.
    if (const auto* r = room_by_id_(room_id); r && r->is_space)
    {
        space_nav_frames_.push_back(SpaceNavFrame::capture(room_list_view_));
        space_stack_.push_back(room_id);
        refresh_room_list();
        SpaceNavFrame::enter(room_list_view_);
        return;
    }

    // Route through the controllers so their visible_ state stays in sync.
    if (slash_controller_)
        slash_controller_->hide();
    if (shortcode_controller_)
        shortcode_controller_->hide();
    if (mention_controller_)
        mention_controller_->hide();
    handle_compose_room_leaving_(current_room_id_);
    // (No unsubscribe-on-leave here: ShellBase::prune_warm_subscriptions_ owns
    // timeline lifecycle via the warm-subscription LRU.)
    current_room_id_ = room_id;
    clear_focused_state_(room_id);
    if (mark_read_timer_id_)
    {
        g_source_remove(mark_read_timer_id_);
        mark_read_timer_id_ = 0;
    }
    mark_read_timer_id_ = g_timeout_add(
        static_cast<guint>(
            tesseract::Settings::instance().mark_as_read_delay_ms),
        [](gpointer user_data) -> gboolean
        {
            auto* self = static_cast<MainWindow*>(user_data);
            self->mark_read_timer_id_ = 0;
            self->mark_room_read_(self->current_room_id_);
            return G_SOURCE_REMOVE;
        },
        this);
    update_typing_bar_({}, false);
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
    }
    if (room_text_area_)
    {
        room_text_area_->set_text("");
    }
    // Focus is handled by RoomView::set_room()'s own default-focus policy
    // below — no need to request it here too.
    if (room_view_)
    {
        room_view_->clear_compose_text();
    }

    if (const auto* r = room_by_id_(current_room_id_))
    {
        room_view_->set_room(*r);
    }
    apply_room_compose_draft_(current_room_id_);

    // Subscribe (mut pool) + initial history (shared pool). The split keeps the
    // network paginate off the single mut thread so the next switch's reset is
    // never blocked. See ShellBase::start_room_subscription_.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    start_room_subscription_(current_room_id_, std::move(visible_ids));
}

void MainWindow::push_paginate_result(std::string room_id, bool reached_start)
{
    bool is_current = (room_id == current_room_id_);
    push_paginate_result_(std::move(room_id), reached_start);
    if (is_current && room_view_)
    {
        room_view_->message_list()->reset_near_top_latch();
    }
}

void MainWindow::request_more_history(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    state.in_flight = true;
    if (room_view_)
        room_view_->set_paginating(true);
    start_anim_tick_();

    // Worker thread: invoke the blocking SDK call, marshal the result
    // back via g_idle_add on the main loop.
    run_async_(
        [this, room_id]
        {
            auto pr =
                client_->paginate_back_with_status(room_id, kPaginationBatch);
            // guarded() is called here, on the worker thread, at the exact
            // point the old code read its own alive_ member — same risk
            // profile as before, just consolidated onto the shared guard.
            gtk_post_idle(guarded(
                [this, room_id, reached = pr.ok && pr.reached_start]() mutable
                {
                    push_paginate_result(std::move(room_id), reached);
                }));
        });
}

void MainWindow::on_login_clicked(GtkButton*, gpointer user_data)
{
    static_cast<MainWindow*>(user_data)->do_login();
}

// ---------------------------------------------------------------------------

void MainWindow::push_rooms(std::string user_id,
                            std::vector<tesseract::RoomInfo> rooms)
{
    push_rooms_(std::move(user_id), std::move(rooms));
}

void MainWindow::on_rooms_updated_()
{
    refresh_room_list();
    if (!current_room_id_.empty() && room_view_)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == current_room_id_)
            {
                room_view_->set_room(r);
                break;
            }
        }
    }
    else if (!pending_restore_rooms_.empty())
    {
        if (try_restore_tab_session_(pending_restore_rooms_,
                                     pending_restore_rooms_[0]))
            pending_restore_rooms_.clear();
    }

    update_secondary_room_infos_();
}

void MainWindow::on_invites_updated_()
{
    if (room_list_view_)
    {
        room_list_view_->set_invites(&invites_);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_my_knocks_updated_()
{
    if (room_list_view_)
    {
        room_list_view_->set_my_knocks(&my_knocks_);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::on_space_children_cache_ready_ui_()
{
    refresh_room_list();
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refresh_room_list();
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
}


void MainWindow::push_error(std::string description)
{
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}


void MainWindow::clear_messages()
{
    if (room_view_)
    {
        room_view_->clear_room();
        room_view_->set_messages({});
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

namespace
{

// Decode raw image bytes to a premultiplied-ARGB32 cairo_surface_t the
// shared CairoImage wrapper expects. Reuses GdkPixbufLoader so the
// existing matrix-sdk attachments path (PNG/JPEG/WebP/AVIF) decodes
// identically to the legacy GTK rendering.
//
// Inner helper: convert an already-decoded GdkPixbuf into a premultiplied
// ARGB32 cairo surface. Reused by both the static decoder and the
// animated-frame iterator below.
cairo_surface_t* pixbuf_to_premultiplied_argb32(GdkPixbuf* pb)
{
    if (!pb)
    {
        return nullptr;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    int in_stride = gdk_pixbuf_get_rowstride(pb);
    const guchar* pixels = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char* dst = cairo_image_surface_get_data(surface);
    int out_stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < h; ++y)
    {
        const guchar* src_row = pixels + y * in_stride;
        unsigned char* dst_row = dst + y * out_stride;
        for (int x = 0; x < w; ++x)
        {
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

cairo_surface_t*
decode_image_to_cairo_surface(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return nullptr;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return nullptr;
    }
    if (!gdk_pixbuf_loader_close(loader, &err))
    {
        if (err)
        {
            g_error_free(err);
        }
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
struct DecodedAnimation
{
    std::vector<cairo_surface_t*> frames; // caller owns each
    std::vector<int> delays_ms;
};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
std::optional<DecodedAnimation>
decode_animation(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return std::nullopt;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return std::nullopt;
    }
    if (!gdk_pixbuf_loader_close(loader, &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return std::nullopt;
    }
    GdkPixbufAnimation* anim = gdk_pixbuf_loader_get_animation(loader);
    if (!anim || gdk_pixbuf_animation_is_static_image(anim))
    {
        g_object_unref(loader);
        return std::nullopt;
    }

    GTimeVal t = {0, 0};
    GdkPixbufAnimationIter* iter = gdk_pixbuf_animation_get_iter(anim, &t);
    if (!iter)
    {
        g_object_unref(loader);
        return std::nullopt;
    }

    DecodedAnimation out;
    constexpr int kMaxFrames = 200;
    for (int i = 0; i < kMaxFrames; ++i)
    {
        GdkPixbuf* pb = gdk_pixbuf_animation_iter_get_pixbuf(iter);
        if (!pb)
        {
            break;
        }
        cairo_surface_t* surf = pixbuf_to_premultiplied_argb32(pb);
        if (!surf)
        {
            break;
        }
        int delay = gdk_pixbuf_animation_iter_get_delay_time(iter);
        // -1 means there's no upcoming frame (last frame of a
        // non-looping animation). Capture this final frame and stop.
        if (delay < 0)
        {
            out.frames.push_back(surf);
            out.delays_ms.push_back(100); // arbitrary tail-hold
            break;
        }
        if (delay < 20)
        {
            delay = 20;
        }
        out.frames.push_back(surf);
        out.delays_ms.push_back(delay);

        // Advance the synthesised clock by the just-captured delay.
        t.tv_usec += delay * 1000;
        while (t.tv_usec >= G_USEC_PER_SEC)
        {
            t.tv_sec += 1;
            t.tv_usec -= G_USEC_PER_SEC;
        }
        if (!gdk_pixbuf_animation_iter_advance(iter, &t))
        {
            // Iterator decided no new frame would be shown — we'd
            // duplicate the same pixbuf on the next iteration. Stop.
            break;
        }
    }
    g_object_unref(iter);
    g_object_unref(loader);
    if (out.frames.empty())
    {
        return std::nullopt;
    }
    return out;
}
G_GNUC_END_IGNORE_DEPRECATIONS

} // namespace

void MainWindow::start_anim_tick_if_needed_()
{
    if (tk_anim_tick_id_ != 0)
    {
        return;
    }
    // Also start for an active back-pagination spinner even when nothing
    // animated has been decoded yet — otherwise a fresh paginate in a room
    // with no cached animated images never gets a timer at all, and the
    // spinner (whose phase is computed from elapsed time at paint time)
    // never advances until something unrelated forces a repaint.
    const bool spinner_active = room_view_ && room_view_->message_list() &&
                                room_view_->message_list()->paginating();
    if (account_manager_.anim_cache().empty() && !spinner_active)
    {
        return;
    }
    tk_anim_tick_id_ = g_timeout_add(16, on_tk_anim_tick_, this);
}

void MainWindow::invalidate_anim_consumers_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    if (room_view_)
    {
        if (room_view_->emoji_picker())
            room_view_->emoji_picker()->invalidate_image_cache();
        if (room_view_->sticker_picker())
            room_view_->sticker_picker()->invalidate_image_cache();
    }
}

bool MainWindow::is_main_window_visible_() const
{
    if (!window_ || !gtk_widget_is_visible(GTK_WIDGET(window_))) return false;
    auto* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface) return true;  // surface not yet realized — assume visible
    return !(gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) &
             GDK_TOPLEVEL_STATE_MINIMIZED);
}

gboolean MainWindow::on_tk_anim_tick_(gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    // tick_anim_ returns false (and has called stop_anim_tick_, clearing the
    // source id) when nothing animated remains on-screen.
    return self->tick_anim_() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void MainWindow::stop_anim_tick_()
{
    // Clear the id; the G_SOURCE_REMOVE returned by on_tk_anim_tick_ removes
    // the GSource itself (calling g_source_remove from inside its own dispatch
    // would double-remove).
    tk_anim_tick_id_ = 0;
}

gboolean MainWindow::on_tk_inflight_tick_(gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    return self->inflight_tick_() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void MainWindow::start_inflight_tick_()
{
    if (!tk_inflight_tick_id_)
        tk_inflight_tick_id_ = g_timeout_add(16, on_tk_inflight_tick_, this);
}

void MainWindow::stop_inflight_tick_()
{
    tk_inflight_tick_id_ = 0;
}

void MainWindow::repaint_inflight_spinner_()
{
    if (inflight_dot_)
        gtk_widget_queue_draw(inflight_dot_);
}

void MainWindow::repaint_anim_frame_()
{
    // GTK4 has no partial-widget invalidation (gtk_widget_queue_draw_area was
    // removed; the render-node model only supports whole-widget queue_draw)
    // — but that invalidation is scoped to a single widget, and each
    // currently-animating image now gets its own small overlay GtkDrawingArea
    // (see host_gtk.cpp's live_overlays_/sync_anim_overlays_). update_anim_regions()
    // queues a redraw of just those overlays, not the whole surface.
    if (main_app_surface_)
    {
        main_app_surface_->update_anim_regions();
    }
    if (room_view_)
    {
        if (room_view_->emoji_picker_visible() && room_view_->emoji_picker())
            room_view_->emoji_picker()->invalidate_image_cache();
        if (room_view_->sticker_picker_visible() && room_view_->sticker_picker())
            room_view_->sticker_picker()->invalidate_image_cache();
    }
    if (gif_popup_ && gif_popup_visible_())
        gif_popup_->update_anim_regions();
}

// ---------------------------------------------------------------------------

void MainWindow::show_rooms(const std::vector<tesseract::RoomInfo>& rooms)
{
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms)
    {
        if (!r.is_space)
        {
            sorted.push_back(r);
        }
    }
    for (const auto& r : rooms)
    {
        if (r.is_space)
        {
            sorted.push_back(r);
        }
    }

    // Avatars are fetched lazily as rows are painted (RoomListView's
    // on_room_avatar_needed), so collapsed / off-screen rooms aren't requested.
    room_list_view_->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
    {
        room_list_view_->set_selected_room(current_room_id_);
    }
    main_app_surface_->relayout();
}

void MainWindow::refresh_room_list()
{
    ShellBase::refresh_room_list_();
}

// ---------------------------------------------------------------------------
//  GTK4-specific ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::request_relayout_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::request_repaint_()
{
    if (main_app_surface_)
    {
        main_app_surface_->host().request_repaint();
    }
}

void MainWindow::post_to_ui_(std::function<void()> fn)
{
    // guarded() is called here, at the exact point the old code read its own
    // alive_ member (this may run on whatever thread the caller is on) —
    // same risk profile as before, just consolidated onto the shared guard.
    gtk_post_idle(guarded(std::move(fn)));
}

void MainWindow::post_to_ui_after_(int ms, std::function<void()> fn)
{
    // guarded() is called here, at the exact point the old code read its own
    // alive_ member (this may run on whatever thread the caller is on) —
    // same risk profile as before, just consolidated onto the shared guard.
    gtk_post_timeout(static_cast<guint>(ms), guarded(std::move(fn)));
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                       MediaKind kind,
                                       std::vector<uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    const bool is_avatar =
        (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar);
    // Avatars and inline thumbnails share thumbnail_cache_; full-size media
    // and tiles use image_cache_. Inline media (full or thumbnail) may animate.
    const bool uses_thumb_cache =
        is_avatar || kind == MediaKind::MediaThumbnail;
    const bool try_anim = (kind == MediaKind::MediaImage ||
                           kind == MediaKind::MediaThumbnail ||
                           kind == MediaKind::Sticker ||
                           kind == MediaKind::Reaction);

    // Already decoded? Cheap early-out on the UI thread.
    if (account_manager_.anim_cache().has(cache_key) ||
        (uses_thumb_cache ? account_manager_.thumbnail_cache().contains(cache_key)
                          : account_manager_.image_cache().contains(cache_key)))
    {
        return;
    }

    // Decode OFF the UI thread. gdk-pixbuf now routes image loading through
    // glycin, which decodes in a sandboxed subprocess and blocks the calling
    // thread (block_on). Decoding many room avatars synchronously on the UI
    // thread froze the window for seconds at startup. decode_animation /
    // decode_image_to_cairo_surface are thread-safe; we hand the resulting
    // cairo surfaces (raw pointers) back to the UI thread to wrap + store.
    run_async_(
        [this, cache_key, kind, is_avatar, uses_thumb_cache, try_anim,
         bytes = std::move(bytes)]()
        {
            if (try_anim)
            {
                if (auto anim = decode_animation(bytes))
                {
                    post_to_ui_(
                        [this, cache_key, kind, uses_thumb_cache,
                         frames_raw = std::move(anim->frames),
                         delays = std::move(anim->delays_ms)]() mutable
                        {
                            if (account_manager_.anim_cache().has(cache_key) ||
                                (uses_thumb_cache
                                     ? account_manager_.thumbnail_cache().contains(cache_key)
                                     : account_manager_.image_cache().contains(cache_key)))
                            {
                                for (cairo_surface_t* s : frames_raw)
                                    cairo_surface_destroy(s);
                                return;
                            }
                            std::vector<std::unique_ptr<tk::Image>> frames;
                            frames.reserve(frames_raw.size());
                            for (cairo_surface_t* s : frames_raw)
                            {
                                frames.push_back(tk::cairo_pango::make_image(s));
                                cairo_surface_destroy(s);
                            }
                            if (frames.empty())
                            {
                                return;
                            }
                            const gint64 now_ms = g_get_monotonic_time() / 1000;
                            account_manager_.anim_cache().store(cache_key, std::move(frames),
                                              std::move(delays), now_ms);
                            start_anim_tick_if_needed_();
                            if (room_view_)
                            {
                                room_view_->notify_image_ready(cache_key);
                            }
                            // Coalesced: a burst of media completions folds into
                            // one arrange per drain instead of one full arrange
                            // each — keeps the queue short for a pending echo.
                            schedule_relayout_();
                            if (settings_widget_ &&
                                gtk_widget_get_visible(settings_widget_->widget()))
                            {
                                settings_widget_->request_repaint();
                            }
                            notify_secondary_media_ready_(cache_key, kind);
                        });
                    return;
                }
            }

            cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
            if (!surface)
            {
                return;
            }
            post_to_ui_(
                [this, cache_key, kind, is_avatar, uses_thumb_cache, surface]()
                {
                    const bool present =
                        account_manager_.anim_cache().has(cache_key) ||
                        (uses_thumb_cache
                             ? account_manager_.thumbnail_cache().contains(cache_key)
                             : account_manager_.image_cache().contains(cache_key));
                    if (present)
                    {
                        cairo_surface_destroy(surface);
                        return;
                    }
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    if (uses_thumb_cache)
                    {
                        account_manager_.thumbnail_cache().store(cache_key, std::move(img));
                        // MediaThumbnail re-measures the row (its height
                        // depends on decoded dimensions); avatars are
                        // fixed-size and don't affect row height, but still
                        // need this to clear a room-switch-gate pending entry
                        // waiting on this avatar (see RoomSwitchGateKeeper).
                        if ((kind == MediaKind::MediaThumbnail || is_avatar) &&
                            room_view_)
                        {
                            room_view_->notify_image_ready(cache_key);
                        }
                    }
                    else
                    {
                        account_manager_.image_cache().store(cache_key, std::move(img));
                        // Real row images re-measure only the rows that show
                        // them (targeted, vs a blanket invalidate_data()). A map
                        // Tile fills a fixed-size card and isn't a tracked row
                        // source, so it needs no re-measure — the coalesced
                        // relayout below repaints it (the old path did a full
                        // invalidate_data() re-measure just to repaint it).
                        if ((kind == MediaKind::MediaImage ||
                             kind == MediaKind::Sticker ||
                             kind == MediaKind::Reaction) && room_view_)
                        {
                            room_view_->notify_image_ready(cache_key);
                        }
                    }
                    // Coalesced relayout (see anim path above).
                    schedule_relayout_();
                    if (kind == MediaKind::MediaImage &&
                        shortcode_popup_visible_() && shortcode_popup_)
                    {
                        shortcode_popup_->request_relayout();
                    }
                    if (is_avatar && account_picker_surface_ &&
                        account_picker_popover_ &&
                        gtk_widget_get_visible(account_picker_popover_))
                    {
                        account_picker_surface_->relayout();
                    }
                    if (settings_widget_ &&
                        gtk_widget_get_visible(settings_widget_->widget()))
                    {
                        settings_widget_->request_repaint();
                    }
                    notify_secondary_media_ready_(cache_key, kind);
                });
        });
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select image");
    GtkFileFilter* filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, "Images");
    gtk_file_filter_add_mime_type(filt, "image/png");
    gtk_file_filter_add_mime_type(filt, "image/jpeg");
    gtk_file_filter_add_mime_type(filt, "image/gif");
    gtk_file_filter_add_mime_type(filt, "image/webp");
    GListStore* flist = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(flist, filt);
    g_object_unref(filt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(flist));
    g_object_unref(flist);

    struct Ctx {
        std::function<void(std::vector<uint8_t>, std::string)> cb;
        MainWindow* self;
    };
    auto* ctx = new Ctx{std::move(cb), this};

    gtk_file_dialog_open(dlg, GTK_WINDOW(window_), nullptr,
        +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
        {
            auto* c = static_cast<Ctx*>(data);
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_open_finish(
                GTK_FILE_DIALOG(dialog_obj), res, &err);
            if (file)
            {
                gsize len = 0;
                char* raw = nullptr;
                GError* load_err = nullptr;
                g_file_load_contents(file, nullptr, &raw, &len, nullptr, &load_err);
                if (!load_err && raw && len > 0)
                {
                    std::vector<uint8_t> bytes(raw, raw + len);
                    g_free(raw);
                    char* path = g_file_get_path(file);
                    std::string mime = "image/jpeg";
                    if (path)
                    {
                        std::string p(path);
                        if (p.ends_with(".png"))       mime = "image/png";
                        else if (p.ends_with(".gif"))  mime = "image/gif";
                        else if (p.ends_with(".webp")) mime = "image/webp";
                        g_free(path);
                    }
                    auto callback = std::move(c->cb);
                    c->self->post_to_ui_(
                        [callback = std::move(callback),
                         bytes = std::move(bytes), mime]() mutable
                        { callback(std::move(bytes), mime); });
                }
                if (load_err) g_error_free(load_err);
                g_object_unref(file);
            }
            if (err) g_error_free(err);
            delete c;
        },
        ctx);
    g_object_unref(dlg);
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int /*max_w*/,
                          int /*max_h*/)
{
    // decode_image_to_cairo_surface / decode_animation are in this
    // file's anonymous namespace and are thread-safe (GdkPixbuf + cairo).
    // tk::cairo_pango::make_image refcounts the surface (thread-safe).
    DecodedImage d;
    if (auto anim = decode_animation(bytes))
    {
        d.frames.reserve(anim->frames.size());
        for (cairo_surface_t* s : anim->frames)
        {
            d.frames.push_back(tk::cairo_pango::make_image(s));
            cairo_surface_destroy(s);
        }
        d.delays_ms = std::move(anim->delays_ms);
        if (!d.frames.empty())
        {
            return d;
        }
        d.delays_ms.clear();
    }
    if (cairo_surface_t* surf = decode_image_to_cairo_surface(bytes))
    {
        d.still = tk::cairo_pango::make_image(surf);
        cairo_surface_destroy(surf);
    }
    return d;
}

std::int64_t MainWindow::monotonic_ms_()
{
    return g_get_monotonic_time() / 1000;
}

void MainWindow::start_anim_tick_()
{
    start_anim_tick_if_needed_();
}

void MainWindow::repaint_pickers_()
{
    if (room_view_)
    {
        if (room_view_->emoji_picker())
            room_view_->emoji_picker()->invalidate_image_cache();
        if (room_view_->sticker_picker())
            room_view_->sticker_picker()->invalidate_image_cache();
    }
    invalidate_anim_consumers_();
}

void MainWindow::extract_drop_media_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     std::string mime,
                                     tesseract::views::ComposeBar* target,
                                     std::shared_ptr<bool> target_alive)
{
    run_async_(
        [this, pending_gen, target, target_alive = std::move(target_alive),
         bytes = std::move(bytes), mime = std::move(mime)]() mutable
        {
            tesseract::views::MediaInfo info;
            info.pending_gen = pending_gen;

            // ── Animated image detection ────────────────────────────────────
            if (mime == "image/gif" || mime == "image/webp")
            {
                GInputStream* stream = g_memory_input_stream_new_from_data(
                    bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
                GError* gerr = nullptr;
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                GdkPixbufAnimation* anim =
                    gdk_pixbuf_animation_new_from_stream(stream, nullptr, &gerr);
                g_object_unref(stream);
                if (anim)
                {
                    info.is_animated = !gdk_pixbuf_animation_is_static_image(anim);
                    g_object_unref(anim);
                }
                G_GNUC_END_IGNORE_DEPRECATIONS
                if (gerr)
                    g_error_free(gerr);
            }
            // ── Video: thumbnail + duration via GStreamer ────────────────────
            else if (mime.starts_with("video/"))
            {
                GstElement* pipe = gst_pipeline_new(nullptr);
                GstElement* gsrc =
                    gst_element_factory_make("giostreamsrc", nullptr);
                GstElement* dec = gst_element_factory_make("decodebin", nullptr);
                GstElement* vconv =
                    gst_element_factory_make("videoconvert", nullptr);
                GstElement* vsink =
                    gst_element_factory_make("appsink", nullptr);
                if (!pipe || !gsrc || !dec || !vconv || !vsink)
                {
                    if (pipe) gst_object_unref(pipe);
                    if (gsrc) gst_object_unref(gsrc);
                    if (dec)  gst_object_unref(dec);
                    if (vconv) gst_object_unref(vconv);
                    if (vsink) gst_object_unref(vsink);
                }
                else
                {
                    GstCaps* caps =
                        gst_caps_from_string("video/x-raw,format=BGRA");
                    gst_app_sink_set_caps(GST_APP_SINK(vsink), caps);
                    gst_caps_unref(caps);
                    gst_app_sink_set_drop(GST_APP_SINK(vsink), FALSE);
                    gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 1);

                    GInputStream* mem_stream =
                        g_memory_input_stream_new_from_data(
                            bytes.data(),
                            static_cast<gssize>(bytes.size()), nullptr);
                    g_object_set(gsrc, "stream", mem_stream, nullptr);
                    g_object_unref(mem_stream);

                    gst_bin_add_many(GST_BIN(pipe), gsrc, dec, vconv, vsink,
                                     nullptr);
                    gst_element_link(gsrc, dec);
                    gst_element_link(vconv, vsink);
                    struct PadCtx { GstElement* vconv; };
                    auto* pad_ctx = new PadCtx{vconv};
                    g_signal_connect(
                        dec, "pad-added",
                        G_CALLBACK(
                            +[](GstElement*, GstPad* pad, gpointer ud)
                            {
                                auto* pc = static_cast<PadCtx*>(ud);
                                GstCaps* c2 = gst_pad_get_current_caps(pad);
                                if (!c2) c2 = gst_pad_query_caps(pad, nullptr);
                                GstStructure* st =
                                    gst_caps_get_structure(c2, 0);
                                if (g_str_has_prefix(
                                        gst_structure_get_name(st), "video"))
                                {
                                    GstPad* sp = gst_element_get_static_pad(
                                        pc->vconv, "sink");
                                    if (sp && !gst_pad_is_linked(sp))
                                        gst_pad_link(pad, sp);
                                    if (sp) gst_object_unref(sp);
                                }
                                gst_caps_unref(c2);
                            }),
                        pad_ctx);

                    gst_element_set_state(pipe, GST_STATE_PAUSED);
                    gst_element_get_state(pipe, nullptr, nullptr,
                                          5 * GST_SECOND);
                    GstSample* sample = gst_app_sink_try_pull_preroll(
                        GST_APP_SINK(vsink), 0);

                    // Duration query after pipeline reaches PAUSED.
                    gint64 dur_ns = 0;
                    if (gst_element_query_duration(pipe, GST_FORMAT_TIME,
                                                   &dur_ns) &&
                        dur_ns > 0)
                    {
                        info.duration_ms =
                            static_cast<std::uint64_t>(dur_ns / 1000000);
                    }
                    gst_element_set_state(pipe, GST_STATE_NULL);
                    delete pad_ctx;
                    gst_object_unref(pipe);

                    if (sample)
                    {
                        GstBuffer* buf = gst_sample_get_buffer(sample);
                        GstCaps* scaps = gst_sample_get_caps(sample);
                        int w = 0, h = 0;
                        if (scaps)
                        {
                            GstStructure* st =
                                gst_caps_get_structure(scaps, 0);
                            gst_structure_get_int(st, "width", &w);
                            gst_structure_get_int(st, "height", &h);
                        }
                        if (buf && w > 0 && h > 0)
                        {
                            GstMapInfo map;
                            if (gst_buffer_map(buf, &map, GST_MAP_READ))
                            {
                                // BGRA pixels → PNG via GdkPixbuf → JPEG
                                GdkPixbuf* pb = gdk_pixbuf_new_from_data(
                                    map.data, GDK_COLORSPACE_RGB, TRUE, 8,
                                    w, h, w * 4, nullptr, nullptr);
                                if (pb)
                                {
                                    GError* gerr = nullptr;
                                    gchar* data = nullptr;
                                    gsize  sz = 0;
                                    if (gdk_pixbuf_save_to_buffer(
                                            pb, &data, &sz, "jpeg", &gerr,
                                            "quality", "85", nullptr))
                                    {
                                        info.thumb_bytes.assign(
                                            reinterpret_cast<const std::uint8_t*>(data),
                                            reinterpret_cast<const std::uint8_t*>(data) + sz);
                                        g_free(data);
                                    }
                                    if (gerr) g_error_free(gerr);
                                    info.video_w = static_cast<std::uint32_t>(w);
                                    info.video_h = static_cast<std::uint32_t>(h);
                                    info.thumb_w  = info.video_w;
                                    info.thumb_h  = info.video_h;
                                    g_object_unref(pb);
                                }
                                gst_buffer_unmap(buf, &map);
                            }
                        }
                        gst_sample_unref(sample);
                    }
                }
            }
            // ── Audio: duration via GStreamer ───────────────────────────────
            else if (mime.starts_with("audio/"))
            {
                GstElement* pipe = gst_pipeline_new(nullptr);
                GstElement* gsrc =
                    gst_element_factory_make("giostreamsrc", nullptr);
                GstElement* dec = gst_element_factory_make("decodebin", nullptr);
                GstElement* fsink =
                    gst_element_factory_make("fakesink", nullptr);
                if (!pipe || !gsrc || !dec || !fsink)
                {
                    if (pipe)  gst_object_unref(pipe);
                    if (gsrc)  gst_object_unref(gsrc);
                    if (dec)   gst_object_unref(dec);
                    if (fsink) gst_object_unref(fsink);
                }
                else
                {
                    GInputStream* mem_stream =
                        g_memory_input_stream_new_from_data(
                            bytes.data(),
                            static_cast<gssize>(bytes.size()), nullptr);
                    g_object_set(gsrc, "stream", mem_stream, nullptr);
                    g_object_unref(mem_stream);

                    gst_bin_add_many(GST_BIN(pipe), gsrc, dec, fsink, nullptr);
                    gst_element_link(gsrc, dec);
                    struct PadCtx { GstElement* fsink; };
                    auto* pad_ctx = new PadCtx{fsink};
                    g_signal_connect(
                        dec, "pad-added",
                        G_CALLBACK(
                            +[](GstElement*, GstPad* pad, gpointer ud)
                            {
                                auto* pc = static_cast<PadCtx*>(ud);
                                GstCaps* c2 = gst_pad_get_current_caps(pad);
                                if (!c2) c2 = gst_pad_query_caps(pad, nullptr);
                                GstStructure* st =
                                    gst_caps_get_structure(c2, 0);
                                if (!g_str_has_prefix(
                                        gst_structure_get_name(st), "video"))
                                {
                                    GstPad* sp = gst_element_get_static_pad(
                                        pc->fsink, "sink");
                                    if (sp && !gst_pad_is_linked(sp))
                                        gst_pad_link(pad, sp);
                                    if (sp) gst_object_unref(sp);
                                }
                                gst_caps_unref(c2);
                            }),
                        pad_ctx);

                    gst_element_set_state(pipe, GST_STATE_PAUSED);
                    gst_element_get_state(pipe, nullptr, nullptr,
                                          5 * GST_SECOND);
                    gint64 dur_ns = 0;
                    if (gst_element_query_duration(pipe, GST_FORMAT_TIME,
                                                   &dur_ns) &&
                        dur_ns > 0)
                    {
                        info.duration_ms =
                            static_cast<std::uint64_t>(dur_ns / 1000000);
                    }
                    gst_element_set_state(pipe, GST_STATE_NULL);
                    delete pad_ctx;
                    gst_object_unref(pipe);
                }
            }

            // Post result to UI thread — resolve compose_bar() at call time
            // to avoid any raw-pointer lifetime hazard with the captured cb.
            // guarded() is called here, on the worker thread, at the exact
            // point the old code read its own alive_ member. target_alive
            // stays a genuinely-independent shared_ptr<bool> (not this
            // shell's own guard) — the pop-out it guards is a different
            // object with its own lifetime; see RoomPane's
            // media_extract_alive_ for the matching rationale.
            std::function<void()> fn;
            if (target)
            {
                fn = [target, target_alive, info = std::move(info)]() mutable
                {
                    if (target_alive && *target_alive)
                        target->update_pending_attachment(info);
                };
            }
            else
            {
                fn = guarded([this, info = std::move(info)]() mutable
                {
                    if (room_view_)
                        room_view_->compose_bar()->update_pending_attachment(info);
                });
            }
            // fn is std::function<void()> here (not a bare guarded() closure)
            // because the two branches above build genuinely different
            // concrete closure types that must unify to one variable.
            gtk_post_idle(std::move(fn));
        });
}

void MainWindow::extract_video_first_frame_jpeg_(
    const std::string& /*event_id*/, const std::string& source_token,
    std::function<void(std::vector<std::uint8_t>)> cb)
{
    if (!client_)
    {
        cb({});
        return;
    }
    const std::string src = source_token;
    // decode: runs the GStreamer decode + PNG-encode pipeline against
    // `bytes` off-thread and invokes `done(png)` on the UI thread (empty
    // png on any failure). Shared (via shared_ptr) so both the prefix
    // attempt and the full-file fallback below can reuse it.
    auto decode = std::make_shared<
        std::function<void(std::vector<uint8_t>,
                           std::function<void(std::vector<uint8_t>)>)>>();
    *decode =
        [this](std::vector<uint8_t> bytes,
               std::function<void(std::vector<uint8_t>)> done) mutable
        {
            run_async_(
                [this, done = std::move(done), bytes = std::move(bytes)]() mutable
                {
            // Extract first frame via GStreamer appsink.
            GstElement* pipe = gst_pipeline_new(nullptr);
            GstElement* gsrc =
                gst_element_factory_make("giostreamsrc", nullptr);
            GstElement* dec = gst_element_factory_make("decodebin", nullptr);
            GstElement* vconv =
                gst_element_factory_make("videoconvert", nullptr);
            GstElement* vsink = gst_element_factory_make("appsink", nullptr);
            if (!pipe || !gsrc || !dec || !vconv || !vsink)
            {
                if (pipe)
                {
                    gst_object_unref(pipe);
                }
                if (gsrc)
                {
                    gst_object_unref(gsrc);
                }
                if (dec)
                {
                    gst_object_unref(dec);
                }
                if (vconv)
                {
                    gst_object_unref(vconv);
                }
                if (vsink)
                {
                    gst_object_unref(vsink);
                }
                done({});
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
            struct PadCtx
            {
                GstElement* vconv;
            };
            auto* pad_ctx = new PadCtx{vconv};
            g_signal_connect(
                dec, "pad-added",
                G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer ud)
                           {
                               auto* pc = static_cast<PadCtx*>(ud);
                               GstCaps* c2 = gst_pad_get_current_caps(pad);
                               if (!c2)
                               {
                                   c2 = gst_pad_query_caps(pad, nullptr);
                               }
                               GstStructure* st = gst_caps_get_structure(c2, 0);
                               if (g_str_has_prefix(gst_structure_get_name(st),
                                                    "video"))
                               {
                                   GstPad* sp = gst_element_get_static_pad(
                                       pc->vconv, "sink");
                                   if (sp && !gst_pad_is_linked(sp))
                                   {
                                       gst_pad_link(pad, sp);
                                   }
                                   if (sp)
                                   {
                                       gst_object_unref(sp);
                                   }
                               }
                               gst_caps_unref(c2);
                           }),
                pad_ctx);
            gst_element_set_state(pipe, GST_STATE_PLAYING);
            // Pull exactly one preroll frame.
            GstSample* sample = gst_app_sink_pull_preroll(GST_APP_SINK(vsink));
            gst_element_set_state(pipe, GST_STATE_NULL);
            delete pad_ctx;
            gst_object_unref(pipe);
            if (!sample)
            {
                done({});
                return;
            }
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstCaps* scaps = gst_sample_get_caps(sample);
            int w = 0, h = 0;
            if (scaps)
            {
                GstStructure* st = gst_caps_get_structure(scaps, 0);
                gst_structure_get_int(st, "width", &w);
                gst_structure_get_int(st, "height", &h);
            }
            if (!buf || w <= 0 || h <= 0)
            {
                gst_sample_unref(sample);
                done({});
                return;
            }
            GstMapInfo map;
            if (!gst_buffer_map(buf, &map, GST_MAP_READ))
            {
                gst_sample_unref(sample);
                done({});
                return;
            }
            std::vector<uint8_t> frame_bytes(map.data, map.data + map.size);
            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
            // BGRA → cairo surface → PNG-encoded bytes, on the main thread
            // (cairo_image_surface_create needs no display, but this mirrors
            // the pre-existing hop used before this path fed image_cache_
            // directly). guarded() is called here, on the worker thread, at
            // the exact point the old code read its own alive_ member.
            gtk_post_idle(guarded(
                [done, pixels = std::move(frame_bytes), w, h]() mutable
                {
                    // Create an owned cairo surface and blit the BGRA pixels in.
                    cairo_surface_t* surf = cairo_image_surface_create(
                        CAIRO_FORMAT_ARGB32, w, h);
                    std::vector<uint8_t> png;
                    if (surf &&
                        cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
                    {
                        int dst_stride =
                            cairo_image_surface_get_stride(surf);
                        unsigned char* dst =
                            cairo_image_surface_get_data(surf);
                        int src_stride = w * 4;
                        for (int row = 0; row < h; ++row)
                        {
                            std::memcpy(
                                dst + row * dst_stride,
                                pixels.data() + row * src_stride,
                                static_cast<std::size_t>(src_stride));
                        }
                        cairo_surface_mark_dirty(surf);
                        cairo_surface_write_to_png_stream(
                            surf,
                            [](void* closure, const unsigned char* data,
                               unsigned int length) -> cairo_status_t
                            {
                                auto* out =
                                    static_cast<std::vector<uint8_t>*>(
                                        closure);
                                out->insert(out->end(), data,
                                           data + length);
                                return CAIRO_STATUS_SUCCESS;
                            },
                            &png);
                        cairo_surface_destroy(surf);
                    }
                    else if (surf)
                    {
                        cairo_surface_destroy(surf);
                    }
                    done(std::move(png));
                }));
                }); // run_async_
        }; // *decode
    auto req_id = begin_media_req_(0,
        [this, cb, src, decode](std::vector<uint8_t> prefix_bytes) mutable
        {
            if (prefix_bytes.empty())
            {
                cb({});
                return;
            }
            (*decode)(
                std::move(prefix_bytes),
                [this, cb, src, decode](std::vector<uint8_t> png) mutable
                {
                    if (!png.empty())
                    {
                        cb(std::move(png));
                        return;
                    }
                    // Prefix wasn't enough (e.g. a non-fast-start file with
                    // its moov atom at EOF) — fall back to the full file.
                    auto full_req = begin_media_req_(0,
                        [cb, decode](std::vector<uint8_t> full_bytes) mutable
                        {
                            if (full_bytes.empty())
                            {
                                cb({});
                                return;
                            }
                            (*decode)(std::move(full_bytes), cb);
                        });
                    client_->fetch_source_bytes_async(full_req, src);
                });
        });
    client_->fetch_source_prefix_async(
        req_id, src, tesseract::visual::kVideoThumbnailPrefixBytes);
}

void MainWindow::cache_rgba_image_(const std::string& key, int w, int h,
                                   std::vector<uint8_t> rgba)
{
    if (account_manager_.image_cache().contains(key))
    {
        return;
    }
    cairo_surface_t* surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!surf)
        return;
    cairo_surface_flush(surf);
    uint8_t* dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    const uint8_t* src_px = rgba.data();
    for (int y = 0; y < h; ++y)
    {
        auto* row = reinterpret_cast<uint32_t*>(dst + y * stride);
        const uint8_t* s = src_px + y * w * 4;
        for (int x = 0; x < w; ++x, s += 4)
        {
            uint32_t a = s[3], r = s[0], g = s[1], b = s[2];
            r = (r * a + 127u) / 255u;
            g = (g * a + 127u) / 255u;
            b = (b * a + 127u) / 255u;
            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);
    account_manager_.image_cache().store(key, tk::cairo_pango::make_image(surf));
    cairo_surface_destroy(surf);
    if (main_app_surface_)
    {
        gtk_widget_queue_draw(main_app_surface_->widget());
    }
}

// ---------------------------------------------------------------------------
// EncryptionSetupOverlay wiring (GTK4 shell)
// ---------------------------------------------------------------------------

void MainWindow::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    if (!main_app_)
        return;
    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // Reconfigure the overlay (clears prior callbacks + field text) before
    // wiring the shared callbacks via ShellBase.
    ov->reset(mode);

    wire_encryption_setup_callbacks_(*ov, main_app_surface_->host());

    main_app_->show_encryption_setup(true);
    main_app_surface_->relayout();
}

// ---------------------------------------------------------------------------

void MainWindow::push_notification(const std::string& user_id,
                                   const std::string& room_id,
                                   const std::string& room_name,
                                   const std::string& sender,
                                   const std::string& body, bool is_mention,
                                   std::vector<uint8_t> avatar_bytes,
                                   std::vector<uint8_t> image_bytes,
                                   std::string event_id)
{
    handle_notification(user_id, room_id, room_name, sender, body, is_mention,
                        std::move(avatar_bytes), std::move(image_bytes),
                        std::move(event_id));
}

void MainWindow::handle_notification(const std::string& user_id,
                                     const std::string& room_id,
                                     const std::string& room_name,
                                     const std::string& sender,
                                     const std::string& body, bool is_mention,
                                     std::vector<uint8_t> avatar_bytes,
                                     std::vector<uint8_t> image_bytes,
                                     std::string event_id)
{
    bool win_focused = gtk_window_is_active(GTK_WINDOW(window_));
    auto* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    auto state = gdk_toplevel_get_state(GDK_TOPLEVEL(surface));
    bool win_visible = gtk_widget_get_visible(GTK_WIDGET(window_)) &&
                       !(state & GDK_TOPLEVEL_STATE_MINIMIZED);

    for (auto& sess : account_manager_.accounts())
    {
        if (sess->user_id != user_id)
        {
            continue;
        }
        // Already watching this exact room — suppress silently.
        if (win_focused && active_account_ &&
            active_account_->user_id == user_id &&
            current_room_id_ == room_id)
        {
            return;
        }
        // Window on screen, not focused: GTK4 has no urgency-hint API, so
        // request attention with a GNotification instead (the GTK4-native
        // mechanism; on most shells it also flags the app in the dock /
        // taskbar). One reusable id so a newer message replaces the
        // previous banner; it is withdrawn when the window regains focus
        // (notify::is-active handler in the constructor).
        if (win_visible)
        {
            if (!win_focused && app_)
            {
                GNotification* notif = g_notification_new(sender.c_str());
                g_notification_set_body(notif, body.c_str());
                g_notification_set_priority(
                    notif, is_mention ? G_NOTIFICATION_PRIORITY_HIGH
                                      : G_NOTIFICATION_PRIORITY_NORMAL);
                if (!avatar_bytes.empty())
                {
                    GBytes* gb =
                        g_bytes_new(avatar_bytes.data(), avatar_bytes.size());
                    GIcon* ic = g_bytes_icon_new(gb);
                    g_notification_set_icon(notif, ic);
                    g_object_unref(ic);
                    g_bytes_unref(gb);
                }
                g_application_send_notification(G_APPLICATION(app_),
                                                kAttentionNotifId, notif);
                g_object_unref(notif);
            }
            // Focused (different room) or no app: the sidebar unread badge
            // is signal enough — no popup.
            return;
        }
        // Window minimised / hidden: send system notification.
        if (sess->notifier)
        {
            tesseract::Notification n;
            n.room_id = room_id;
            n.room_name = room_name;
            n.sender = sender;
            n.body = body;
            n.is_mention = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            n.image_bytes = std::move(image_bytes);
            n.event_id = std::move(event_id);
            sess->notifier->notify(n);
        }
        return;
    }
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (room_list_view_)
    {
        room_list_view_->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::refresh_pickers_packs_()
{
    if (!room_view_)
        return;
    room_view_->set_current_room_parent_spaces(
        parent_spaces_for_room_(current_room_id_));
    room_view_->refresh_stickers();
    room_view_->refresh_emoticon_packs();
}

void MainWindow::push_backup_progress(tesseract::BackupProgress progress)
{
    // Key-download progress is surfaced by refresh_sync_status()
    // ("Downloading encryption keys (N)…").
    last_backup_state_ = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refresh_sync_status();
}

void MainWindow::push_room_list_state(tesseract::RoomListState state)
{
    push_room_list_state_(state);
    refresh_sync_status();
}

gboolean MainWindow::on_sync_status_debounce_(gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    self->sync_status_debounce_id_ = 0;
    using RLS = tesseract::RoomListState;
    if (self->status_bar_ && (self->last_room_list_state_ == RLS::Init ||
                              self->last_room_list_state_ == RLS::SettingUp))
    {
        self->sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(self->status_bar_),
                           _("Syncing rooms\xe2\x80\xa6"));
    }
    return G_SOURCE_REMOVE;
}

void MainWindow::refresh_sync_status()
{
    if (!status_bar_)
    {
        return;
    }
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init ||
                            last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy)
    {
        if (!sync_progress_shown_ && sync_status_debounce_id_ == 0)
        {
            sync_status_debounce_id_ =
                g_timeout_add(300, on_sync_status_debounce_, this);
        }
        else if (sync_progress_shown_)
        {
            gtk_label_set_text(GTK_LABEL(status_bar_),
                               _("Syncing rooms\xe2\x80\xa6"));
        }
        return;
    }

    if (sync_status_debounce_id_ != 0)
    {
        g_source_remove(sync_status_debounce_id_);
        sync_status_debounce_id_ = 0;
    }

    if (reconnecting)
    {
        sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(status_bar_),
                           _("Reconnecting\xe2\x80\xa6"));
        return;
    }
    if (keys_busy)
    {
        sync_progress_shown_ = true;
        std::string msg = std::string(_("Downloading encryption keys (")) +
                          std::to_string(last_imported_keys_) + ")\xe2\x80\xa6";
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        return;
    }
    // Steady state: settle to "Connected" unless a persistent status override
    // (e.g., "Fetching older messages…" from in-room search) is still active.
    if (has_status_override_())
        return;
    sync_progress_shown_ = false;
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populate_user_strip()
{
    if (!main_app_)
    {
        return;
    }
    auto* ui = main_app_->user_info();
    std::string shown =
        my_display_name_.empty() ? my_user_id_ : my_display_name_;
    ui->set_display_name(shown);
    ui->set_user_id(my_user_id_);
    ui->set_avatar_url(my_avatar_url_);
    ui->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        { return account_manager_.thumbnail_cache().peek(mxc); });
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    // Kick off avatar fetch if not yet cached (result arrives via
    // on_media_bytes_ready_ → tk_avatars_ → relayout).
    if (!my_avatar_url_.empty() && client_)
    {
        ensure_user_avatar_(my_avatar_url_);
    }
}

void MainWindow::open_settings_()
{
    settings_widget_->populate(
        my_display_name_, my_user_id_, my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        { return account_manager_.thumbnail_cache().peek(mxc); });

    // load_persisted_settings() (inside populate()) seeds the checkbox from
    // Settings::launch_at_login (the bookkeeping cache); re-push the actual
    // queried OS state here so the checkbox self-heals if that cache drifted
    // (e.g. the user removed the autostart entry outside the app).
    settings_widget_->settings_view()->set_launch_at_login_pref(
        autostart_->is_enabled());

    if (settings_controller_)
        settings_widget_->set_controller(settings_controller_.get());

    // Refresh storage sizes each time settings opens.
    compute_cache_sizes_([this](uint64_t local, uint64_t sdk, uint64_t memory,
                                uint64_t mh, uint64_t mm,
                                uint64_t dh, uint64_t dm)
    {
        if (settings_widget_)
            settings_widget_->set_cache_sizes(local, sdk, memory, mh, mm, dh,
                                              dm);
    });

    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "settings");
    start_search_index_stats_poll_();
}

void MainWindow::do_logout()
{
    logout_active_account();
}

// ---------------------------------------------------------------------------
// Shortcode popup — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::ShortcodePopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    // Widget + controller created eagerly in the constructor; this positions
    // the already-populated popup at the caret.
    if (!shortcode_popup_)
    {
        return;
    }
    const float w = tesseract::views::ShortcodePopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::ShortcodePopup::kRowHeight;
    shortcode_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    shortcode_popup_->set_visible(true);
}

void MainWindow::hide_shortcode_popup_()
{
    if (shortcode_popup_)
    {
        shortcode_popup_->set_visible(false);
    }
}

// ── Slash-command popup ────────────────────────────────────────────────────

void MainWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    if (!slash_popup_)
    {
        return;
    }
    const float w = tesseract::views::SlashCommandPopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::SlashCommandPopup::kRowHeight;
    slash_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    slash_popup_->set_visible(true);
}

void MainWindow::hide_slash_popup_()
{
    if (slash_popup_)
    {
        slash_popup_->set_visible(false);
    }
}

void MainWindow::show_gif_popup_()
{
    if (!gif_popup_ || !gif_popup_widget_ || !room_text_area_ ||
        !main_app_surface_)
    {
        return;
    }
    // Full-width strip spanning the compose bar, floating just above it (like
    // the attachment preview band). content_size() drives only the height and
    // the empty/status check; the width comes from the compose bar.
    const tk::Rect cb = room_view_ ? room_view_->compose_bar_rect() : tk::Rect{};
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    gif_popup_->set_rect(cb, {cb.w, sz.h}, tk::PopupPlacement::PreferAbove);
    gif_popup_->set_visible(true);
}

void MainWindow::hide_gif_popup_()
{
    if (gif_popup_)
    {
        gif_popup_->set_visible(false);
    }
}

const tk::Image*
MainWindow::gif_strip_image_(const tesseract::GifResult& result,
                             const std::function<void()>& repaint)
{
    // Shared with every pop-out's GIF strip (RoomWindowBase::shell_gif_strip_image_
    // → here). The pop-out passes a repaint that refreshes its own popup surface.
    return gif_strip_provider_ ? gif_strip_provider_(result, repaint) : nullptr;
}

void MainWindow::handle_gif_results_ui_(std::uint64_t request_id,
                                        std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void MainWindow::handle_gif_search_failed_ui_(std::uint64_t request_id,
                                              std::string message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, std::move(message));
    }
}

// ── @mention popup ─────────────────────────────────────────────────────────

void MainWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_)
    {
        return;
    }
    const float w = tesseract::views::MentionPopup::kWidth;
    const float h = static_cast<float>(rows) * tesseract::views::MentionPopup::kRowHeight;
    mention_popup_->set_rect(cursor_local, {w, h}, tk::PopupPlacement::PreferAbove);
    mention_popup_->set_visible(true);
}

void MainWindow::hide_mention_popup_()
{
    if (mention_popup_)
    {
        mention_popup_->set_visible(false);
    }
}

// ---------------------------------------------------------------------------
// Emoji picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::EmojiPicker. The search row is a native
// GtkEntry overlaid by the Surface; selection routes back through the
// shared widget's on_selected callback.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Sticker context menu — right-click on a sticker row offers
// "Add to Saved Stickers" (suppressed for stickers already saved).
// ---------------------------------------------------------------------------

void MainWindow::build_sticker_context_menu()
{
    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Add to Saved Stickers"), "sticker.save");

    sticker_ctx_menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_ctx_menu_), FALSE);
    gtk_widget_set_parent(sticker_ctx_menu_, main_app_surface_->widget());
    g_object_unref(menu);

    sticker_ctx_actions_ = g_simple_action_group_new();
    GSimpleAction* save = g_simple_action_new("save", nullptr);
    g_signal_connect(save, "activate", G_CALLBACK(on_sticker_save_activate_),
                     this);
    g_action_map_add_action(G_ACTION_MAP(sticker_ctx_actions_), G_ACTION(save));
    g_object_unref(save);
    gtk_widget_insert_action_group(main_app_surface_->widget(), "sticker",
                                   G_ACTION_GROUP(sticker_ctx_actions_));
}

// ---------------------------------------------------------------------------
// Copy context menu — right-click on a text selection offers "Copy".
// ---------------------------------------------------------------------------

void MainWindow::build_copy_context_menu_()
{
    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Copy"), "copy-sel.copy");

    copy_ctx_menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(copy_ctx_menu_), FALSE);
    gtk_widget_set_parent(copy_ctx_menu_, main_app_surface_->widget());
    g_object_unref(menu);

    copy_ctx_actions_ = g_simple_action_group_new();
    GSimpleAction* act = g_simple_action_new("copy", nullptr);
    g_signal_connect(act, "activate", G_CALLBACK(on_copy_action_), this);
    g_action_map_add_action(G_ACTION_MAP(copy_ctx_actions_), G_ACTION(act));
    g_object_unref(act);
    gtk_widget_insert_action_group(main_app_surface_->widget(), "copy-sel",
                                   G_ACTION_GROUP(copy_ctx_actions_));
}

// static
void MainWindow::on_copy_action_(GSimpleAction* /*action*/,
                                  GVariant* /*parameter*/,
                                  gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->room_view_)
        self->room_view_->message_list()->copy_selection();
}

void MainWindow::on_msg_right_click_(GtkGestureClick* gesture, int /*n_press*/,
                                     double x, double y, gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);

    // User strip (lower-left sidebar) → user context menu. The other shells
    // hit-test this region in their native right-click handler and invoke
    // UserInfo::on_secondary; GTK routes right-clicks here, so do the same.
    if (self->main_app_ && self->main_app_->user_info() &&
        self->main_app_->user_info()->on_secondary)
    {
        const int surf_h =
            gtk_widget_get_height(self->main_app_surface_->widget());
        if (x < tesseract::visual::kSidebarWidth &&
            y >= surf_h - tesseract::visual::kUserStripHeight)
        {
            gtk_gesture_set_state(GTK_GESTURE(gesture),
                                  GTK_EVENT_SEQUENCE_CLAIMED);
            self->main_app_->user_info()->on_secondary(
                tk::Point{static_cast<float>(x), static_cast<float>(y)});
            return;
        }
    }

    if (!self->room_view_ || self->room_view_->is_overlay_open())
    {
        return;
    }
    // Lazy-built, like the sibling copy-selection menu (on_show_copy_menu /
    // build_copy_context_menu_ above) — build_sticker_context_menu() was
    // never actually being called anywhere, so this had always fallen
    // through here and no-op'd on every right-click on a sticker.
    if (!self->sticker_ctx_menu_)
    {
        self->build_sticker_context_menu();
    }
    if (!self->sticker_ctx_menu_)
    {
        return;
    }

    auto hit = self->room_view_->message_list()->sticker_hit_at(
        tk::Point{static_cast<float>(x), static_cast<float>(y)});
    if (!hit)
    {
        return;
    }

    // Claim the gesture so the underlying surface doesn't also process it
    // (e.g. as a drag-start or text-selection event).
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Capture sticker fields for the action handler. The hit_at result
    // points into MessageListView's per-frame sticker_geom_ map and would
    // dangle by the time the action fires.
    self->ctx_sticker_event_id_ = hit->event_id;
    self->ctx_sticker_mxc_url_ = hit->source ? hit->source->mxc_url() : std::string{};
    self->ctx_sticker_body_ = hit->body;
    self->ctx_sticker_info_json_ = hit->info_json;

    // Disable the action when the sticker is already saved so the menu item
    // renders grayed-out rather than the menu being suppressed entirely.
    {
        const bool already_saved =
            self->client_->user_pack_has_sticker(self->ctx_sticker_mxc_url_,
                                                 hit->info_json);
        GAction* act = g_action_map_lookup_action(
            G_ACTION_MAP(self->sticker_ctx_actions_), "save");
        if (act)
        {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(act), !already_saved);
        }
    }

    GdkRectangle r{.x = static_cast<int>(x),
                   .y = static_cast<int>(y),
                   .width = 1,
                   .height = 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(self->sticker_ctx_menu_), &r);
    gtk_popover_popup(GTK_POPOVER(self->sticker_ctx_menu_));
}

gboolean MainWindow::on_quick_switch_shortcut_(GtkWidget*, GVariant*,
                                               gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->main_app_)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Character;
        event.text = "k";
        event.ctrl = true;
        self->main_app_->dispatch_key_down(event);
    }
    return TRUE;
}

gboolean MainWindow::on_message_search_shortcut_(GtkWidget*, GVariant*,
                                                 gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->main_app_)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Character;
        event.text = "f";
        event.ctrl = true;
        event.shift = true;
        self->main_app_->dispatch_key_down(event);
    }
    return TRUE;
}

void MainWindow::open_find_in_room_()
{
    if (!main_app_ || !main_app_->room_view())
        return;
    if (current_room_id_.empty())
        return;
    main_app_->room_view()->open_room_search();
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::close_find_in_room_()
{
    if (main_app_ && main_app_->room_view())
        main_app_->room_view()->close_room_search();
    if (main_app_surface_)
        main_app_surface_->relayout();
}

gboolean MainWindow::on_find_in_room_shortcut_(GtkWidget*, GVariant*,
                                               gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->main_app_)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Character;
        event.text = "f";
        event.ctrl = true;
        self->main_app_->dispatch_key_down(event);
    }
    return TRUE;
}

gboolean MainWindow::on_nav_back_shortcut_(GtkWidget*, GVariant*,
                                           gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->main_app_)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Left;
        event.alt = true;
        self->main_app_->dispatch_key_down(event);
    }
    return TRUE;
}

gboolean MainWindow::on_nav_fwd_shortcut_(GtkWidget*, GVariant*,
                                          gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->main_app_)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Right;
        event.alt = true;
        self->main_app_->dispatch_key_down(event);
    }
    return TRUE;
}

void MainWindow::open_quick_switch_()
{
    if (!main_app_ || !main_app_->quick_switcher())
        return;
    main_app_->show_quick_switch(true);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::close_quick_switch_()
{
    if (main_app_)
        main_app_->show_quick_switch(false);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::open_message_search_()
{
    if (!main_app_ || !main_app_->message_search())
        return;
    main_app_->show_message_search(true);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::close_message_search_()
{
    if (main_app_)
        main_app_->show_message_search(false);
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::close_forward_picker_()
{
    if (main_app_ && main_app_->forward_picker())
        main_app_->forward_picker()->close();
}

void MainWindow::focus_forward_picker_field_()
{
    if (!main_app_ || !main_app_->forward_picker())
        return;
    if (auto* f = main_app_->forward_picker()->search_field())
    {
        f->set_text("");
        f->set_focused(true);
    }
}

void MainWindow::hide_forward_picker_field_()
{
    if (main_app_ && main_app_->forward_picker())
        if (auto* f = main_app_->forward_picker()->search_field())
            f->set_visible(false);
}

gboolean MainWindow::on_window_key_pressed_(GtkEventControllerKey*,
                                            guint keyval, guint,
                                            GdkModifierType state,
                                            gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    // Ctrl+K is handled by a global-scope GtkShortcutController (see ctor) so
    // it works while a native entry / text view has focus.
    if (keyval == GDK_KEY_c && (state & GDK_CONTROL_MASK))
    {
        if (self->room_view_ && self->room_view_->message_list()->has_selection())
        {
            self->room_view_->message_list()->copy_selection();
            return TRUE;
        }
    }
    if (keyval == GDK_KEY_Escape)
    {
        const bool had_quick_switch = self->main_app_ &&
            self->main_app_->quick_switcher() &&
            self->main_app_->quick_switcher()->is_open();
        const bool had_message_search = self->main_app_ &&
            self->main_app_->message_search() &&
            self->main_app_->message_search()->is_open();
        const bool had_room_search = self->main_app_ &&
            self->main_app_->room_view() &&
            self->main_app_->room_view()->room_search_open();
        if (self->main_app_ &&
            self->main_app_->dispatch_key_down({tk::Key::Escape}))
        {
            if (had_quick_switch)
                self->close_quick_switch_();
            else if (had_message_search)
                self->close_message_search_();
            else if (had_room_search)
                self->close_find_in_room_();
            else if (self->main_app_surface_)
                self->main_app_surface_->relayout();
            return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::on_sticker_save_activate_(GSimpleAction* /*action*/,
                                           GVariant* /*parameter*/,
                                           gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->ctx_sticker_mxc_url_.empty())
    {
        return;
    }
    auto res = self->client_->save_sticker_to_user_pack(
        self->ctx_sticker_body_, self->ctx_sticker_body_,
        self->ctx_sticker_mxc_url_, self->ctx_sticker_info_json_);
    if (!res.ok)
    {
        self->push_error(res.message);
    }
    self->ctx_sticker_event_id_.clear();
    self->ctx_sticker_mxc_url_.clear();
    self->ctx_sticker_body_.clear();
    self->ctx_sticker_info_json_.clear();
    if (self->sticker_ctx_menu_)
    {
        gtk_popover_popdown(GTK_POPOVER(self->sticker_ctx_menu_));
    }
}


// ---------------------------------------------------------------------------
// Multi-account management
// ---------------------------------------------------------------------------

void MainWindow::switch_active_account(const std::string& user_id)
{
    // Platform-agnostic bookkeeping (unsubscribe previous room, clear
    // per-account state, swap active_account_ / aliases / identity, compute
    // pending restores, swap rooms_/invites_ snapshots, persist the index)
    // lives in ShellBase. Returns false (no-op) when the account isn't found
    // or is already active with a bound client.
    if (!switch_active_account_impl_(user_id))
    {
        return;
    }
    refresh_account_ui_after_switch_();
}

void MainWindow::refresh_account_ui_after_switch_()
{
    clear_messages();

    populate_user_strip();

    if (room_view_)
    {
        room_view_->set_client(client_);
    }

    refresh_room_list();

    if (main_app_)
        main_app_->show_room();

    if (main_app_)
    {
        main_app_->show_verif_banner(false);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    rebuild_account_picker();
    handle_verification_state_ui_(active_account_ && !active_account_->unverified);
}

void MainWindow::begin_add_account()
{
    // Remember the index of the currently active account so we can return to it
    // if the user cancels or adds an account that's already signed in.
    add_account_return_idx_ = -1;
    if (active_account_)
    {
        const auto& accs = account_manager_.accounts();
        for (int i = 0; i < static_cast<int>(accs.size()); ++i)
        {
            if (accs[i].get() == active_account_.get())
            {
                add_account_return_idx_ = i;
                break;
            }
        }
    }
    pending_login_is_add_account_ = true;
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    login_view_->set_client(pending_login_client_.get());
    login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
    login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    login_view_->reset();
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

void MainWindow::logout_active_account()
{
    // Platform-agnostic teardown (unsubscribe the room, up_connector/presence
    // logout, client_->logout() + failure surface, stop_sync, clear account
    // state, tray refresh, index update, and — when other accounts remain — the
    // switch to a survivor) lives in ShellBase.
    const auto result = logout_active_account_impl_();
    if (!result.logged_out)
    {
        return;
    }

    // Native widget cleanup of the now-empty surface (the remaining-account
    // branch already repainted via refresh_account_ui_after_switch_).
    if (!result.has_remaining)
    {
        clear_messages();
        refresh_room_list();
        if (room_view_)
        {
            // Drop RoomView's (and its EmojiPicker/StickerPicker's) cached raw
            // Client* — it's never re-pointed once there's no survivor to
            // switch to, and the old Client is about to be destroyed
            // asynchronously by logout_active_account_impl_'s drain barrier.
            room_view_->set_client(nullptr);
        }
        if (main_app_)
        {
            main_app_->clear_content();
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
    }
    verification_banner_dismissed_ = false;

    // logged_out is already known true here (early-returned above
    // otherwise); a background logout failure still surfaces separately
    // via show_status_message_ once client_->logout() completes on
    // mut_pool_ — see LogoutResult's comment for why there's no synchronous
    // `ok` to gate this on.
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Signed out"));

    if (!result.has_remaining)
    {
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        login_view_->set_client(pending_login_client_.get());
        login_view_->set_on_begin_oauth([this] { arm_pending_login_(); });
        login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
        login_view_->reset();
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    }
}

void MainWindow::on_login_cancelled()
{
    login_view_->set_client(nullptr);
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty())
    {
        std::filesystem::remove_all(pending_login_temp_dir_);
        pending_login_temp_dir_.clear();
    }

    if (pending_login_is_add_account_ && add_account_return_idx_ >= 0 &&
        add_account_return_idx_ < static_cast<int>(account_manager_.accounts().size()))
    {
        switch_active_account(account_manager_.accounts()[add_account_return_idx_]->user_id);
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    }
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
}

void MainWindow::rebuild_account_picker()
{
    if (!account_picker_)
    {
        return;
    }
    std::vector<tesseract::views::AccountEntry> entries;
    const auto& accs = account_manager_.accounts();
    entries.reserve(accs.size());
    for (const auto& sess : accs)
    {
        tesseract::views::AccountEntry e;
        e.user_id = sess->user_id;
        e.display_name = sess->display_name;
        e.avatar_url = sess->avatar_url;
        e.active = (sess->user_id == my_user_id_);
        entries.push_back(std::move(e));
        if (!sess->avatar_url.empty())
        {
            ensure_user_avatar_(sess->avatar_url);
        }
    }
    account_picker_->set_entries(std::move(entries));
    if (account_picker_surface_)
    {
        account_picker_surface_->relayout();
    }
}

void MainWindow::open_account_picker(double /*ax*/, double /*ay*/)
{
    if (account_manager_.accounts().size() < 2)
    {
        return;
    }

    if (!account_picker_popover_)
    {
        // Build once; a GtkPopover parented to the user strip.
        account_picker_surface_ =
            std::make_unique<tk::gtk4::Surface>(current_theme_);
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(make_avatar_image_provider_());
        account_picker_->on_select = [this](const std::string& uid)
        {
            if (account_picker_popover_)
            {
                gtk_popover_popdown(GTK_POPOVER(account_picker_popover_));
            }
            on_account_picker_select_(uid);
        };
        account_picker_->on_avatar_needed =
            [this](const std::string& mxc) { ensure_user_avatar_(mxc); };
        account_picker_surface_->set_root(std::move(picker));

        account_picker_popover_ = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(account_picker_popover_),
                              account_picker_surface_->widget());
        gtk_widget_set_parent(account_picker_popover_,
                              main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(account_picker_popover_),
                                 GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(account_picker_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(account_picker_popover_), TRUE);

        // Size to fit rows.
        const int row_h = 48;
        gtk_widget_set_size_request(account_picker_surface_->widget(), 240,
                                    row_h * static_cast<int>(account_manager_.accounts().size()));
    }

    rebuild_account_picker();
    gtk_popover_popup(GTK_POPOVER(account_picker_popover_));
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    if (!main_app_)
    {
        return;
    }

    auto* tb = main_app_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    main_app_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string name;
            if (const auto* r = room_by_id_(t.room_id))
            {
                name = r->name;
                const std::string& av_mxc = r->effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = account_manager_.thumbnail_cache().peek(av_mxc);
                }
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    if (active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        on_room_selected(active.room_id);
    }

    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

float MainWindow::get_message_scroll_fraction_()
{
    if (!room_view_ || !room_view_->message_list())
    {
        return 0.f;
    }
    return room_view_->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t)
{
    if (!room_view_ || !room_view_->message_list())
    {
        return;
    }
    room_view_->message_list()->scroll_to_offset(t);
}


std::vector<tk::Rect> MainWindow::get_screen_work_areas_() const
{
    std::vector<tk::Rect> result;
    GdkDisplay* display = gdk_display_get_default();
    if (!display)
        return result;
    GListModel* monitors = gdk_display_get_monitors(display);
    const guint n = g_list_model_get_n_items(monitors);
    for (guint i = 0; i < n; ++i)
    {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
        if (!mon) continue;
        GdkRectangle geom{};
        gdk_monitor_get_geometry(mon, &geom);
        result.push_back({static_cast<float>(geom.x),
                          static_cast<float>(geom.y),
                          static_cast<float>(geom.width),
                          static_cast<float>(geom.height)});
        g_object_unref(mon);
    }
    return result;
}

void MainWindow::raise_and_activate_()
{
    if (window_)
        gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::rebuild_tray_()
{
    if (!tray_ || !tray_->is_available())
        return;

    auto items = build_tray_items_();
    tray_->rebuild_menu(std::move(items));
}

bool MainWindow::is_ctrl_held_() const
{
    GdkDisplay* disp = gdk_display_get_default();
    GdkSeat*    seat = disp ? gdk_display_get_default_seat(disp) : nullptr;
    GdkDevice*  kbd  = seat ? gdk_seat_get_keyboard(seat) : nullptr;
    if (!kbd)
        return false;
    GdkModifierType mods = gdk_device_get_modifier_state(kbd);
    return (mods & GDK_CONTROL_MASK) != 0;
}

void MainWindow::switch_active_account_(const std::string& user_id)
{
    switch_active_account(user_id);
}

void MainWindow::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    auto* win = new gtk4::MainWindow(account_manager_, app_);
    win->set_initial_account(account);
    // Shared hand-off: re-point bridge at the new window, seed caches, pin, and
    // register dedicated — before the new window's deferred doLogin().
    hand_account_to_spawned_window_(win, account);
    gtk_window_present(GTK_WINDOW(win->widget()));
}

// ─────────────────────────────────────────────────────────────────────────────

} // namespace gtk4
