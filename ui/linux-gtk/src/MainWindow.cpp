#include "MainWindow.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorGtk.h"
#include "LoginView.h"
#include "views/BrandView.h"
#include "SettingsWidget.h"
#include "LinuxScreenLockGtk.h"
#include "app/SlashCommands.h"

#include "tk/canvas_cairo.h"
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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// g_idle_add helpers (for async workers that are NOT part of EventHandlerBase)
// ---------------------------------------------------------------------------

struct IdlePaginateResult
{
    MainWindow* window;
    std::string room_id;
    bool reached_start;
    std::weak_ptr<bool> alive;
};

struct IdleSubscribeResult
{
    MainWindow* window;
    std::string room_id;
    bool reached_start;
    std::weak_ptr<bool> alive;
};

struct IdleJumpResult
{
    MainWindow* window;
    std::string room_id;
    std::string event_id;
    std::weak_ptr<bool> alive;
};

struct IdleJumpError
{
    MainWindow* window;
    std::string message;
    std::weak_ptr<bool> alive;
};

struct JumpDlgCtx
{
    MainWindow* self;
    GtkWidget* calendar;
    GtkWidget* dialog;
};

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
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }

    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);
    push_notification(user_id, room_id, room_name, sender, body, is_mention,
                      std::move(avatar_bytes), std::move(image_bytes));
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
    on_inflight_ui_();
}

void MainWindow::on_inflight_ui_()
{
    if (!inflight_dot_)
        return;
    const auto   c  = inflight_dot_color_();
    const auto   n  = inflight_total_();
    const size_t fp = pool_pending_count_();
    const size_t sp = mut_pool_pending_count_();
    const size_t mp = pending_media_count_();
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "<span color=\"#%02x%02x%02x\">&#x25cf;</span>",
                  c.r, c.g, c.b);
    gtk_label_set_markup(GTK_LABEL(inflight_dot_), buf);
    char tip[128];
    std::snprintf(tip, sizeof(tip),
                  "%u request%s in flight\nmedia: %zu loading · fetch: %zu queued · send: %zu queued",
                  n, n == 1u ? "" : "s", mp, fp, sp);
    gtk_widget_set_tooltip_text(inflight_dot_, tip);
    // Force-refresh the tooltip window if it is currently shown over this widget.
    gtk_widget_trigger_tooltip_query(inflight_dot_);
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

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    if (status_bar_)
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
}

void MainWindow::on_restore_status_ui_()
{
    refresh_sync_status();
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
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Done);
    main_app_surface_->relayout();
    // Hide after 1.5 s. The payload carries a liveness weak_ptr so a
    // window destroyed within that window doesn't get called on freed `this`.
    struct DoneData
    {
        MainWindow* w;
        std::weak_ptr<bool> alive;
    };
    auto* dd = new DoneData{this, alive_};
    g_timeout_add(
        1500,
        [](gpointer data) -> gboolean
        {
            auto* d = static_cast<DoneData*>(data);
            if (auto a = d->alive.lock(); a && *a)
            {
                auto* self = d->w;
                if (self->verif_shared_ && self->verif_shared_->on_done)
                {
                    self->verif_shared_->on_done();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        },
        dd);
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
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(tesseract::AccountManager& account_manager, GtkApplication* app)
    : ShellBase(account_manager)
    , app_(app)
{
    set_screen_lock_(std::make_unique<LinuxScreenLockGtk>());

    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Tesseract");
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
    branding_surface_->set_root(std::make_unique<tesseract::views::BrandView>());
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
    {
        auto main_app_owner =
            std::make_unique<tesseract::views::MainAppWidget>();
        main_app_ = main_app_owner.get();
        room_list_view_ = main_app_->room_list_view();
        room_view_ = main_app_->room_view();
        verif_shared_ = main_app_->verif_banner();
        img_viewer_ = main_app_->image_viewer();
        vid_viewer_ = main_app_->video_viewer();

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

        // Wire provider callbacks (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // Wire UserInfo callbacks (replaces native GTK user-strip gestures).
        main_app_->user_info()->on_primary = [this](tk::Point world)
        {
            open_account_picker(world.x, world.y);
        };
        main_app_->user_info()->on_secondary = [this](tk::Point world)
        {
            if (!user_popover_)
            {
                return;
            }
            GdkRectangle r = {static_cast<int>(world.x),
                              static_cast<int>(world.y), 1, 1};
            gtk_popover_set_pointing_to(GTK_POPOVER(user_popover_), &r);
            gtk_popover_popup(GTK_POPOVER(user_popover_));
        };

        // Space nav back button.
        main_app_->on_space_back = [this]
        {
            if (!space_stack_.empty())
            {
                space_stack_.pop_back();
            }
            refresh_room_list();
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
                    space_stack_.push_back(room_id);
                    refresh_room_list();
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
                    if (!self->room_list_view_ || !self->client_)
                    {
                        return G_SOURCE_REMOVE;
                    }
                    auto ids = self->room_list_view_->visible_room_ids();
                    self->client_->stop_background_backfill();
                    self->client_->start_background_backfill(ids);
                    return G_SOURCE_REMOVE;
                },
                this);
        };
        room_list_view_->on_search_clear = [this]
        {
            cancel_debounce_(DebounceSlot::RoomSearch);
            search_pending_text_.clear();
            if (room_search_field_)
            {
                room_search_field_->set_text("");
            }
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
        room_list_view_->on_join_room_requested = [this]
        {
            open_join_room_dialog();
        };

        // Wire RoomView shortcode lookup (avatar/image/preview wired via
        // wire_main_app_widget_).
        room_view_->set_shortcode_provider(
            [this](const std::string& mxc) -> std::string
            {
                return shortcode_for_mxc_(mxc);
            });
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

        // Compose text area overlay.
        room_text_area_ = main_app_surface_->host().make_text_area();
        room_text_area_->set_placeholder(_("Message\xe2\x80\xa6"));
        room_text_area_->set_mention_colors(
            main_app_surface_->theme().palette.accent,
            main_app_surface_->theme().palette.text_on_accent);
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
        room_text_area_->set_on_popup_nav(
            [this](tk::NativeTextArea::NavKey nk) -> bool
            {
                return tesseract::views::dispatch_compose_nav(
                    nk, gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get());
            });

        // ── /command autocomplete popup ──────────────────────────────────
        slash_popover_ = gtk_popover_new();
        gtk_widget_set_parent(slash_popover_, main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(slash_popover_), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(slash_popover_), FALSE);
        // autohide=FALSE: a modal popover takes the Wayland keyboard grab and
        // kills composer input while open. Non-autohide keeps the keyboard with
        // the composer; dismissed by the controller on text change / Esc.
        gtk_popover_set_autohide(GTK_POPOVER(slash_popover_), FALSE);
        slash_popup_surface_ =
            std::make_unique<tk::gtk4::Surface>(main_app_surface_->theme());
        {
            auto w = std::make_unique<tesseract::views::SlashCommandPopup>();
            slash_popup_widget_ = w.get();
            slash_popup_surface_->set_root(std::move(w));
            gtk_popover_set_child(GTK_POPOVER(slash_popover_),
                                  slash_popup_surface_->widget());
        }
        {
            tesseract::views::SlashCommandController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_slash_popup_(cursor, rows); };
            sh.hide = [this] { hide_slash_popup_(); };
            sh.repaint = [this]
            {
                if (slash_popup_surface_)
                    slash_popup_surface_->host().request_repaint();
            };
            sh.room_id = [this] { return current_room_id_; };
            sh.client = [this]() -> tesseract::Client* { return client_; };
            sh.clear_composer = [this] { room_view_->clear_compose_text(); };
            slash_controller_ =
                std::make_unique<tesseract::views::SlashCommandController>(
                    room_text_area_.get(), slash_popup_widget_, std::move(sh));
        }

        // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────
        shortcode_popover_ = gtk_popover_new();
        gtk_widget_set_parent(shortcode_popover_, main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(shortcode_popover_), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(shortcode_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(shortcode_popover_), FALSE);
        shortcode_popup_surface_ =
            std::make_unique<tk::gtk4::Surface>(main_app_surface_->theme());
        {
            auto w = std::make_unique<tesseract::views::ShortcodePopup>();
            shortcode_popup_widget_ = w.get();
            shortcode_popup_widget_->set_image_provider(
                make_static_image_provider_());
            shortcode_popup_surface_->set_root(std::move(w));
            gtk_popover_set_child(GTK_POPOVER(shortcode_popover_),
                                  shortcode_popup_surface_->widget());
        }
        {
            tesseract::views::ShortcodeController::Hooks sh;
            sh.show = [this](tk::Rect cursor, int rows)
            { show_shortcode_popup_(cursor, rows); };
            sh.hide = [this] { hide_shortcode_popup_(); };
            sh.repaint = [this]
            {
                if (shortcode_popup_surface_)
                    shortcode_popup_surface_->host().request_repaint();
            };
            sh.emoticons =
                [this]() -> const std::vector<tesseract::ImagePackImage>&
            { return cached_emoticons_; };
            sh.fetch_image = [this](const std::string& url)
            { ensure_media_image_(url, 28, 28); };
            shortcode_controller_ =
                std::make_unique<tesseract::views::ShortcodeController>(
                    room_text_area_.get(), shortcode_popup_widget_,
                    std::move(sh));
        }

        // ── @mention autocomplete popup ───────────────────────────────────
        mention_popover_ = gtk_popover_new();
        gtk_widget_set_parent(mention_popover_, main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(mention_popover_), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(mention_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(mention_popover_), FALSE);
        mention_popup_surface_ =
            std::make_unique<tk::gtk4::Surface>(main_app_surface_->theme());
        {
            auto w = std::make_unique<tesseract::views::MentionPopup>();
            mention_popup_widget_ = w.get();
            mention_popup_widget_->set_image_provider(
                make_avatar_image_provider_());
            mention_popup_surface_->set_root(std::move(w));
            gtk_popover_set_child(GTK_POPOVER(mention_popover_),
                                  mention_popup_surface_->widget());
        }
        {
            tesseract::views::MentionController::Hooks mh;
            mh.show = [this](tk::Rect cursor, int rows)
            { show_mention_popup_(cursor, rows); };
            mh.hide = [this] { hide_mention_popup_(); };
            mh.repaint = [this]
            {
                if (mention_popup_surface_)
                    mention_popup_surface_->host().request_repaint();
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
                    room_text_area_.get(), client_, mention_popup_widget_,
                    std::move(mh));
        }

        // ── GIF picker (/gif <query>) ────────────────────────────────────────
        gif_popover_ = gtk_popover_new();
        gtk_widget_set_parent(gif_popover_, main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(gif_popover_), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(gif_popover_), FALSE);
        // Non-autohide so the composer keeps focus (nav is forwarded from the
        // text area); the controller pops it down on text change / send.
        gtk_popover_set_autohide(GTK_POPOVER(gif_popover_), FALSE);
        gif_popup_surface_ =
            std::make_unique<tk::gtk4::Surface>(main_app_surface_->theme());
        {
            auto w = std::make_unique<tesseract::views::GifPopup>();
            gif_popup_widget_ = w.get();
            gif_popup_surface_->set_root(std::move(w));
        }
        gif_popup_surface_->set_anim_cache(&account_manager_.anim_cache());
        gtk_popover_set_child(GTK_POPOVER(gif_popover_),
                              gif_popup_surface_->widget());
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
                    auto alive = gif_alive_;
                    auto url = result.preview_url;
                    run_async_(
                        [this, url, alive, repaint]
                        {
                            // Disk-cache the preview too, symmetrically with the
                            // animated source. Otherwise a GIF whose MP4 is
                            // already on disk loads its video faster than its
                            // preview downloads, leaving the cell blank until
                            // the video appears instead of the thumbnail first.
                            const std::string disk_key = gif_src_disk_key_(url);
                            std::vector<std::uint8_t> bytes =
                                account_manager_.media_disk_cache().load(disk_key);
                            if (bytes.empty() && client_)
                            {
                                bytes = client_->fetch_url_bytes(url);
                                if (!bytes.empty())
                                    account_manager_.media_disk_cache().store(disk_key, bytes);
                            }
                            post_to_ui_(
                                [this, url, b = std::move(bytes), alive,
                                 repaint]() mutable
                                {
                                    if (!*alive)
                                        return;
                                    gif_preview_inflight_.erase(url);
                                    if (b.empty())
                                        return;
                                    using CW = tesseract::views::GifPopup;
                                    DecodedImage d = decode_image_(
                                        b, int(CW::kCellW) * 2,
                                        int(CW::kCellH) * 2);
                                    if (d.still)
                                        gif_previews_[url] = std::move(d.still);
                                    repaint();
                                });
                        });
                }
                // Kick off the strip-display fetch (strip_url: WebP/GIF) — decode
                // on the worker thread. The MP4 send form is fetched at send time.
                if (gif_anim_inflight_.insert(result.strip_url).second)
                {
                    auto alive = gif_alive_;
                    auto anim_url = result.strip_url;
                    auto anim_mime = result.strip_mime;
                    run_async_(
                        [this, anim_url, anim_mime, alive, repaint]
                        {
                            // Source bytes: disk cache first, else download and
                            // persist so the send path reuses them.
                            const std::string disk_key =
                                gif_src_disk_key_(anim_url);
                            std::vector<std::uint8_t> bytes =
                                account_manager_.media_disk_cache().load(disk_key);
                            if (bytes.empty() && client_)
                            {
                                bytes = client_->fetch_url_bytes(anim_url);
                                if (!bytes.empty())
                                    account_manager_.media_disk_cache().store(disk_key, bytes);
                            }
                            using CW = tesseract::views::GifPopup;
                            if (!bytes.empty() && anim_mime == "video/mp4")
                            {
                                tk::DecodedVideoFrames dvf =
                                    tk::decode_video_frames(
                                        bytes.data(), bytes.size(),
                                        int(CW::kCellW) * 2,
                                        int(CW::kCellH) * 2);
                                // Convert BGRA → cairo ARGB32 surface → Image.
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
                                    [this, anim_url, imgs,
                                     delays = std::move(delays), alive,
                                     repaint]() mutable
                                    {
                                        if (!*alive)
                                            return;
                                        gif_anim_inflight_.erase(anim_url);
                                        if (!imgs->empty())
                                        {
                                            account_manager_.anim_cache().store(
                                                anim_url, std::move(*imgs),
                                                std::move(delays),
                                                g_get_monotonic_time() / 1000);
                                            start_anim_tick_if_needed_();
                                        }
                                        repaint();
                                    });
                            }
                            else
                            {
                                auto d = std::make_shared<DecodedImage>(
                                    bytes.empty()
                                        ? DecodedImage{}
                                        : decode_image_(bytes,
                                                        int(CW::kCellW) * 2,
                                                        int(CW::kCellH) * 2));
                                post_to_ui_(
                                    [this, anim_url, d, alive, repaint]() mutable
                                    {
                                        if (!*alive)
                                            return;
                                        gif_anim_inflight_.erase(anim_url);
                                        if (!d->frames.empty())
                                        {
                                            account_manager_.anim_cache().store(
                                                anim_url, std::move(d->frames),
                                                std::move(d->delays_ms),
                                                g_get_monotonic_time() / 1000);
                                            start_anim_tick_if_needed_();
                                        }
                                        else if (d->still)
                                        {
                                            gif_previews_[anim_url] =
                                                std::move(d->still);
                                        }
                                        repaint();
                                    });
                            }
                        });
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
                                               if (gif_popup_surface_)
                                                   gtk_widget_queue_draw(
                                                       gif_popup_surface_
                                                           ->widget());
                                           });
            });
        {
            tesseract::views::GifController::Hooks gh;
            gh.show = [this] { show_gif_popup_(); };
            gh.hide = [this] { hide_gif_popup_(); };
            gh.repaint = [this]
            {
                if (gif_popup_surface_)
                    gtk_widget_queue_draw(gif_popup_surface_->widget());
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
                room_text_area_.get(), gif_popup_widget_, std::move(gh));
        }

        room_text_area_->set_on_edit_last(
            [this]
            {
                return room_view_ && room_view_->edit_last_own();
            });
        room_text_area_->set_on_height_changed(
            [this](float h)
            {
                room_view_->set_text_area_natural_height(h);
                main_app_surface_->relayout();
            });
        room_text_area_->set_on_image_paste(
            [this](std::vector<std::uint8_t> bytes, std::string mime)
            {
                if (room_view_)
                {
                    room_view_->compose_bar()->set_pending_image(
                        std::move(bytes), std::move(mime));
                }
            });

        // Topic edit text area overlay.
        topic_text_area_ = main_app_surface_->host().make_text_area();
        topic_text_area_->set_on_changed([this](const std::string& t)
        {
            if (main_app_) main_app_->room_view()->set_topic_edit_text(t);
        });
        topic_text_area_->set_visible(false);

        // File drop. Shared dispatch routes the payload into the compose bar
        // by MIME type; the per-shell hook probes video/audio + gif animation.
        auto on_file_drop = [this](std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename)
        {
            if (!room_view_)
                return;
            const auto limit = client_->media_upload_limit();
            auto outcome = tesseract::views::dispatch_file_drop(
                *room_view_->compose_bar(), std::move(bytes), std::move(mime),
                std::move(filename), limit,
                [this](std::uint32_t gen, std::vector<std::uint8_t> b,
                       std::string m)
                { extract_drop_media_(gen, std::move(b), std::move(m)); });
            if (outcome == tesseract::views::FileDropOutcome::TooLarge)
                show_status_message_("File exceeds the upload limit");
        };
        main_app_surface_->set_on_file_drop(on_file_drop);
        main_app_surface_->set_on_file_drop_error(
            [this](std::string reason)
            {
                show_status_message_(std::move(reason));
            });

        room_view_->on_layout_changed = [this]
        {
            main_app_surface_->relayout();
        };

        room_view_->on_send = [this](const std::string& body)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            // Build from the composer's mention draft so inline pills become
            // matrix.to links + m.mentions; fall back to the plain body.
            std::vector<tesseract::MentionSeg> draft =
                room_text_area_ ? room_text_area_->mention_draft()
                                : std::vector<tesseract::MentionSeg>{};
            bool has_mention = false;
            for (const auto& seg : draft)
            {
                if (seg.kind == tesseract::MentionSeg::Kind::Mention)
                {
                    has_mention = true;
                }
            }
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            std::string trimmed = tesseract::text::trim(msg.body);
            if (trimmed.empty() && !has_mention)
            {
                return;
            }
            auto outcome = dispatch_room_send_(current_room_id_, msg.body,
                                               msg.formatted_body);
            if (outcome.handled_as_command || outcome.send_result)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
        };
        room_view_->on_send_reply =
            [this](const std::string& reply_event_id, const std::string& body)
        {
            if (body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res =
                client_->send_reply(current_room_id_, reply_event_id, body);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send reply failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_edit =
            [this](const std::string& event_id, const std::string& new_body)
        {
            if (new_body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_edit(current_room_id_, event_id, new_body);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg = std::string(_("Edit failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_image =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int src_w,
                   int src_h, bool is_animated, std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            tesseract::Result res;
            if (is_animated)
            {
                // Animated GIF/WebP: send the original bytes verbatim via
                // the MSC4230 raw path. Re-encoding would flatten the
                // animation to a single frame.
                res = client_->send_image(
                    current_room_id_, bytes, mime, filename, caption,
                    static_cast<std::uint32_t>(src_w < 0 ? 0 : src_w),
                    static_cast<std::uint32_t>(src_h < 0 ? 0 : src_h),
                    /*is_animated=*/true, reply_event_id);
            }
            else
            {
                const bool compress =
                    tesseract::Settings::instance().image_quality ==
                    tesseract::Settings::ImageQuality::Compressed;
                auto enc = main_app_surface_->host().encode_for_send(
                    bytes.data(), bytes.size(), compress);
                if (enc.bytes.empty())
                {
                    return;
                }
                std::string out_name = filename;
                if (enc.mime == "image/jpeg")
                {
                    auto dot = out_name.find_last_of('.');
                    if (dot != std::string::npos)
                    {
                        out_name = out_name.substr(0, dot);
                    }
                    out_name += ".jpg";
                }
                res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
                                          out_name, caption, enc.width,
                                          enc.height, /*is_animated=*/false,
                                          reply_event_id);
            }
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send image failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_video =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int w, int h,
                   std::vector<std::uint8_t> thumb_bytes, int thumb_w,
                   int thumb_h, std::uint64_t duration_ms,
                   std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_video(
                current_room_id_, bytes, mime, filename, caption,
                static_cast<std::uint32_t>(w < 0 ? 0 : w),
                static_cast<std::uint32_t>(h < 0 ? 0 : h), thumb_bytes,
                static_cast<std::uint32_t>(thumb_w < 0 ? 0 : thumb_w),
                static_cast<std::uint32_t>(thumb_h < 0 ? 0 : thumb_h),
                duration_ms, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send video failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_audio =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::uint64_t duration_ms, std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res =
                client_->send_audio(current_room_id_, bytes, mime, filename,
                                     caption, duration_ms, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send audio failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_file =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_file(current_room_id_, bytes, mime,
                                          filename, caption, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send file failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_edit_cancelled = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->clear_compose_text();
        };
        room_view_->on_reply_focus = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_focused(true);
            }
        };
        room_view_->on_focus_input = [this]
        {
            if (room_text_area_)
                room_text_area_->set_focused(true);
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
        room_view_->on_delete_requested = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->redact_event(current_room_id_, event_id);
        };
        room_view_->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key,
                   const std::string& source_mxc)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            if (!source_mxc.empty())
            {
                // For MSC4027 chips matrix-sdk aggregates by the mxc:// key
                // (so `key` IS the mxc URI). Look up the shortcode locally
                // so the outgoing event carries `:shortcode:` rather than
                // re-broadcasting the URI as its own shortcode.
                std::string sc = shortcode_for_mxc_(source_mxc);
                std::string shortcode =
                    sc.empty() ? std::string() : ":" + sc + ":";
                client_->send_reaction_custom(current_room_id_, event_id,
                                              source_mxc, shortcode);
                return;
            }
            client_->send_reaction(current_room_id_, event_id, key);
        };
        room_view_->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor)
        {
            if (!emoji_popover_ || current_room_id_.empty())
            {
                return;
            }
            pending_reaction_event_id_ = event_id;
            popup_emoji_at_rect(main_app_surface_->widget(), anchor);
        };
        setup_link_clicked_(room_view_);
        room_view_->on_set_clipboard = [this](std::string_view t)
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
        room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
        {
            GtkWidget* w = main_app_surface_->widget();
            if (!topic_tooltip_popover_)
            {
                topic_tooltip_label_ = gtk_label_new(nullptr);
                gtk_label_set_wrap(GTK_LABEL(topic_tooltip_label_), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(topic_tooltip_label_),
                                              60);
                topic_tooltip_popover_ = gtk_popover_new();
                gtk_widget_add_css_class(topic_tooltip_popover_, "tooltip");
                gtk_popover_set_child(GTK_POPOVER(topic_tooltip_popover_),
                                      topic_tooltip_label_);
                gtk_widget_set_parent(topic_tooltip_popover_, w);
                gtk_popover_set_autohide(GTK_POPOVER(topic_tooltip_popover_),
                                         FALSE);
                gtk_popover_set_has_arrow(GTK_POPOVER(topic_tooltip_popover_),
                                          FALSE);
            }
            gtk_label_set_text(GTK_LABEL(topic_tooltip_label_), text.c_str());
            GdkRectangle rect{
                static_cast<int>(anchor.x), static_cast<int>(anchor.y),
                static_cast<int>(anchor.w), static_cast<int>(anchor.h)};
            gtk_popover_set_pointing_to(GTK_POPOVER(topic_tooltip_popover_),
                                        &rect);
            gtk_popover_popup(GTK_POPOVER(topic_tooltip_popover_));
        };
        room_view_->on_hide_tooltip = [this]
        {
            if (topic_tooltip_popover_)
            {
                gtk_popover_popdown(GTK_POPOVER(topic_tooltip_popover_));
            }
        };
        room_view_->on_receipt_needed = [this](const std::string& eid)
        {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        room_view_->message_list()->on_tile_needed = [this](int z, int x, int y)
        {
            ensure_tile_async(z, x, y);
        };
        room_view_->on_near_top = [this]
        {
            if (current_room_id_.empty())
            {
                return;
            }
            request_more_history(current_room_id_);
        };
        room_view_->on_near_bottom = [this]
        {
            if (!current_room_id_.empty())
            {
                request_forward_history_(current_room_id_);
            }
        };
        room_view_->on_return_to_live = [this]
        {
            if (!current_room_id_.empty())
            {
                return_to_live_(current_room_id_);
            }
        };
        room_view_->on_scroll_to_original = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string room = current_room_id_;
            begin_focused_subscription_(room, event_id);
            run_async_mut_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        room_view_->on_jump_to_date_requested = [this]
        {
            open_jump_to_date_dialog();
        };
        room_view_->on_threads_button_clicked = [this]
        {
            on_threads_button_clicked();
        };
        room_view_->on_pin_requested =
            [this](const std::string& ev) { on_pin_requested(ev); };
        room_view_->on_unpin_requested =
            [this](const std::string& ev) { on_unpin_requested(ev); };
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
            [this](const std::string& body, const std::string& formatted)
        {
            on_thread_send_requested(body, formatted);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_thread_send_reply =
            [this](const std::string& reply_id,
                   const std::string& body,
                   const std::string& formatted)
        {
            on_thread_send_reply_requested(reply_id, body, formatted);
            if (room_text_area_)
                room_text_area_->set_text("");
            room_view_->set_current_text({});
        };
        room_view_->on_emoji = [this](tk::Rect btn)
        {
            if (!emoji_popover_)
            {
                return;
            }
            if (gtk_widget_get_visible(emoji_popover_))
            {
                gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
            }
            else
            {
                popup_emoji_at_rect(main_app_surface_->widget(), btn);
            }
        };
        room_view_->on_sticker = [this](tk::Rect btn)
        {
            if (!sticker_popover_)
            {
                return;
            }
            if (gtk_widget_get_visible(sticker_popover_))
            {
                gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
            }
            else
            {
                popup_sticker_at_rect(main_app_surface_->widget(), btn);
            }
        };
        room_view_->on_fetch_room_members = [this](std::string room_id)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_([this, c, room_id = std::move(room_id)]() mutable
            {
                auto members = c->get_room_members(room_id);
                post_to_ui_([this, members = std::move(members)]() mutable
                {
                    if (room_view_)
                    {
                        for (const auto& m : members)
                            ensure_user_avatar_(m.avatar_url);
                        room_view_->set_room_members(std::move(members));
                    }
                });
            });
        };
        room_view_->on_save_topic = [this](std::string room_id, std::string t)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_([c, room_id = std::move(room_id), t = std::move(t)]() mutable
            {
                c->set_room_topic(room_id, t);
            });
        };
        room_view_->on_leave_room = [this](std::string room_id)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_([this, c, room_id = std::move(room_id)]() mutable
            {
                auto res = c->leave_room(room_id);
                post_to_ui_([this, room_id, ok = res.ok]() mutable
                {
                    if (!main_app_ || !ok) return;
                    if (current_room_id_ == room_id)
                    {
                        current_room_id_.clear();
                        if (room_view_) room_view_->clear_room();
                        if (main_app_) main_app_->room_list_view()->set_selected_room("");
                        if (main_app_surface_) main_app_surface_->relayout();
                    }
                });
            });
        };
        setup_dm_callbacks();
        room_view_->on_ignore_user = [this](std::string user_id)
        {
            if (!client_) return;
            auto* c = client_;
            run_async_mut_([c, user_id = std::move(user_id)]() mutable
            {
                c->ignore_user(user_id);
            });
        };

        // Image + video viewers — providers / repaint / on_close.
        wire_main_app_viewers_(
            main_app_, main_app_surface_->host(),
            [this]
            {
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            });

        room_view_->on_image_clicked =
            [this](const tesseract::views::MessageListView::ImageHit& hit)
        {
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            img_viewer_->open(src_tok, thumb_tok, hit.body,
                              hit.natural_w, hit.natural_h);
            main_app_->show_image_viewer(true);
            main_app_surface_->relayout();
            gtk_widget_grab_focus(main_app_surface_->widget());
            ensure_viewer_fullres_(src_tok);
        };

        // Avatar click → open the lightbox with the original avatar mxc.
        // Overrides the basic wiring in ShellBase::wire_main_app_widget_
        // (which only shows the cached thumbnail) so ensure_media_image_
        // fetches the original bytes and decodes them at native size into
        // tk_images_; the viewer's image_provider picks that up over the
        // tk_avatars_ entry.
        room_view_->on_avatar_clicked =
            [this](std::string url, std::string name)
        {
            if (url.empty() || !img_viewer_)
                return;
            img_viewer_->open(url, url, name, 0, 0);
            main_app_->show_image_viewer(true);
            main_app_surface_->relayout();
            gtk_widget_grab_focus(main_app_surface_->widget());
            ensure_viewer_fullres_(url);
        };

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
                        c->self->run_async_(
                            [self = c->self, url = std::move(url), dest]()
                            {
                                auto bytes = self->client_->fetch_source_bytes(url);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
                    }
                    if (err) g_error_free(err);
                    delete c;
                },
                ctx);
            g_object_unref(dlg);
        };

        room_view_->on_video_clicked =
            [this](const tesseract::views::MessageListView::VideoHit& hit)
        {
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            vid_viewer_->open(src_tok, thumb_tok, hit.mime_type,
                              hit.duration_ms, hit.natural_w, hit.natural_h,
                              hit.loop, hit.no_audio, hit.hide_controls);
            main_app_->show_video_viewer(true);
            main_app_surface_->relayout();
            gtk_widget_grab_focus(main_app_surface_->widget());
            std::string src = src_tok;
            run_async_(
                [this, src = std::move(src), walive = std::weak_ptr<bool>(alive_)]() mutable
                {
                    auto bytes = client_->fetch_source_bytes(src);
                    struct Ctx
                    {
                        MainWindow* self;
                        std::vector<uint8_t> bytes;
                        std::weak_ptr<bool> alive;
                    };
                    auto* ctx = new Ctx{this, std::move(bytes), walive};
                    g_idle_add(
                        [](gpointer p) -> gboolean
                        {
                            auto* c = static_cast<Ctx*>(p);
                            if (auto a = c->alive.lock(); a && *a)
                            {
                                if (c->self->vid_viewer_)
                                {
                                    c->self->vid_viewer_->load_bytes(
                                        c->bytes.data(), c->bytes.size());
                                }
                            }
                            delete c;
                            return G_SOURCE_REMOVE;
                        },
                        ctx);
                });
        };

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
                        c->self->run_async_(
                            [self = c->self, json_src = std::move(json_src), dest]()
                            {
                                auto bytes = self->client_->fetch_source_bytes(json_src);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
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
                        c->self->run_async_(
                            [self = c->self, url = std::move(url), dest]()
                            {
                                auto bytes = self->client_->fetch_source_bytes(url);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
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
                run_async_(
                    [this, src, on_ready = std::move(on_ready)]() mutable
                    {
                        auto bytes = client_->fetch_source_bytes(src);
                        struct Ctx
                        {
                            std::function<void(std::vector<std::uint8_t>)> cb;
                            std::vector<std::uint8_t> bytes;
                        };
                        auto* ctx =
                            new Ctx{std::move(on_ready), std::move(bytes)};
                        g_idle_add(
                            [](gpointer p) -> gboolean
                            {
                                auto* c = static_cast<Ctx*>(p);
                                c->cb(std::move(c->bytes));
                                delete c;
                                return G_SOURCE_REMOVE;
                            },
                            ctx);
                    });
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

        // Room search field overlay.
        room_search_field_ = main_app_surface_->host().make_text_field();
        room_search_field_->set_placeholder("Search");
        room_search_field_->set_visible(false);
        room_search_field_->set_on_changed(
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

        // Quick switcher (Ctrl+K) search field.
        quick_switch_field_ = main_app_surface_->host().make_text_field();
        quick_switch_field_->set_placeholder(
            "Jump to a room, or @user to start a chat…");
        quick_switch_field_->set_visible(false);
        quick_switch_field_->set_on_changed(
            [this](const std::string& q)
            {
                if (main_app_ && main_app_->quick_switcher())
                {
                    main_app_->quick_switcher()->set_query(q);
                    main_app_surface_->relayout();
                }
            });
        quick_switch_field_->set_on_submit(
            [this]
            {
                if (main_app_ && main_app_->quick_switcher())
                    main_app_->quick_switcher()->activate_selected();
            });
        quick_switch_field_->set_on_popup_nav(
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
        if (main_app_ && main_app_->quick_switcher())
            main_app_->quick_switcher()->on_close = [this]
            { close_quick_switch_(); };

        // Unified layout callback — positions all native overlays.
        main_app_surface_->set_on_layout(
            [this]
            {
                if (!main_app_)
                {
                    return;
                }

                bool search_visible = main_app_->room_search_field_visible();
                if (room_search_field_)
                {
                    room_search_field_->set_visible(search_visible);
                    if (search_visible)
                    {
                        room_search_field_->set_rect(
                            main_app_->room_search_field_rect());
                    }
                }

                if (quick_switch_field_)
                {
                    const bool qs_vis = main_app_->quick_switch_field_visible();
                    quick_switch_field_->set_visible(qs_vis);
                    if (qs_vis)
                    {
                        quick_switch_field_->set_rect(
                            main_app_->quick_switch_field_rect());
                    }
                }

                if (room_text_area_)
                {
                    const tk::Rect ta = main_app_->compose_text_area_rect();
                    room_text_area_->set_visible(!ta.empty());
                    if (!ta.empty())
                        room_text_area_->set_rect(ta);
                }

                if (topic_text_area_)
                {
                    const tk::Rect tr = main_app_->room_view()->topic_edit_rect();
                    const bool was_visible = topic_text_area_->visible();
                    topic_text_area_->set_visible(!tr.empty());
                    if (!tr.empty())
                    {
                        topic_text_area_->set_rect(tr);
                        if (!was_visible)
                            topic_text_area_->set_text(
                                main_app_->room_view()->topic_edit_initial_text());
                    }
                }

                if (enc_passphrase_field_)
                {
                    bool visible =
                        main_app_->encryption_setup_passphrase_field_visible();
                    enc_passphrase_field_->set_visible(visible);
                    if (visible)
                    {
                        enc_passphrase_field_->set_rect(
                            main_app_->encryption_setup_passphrase_field_rect());
                    }
                }

                if (enc_key_field_)
                {
                    bool visible =
                        main_app_->encryption_setup_key_field_visible();
                    enc_key_field_->set_visible(visible);
                    if (visible)
                    {
                        enc_key_field_->set_rect(
                            main_app_->encryption_setup_key_field_rect());
                    }
                }
            });

        main_app_surface_->set_root(std::move(main_app_owner));
    }

    // User context menu (right-click on user strip) — parented to main_app_surface_
    // so it can float anywhere in the window. Position is set via pointing_to in
    // UserInfo::on_secondary.
    {
        GMenu* top = g_menu_new();

        GMenu* main_section = g_menu_new();
        g_menu_append(main_section, _("Settings\xe2\x80\xa6"), "user.settings");
        g_menu_append(main_section, _("Add Account\xe2\x80\xa6"),
                      "user.add_account");
        g_menu_append(main_section, _("Log Out"), "user.logout");
        g_menu_append_section(top, nullptr, G_MENU_MODEL(main_section));
        g_object_unref(main_section);

        GMenu* quit_section = g_menu_new();
        g_menu_append(quit_section, _("Quit"), "user.quit");
        g_menu_append_section(top, nullptr, G_MENU_MODEL(quit_section));
        g_object_unref(quit_section);

        user_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(top));
        gtk_widget_set_parent(user_popover_, main_app_surface_->widget());
        gtk_popover_set_has_arrow(GTK_POPOVER(user_popover_), FALSE);
        g_object_unref(top);

        GSimpleActionGroup* group = g_simple_action_group_new();
        {
            GSimpleAction* act = g_simple_action_new("settings", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_settings_activate_),
                             this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("add_account", nullptr);
            g_signal_connect(act, "activate",
                             G_CALLBACK(on_add_account_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("logout", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_logout_activate_),
                             this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("quit", nullptr);
            g_signal_connect(act, "activate",
                             G_CALLBACK(on_quit_user_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        gtk_widget_insert_action_group(main_app_surface_->widget(), "user",
                                       G_ACTION_GROUP(group));
        g_object_unref(group);
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

        settings_widget_->on_close = [this]
        {
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
        settings_widget_->on_send_presence_changed = [this](bool enabled)
        {
            handle_send_presence_toggle_(enabled);
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
    inflight_dot_ = gtk_label_new("●");
    gtk_widget_set_margin_end(inflight_dot_, 6);
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

    {
        struct LoginCtx
        {
            MainWindow* self;
            std::weak_ptr<bool> alive;
        };
        auto* lctx = new LoginCtx{this, alive_};
        g_idle_add(
            [](gpointer data) -> gboolean
            {
                auto* d = static_cast<LoginCtx*>(data);
                if (auto a = d->alive.lock(); a && *a)
                    d->self->do_login();
                delete d;
                return G_SOURCE_REMOVE;
            },
            lctx);
    }

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

void MainWindow::start_tray_if_needed_()
{
    if (tray_)
    {
        return;
    }
    tray_ = std::make_unique<GtkSniTrayIcon>(
        [this]
        {
            // If the unread room is popped out, raise that window instead.
            if (focus_tray_unread_popout_())
                return;
            gtk_window_present(GTK_WINDOW(window_));
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
    }
}

gboolean MainWindow::on_window_close_request_(GtkWindow* /*window*/,
                                              gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->tray_ && self->tray_->is_available())
    {
        gtk_widget_set_visible(self->window_, FALSE);
        return TRUE; // stop default destruction
    }
    if (self->active_account_ &&
        self->account_manager_.dedicated_window(self->active_account_->user_id) == self)
        self->account_manager_.clear_dedicated(self->active_account_->user_id);
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
    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    return prefer_dark ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
}

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    if (branding_surface_)
    {
        branding_surface_->set_theme(t);
    }
    if (main_app_surface_)
    {
        main_app_surface_->set_theme(t);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(t);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(t);
    }
    if (join_room_surface_)
    {
        join_room_surface_->set_theme(t);
    }
    if (account_picker_surface_)
    {
        account_picker_surface_->set_theme(t);
    }
    if (settings_widget_)
    {
        settings_widget_->set_theme(t);
    }
    if (slash_popup_surface_)
    {
        slash_popup_surface_->set_theme(t);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(t);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
    }
    if (room_text_area_)
    {
        room_text_area_->set_mention_colors(t.palette.accent,
                                            t.palette.text_on_accent);
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
    // unregister_window is called in on_window_close_request_ so it is not
    // repeated here.  broadcast_rebuild_tray_ is still needed to refresh any
    // remaining windows' tray menus after this C++ shell is freed.
    broadcast_rebuild_tray_();

    // Invalidate all outstanding g_idle_add / g_timeout_add payloads that
    // captured a weak_ptr<bool> from alive_.  Setting the flag before
    // draining workers ensures no idle fires on a half-destroyed `this`.
    *alive_ = false;

    if (theme_css_provider_)
    {
        g_object_unref(theme_css_provider_);
        theme_css_provider_ = nullptr;
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
    // GTK4 top-level windows hold their own reference and must be destroyed
    // explicitly; they are not freed when their transient parent is destroyed.
    if (join_room_dialog_window_)
    {
        gtk_window_destroy(GTK_WINDOW(join_room_dialog_window_));
        join_room_dialog_window_ = nullptr;
    }
    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    for (auto& sess : account_manager_.accounts())
    {
        if (sess->sync_started)
            sess->client->stop_sync();
    }
    if (pending_login_client_)
        pending_login_client_->stop_sync();
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
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    start_tray_if_needed_();
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

    // Migrate + restore every stored account (shared loop in ShellBase). The
    // native per-account notifier / UnifiedPush construction runs through
    // install_account_notifier_ / install_account_up_connector_ below.
    auto restore = restore_all_accounts_();

    if (restore.any_accounts)
    {
        finish_login_ui_(restore.active_uid);
        return;
    }

    // No accounts: fresh install or all restores failed → show login view.
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
        login_view_->show_restore_error(restore.restore_error,
                                        [this] { do_login(); });
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
        });
}

void MainWindow::install_account_up_connector_(tesseract::AccountSession& session)
{
    // Per-account UnifiedPush connector.
    auto up = std::make_unique<LinuxUpConnectorGtk>();
    up->start(session.client.get(), session.user_id);
    session.up_connector = std::move(up);
}

void MainWindow::on_login_succeeded()
{
    if (!pending_login_client_)
    {
        return; // defensive
    }

    // The LoginView holds a raw alias to pending_login_client_; clear it before
    // finalize_login_ resets the client underneath us.
    login_view_->set_client(nullptr);

    // Agnostic add-account core (see ShellBase::finalize_login_).
    const auto fin = finalize_login_();

    if (fin.rejected_duplicate)
    {
        gtk_label_set_text(GTK_LABEL(status_bar_),
                           ("Already signed in as " + fin.user_id).c_str());
        if (pending_login_is_add_account_ && add_account_return_idx_ >= 0 &&
            add_account_return_idx_ < static_cast<int>(account_manager_.accounts().size()))
        {
            switch_active_account(account_manager_.accounts()[add_account_return_idx_]->user_id);
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
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    start_tray_if_needed_();
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
}

void MainWindow::bind_settings_controller_()
{
    // settings_controller_ is freshly constructed by
    // ShellBase::ensure_settings_controller_(); install the native key/file
    // dialog hooks and bind it to the native settings widget.
    wire_key_dialog_callbacks_();
    if (settings_widget_)
        settings_widget_->set_controller(settings_controller_.get(),
                                         my_display_name_);
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
    for (const auto& r : rooms_)
    {
        if (r.id == room_id && r.is_space)
        {
            space_stack_.push_back(room_id);
            refresh_room_list();
            return;
        }
    }

    // Route through the controllers so their visible_ state stays in sync.
    if (slash_controller_)
        slash_controller_->hide();
    if (shortcode_controller_)
        shortcode_controller_->hide();
    if (mention_controller_)
        mention_controller_->hide();
    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

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
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        prefs.open_rooms.clear();
        for (const auto& t : tabs_)
            prefs.open_rooms.push_back(t.room_id);
        if (prefs.open_rooms.empty())
            prefs.open_rooms.push_back(current_room_id_);
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
    }
    if (room_text_area_)
    {
        room_text_area_->set_text("");
    }
    if (room_text_area_)
    {
        room_text_area_->set_focused(true);
    }
    if (room_view_)
    {
        room_view_->clear_compose_text();
    }

    for (const auto& r : rooms_)
    {
        if (r.id == current_room_id_)
        {
            room_view_->set_room(r);
            break;
        }
    }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the GTK main loop stays responsive.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    std::string sub_room = current_room_id_;
    {
        auto& state = pagination_[sub_room];
        if (state.in_flight)
            return;
        state.in_flight = true;
    }
    run_async_mut_(
        [this, sub_room, visible_ids = std::move(visible_ids)]
        {
            auto res = client_->subscribe_room(sub_room);
            bool reached = false;
            if (res)
            {
                auto pr = client_->paginate_back_with_status(sub_room,
                                                             kPaginationBatch);
                reached = pr.ok && pr.reached_start;
                client_->start_background_backfill(visible_ids);
            }
            auto* d = new IdleSubscribeResult{this, sub_room, reached, alive_};
            g_idle_add(
                [](gpointer data) -> gboolean
                {
                    auto* dd = static_cast<IdleSubscribeResult*>(data);
                    if (auto a = dd->alive.lock(); a && *a)
                        dd->window->push_subscribe_result(std::move(dd->room_id),
                                                          dd->reached_start);
                    delete dd;
                    return G_SOURCE_REMOVE;
                },
                d);
        });
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

void MainWindow::push_subscribe_result(std::string room_id, bool reached_start)
{
    // Always clear in_flight regardless of current room — otherwise navigating
    // away before the worker finishes permanently blocks re-subscription.
    auto& state = pagination_[room_id];
    state.in_flight = false;
    if (room_id != current_room_id_)
    {
        return;
    }
    state.reached_start = reached_start;
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

    // Worker thread: invoke the blocking SDK call, marshal the result
    // back via g_idle_add on the main loop.
    run_async_(
        [this, room_id]
        {
            auto pr =
                client_->paginate_back_with_status(room_id, kPaginationBatch);
            auto* p = new IdlePaginateResult{this, room_id,
                                             pr.ok && pr.reached_start, alive_};
            g_idle_add(
                [](gpointer data) -> gboolean
                {
                    auto* d = static_cast<IdlePaginateResult*>(data);
                    if (auto a = d->alive.lock(); a && *a)
                        d->window->push_paginate_result(std::move(d->room_id),
                                                        d->reached_start);
                    delete d;
                    return G_SOURCE_REMOVE;
                },
                p);
        });
}

void MainWindow::open_jump_to_date_dialog()
{
    if (current_room_id_.empty())
    {
        return;
    }

    auto* dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), _("Jump to Date"));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(window_));
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 300, -1);

    auto* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    auto* calendar = gtk_calendar_new();
    gtk_box_append(GTK_BOX(vbox), calendar);

    auto* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    auto* cancel_btn = gtk_button_new_with_label(_("Cancel"));
    auto* ok_btn = gtk_button_new_with_label(_("Jump"));
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);
    gtk_box_append(GTK_BOX(btn_row), ok_btn);
    gtk_box_append(GTK_BOX(vbox), btn_row);

    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    auto* ctx = new JumpDlgCtx{this, calendar, dlg};
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_jump_dialog_ok_), ctx);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_jump_dialog_cancel_),
                     ctx);
    // "destroy" fires when the window is torn down (by button OR window-manager X).
    // It is the single place we free ctx; button callbacks must not free it themselves.
    g_signal_connect(dlg, "destroy", G_CALLBACK(on_jump_dialog_destroy_), ctx);

    gtk_window_present(GTK_WINDOW(dlg));
}

void MainWindow::on_jump_dialog_cancel_(GtkButton*, gpointer user_data)
{
    auto* ctx = static_cast<JumpDlgCtx*>(user_data);
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    // ctx freed by on_jump_dialog_destroy_
}

void MainWindow::on_jump_dialog_ok_(GtkButton*, gpointer user_data)
{
    auto* ctx = static_cast<JumpDlgCtx*>(user_data);
    MainWindow* self = ctx->self;

    // Extract date BEFORE destroying the dialog (which unrefs all children).
    GDateTime* gdt = gtk_calendar_get_date(GTK_CALENDAR(ctx->calendar));
    int year, month, day;
    g_date_time_get_ymd(gdt, &year, &month, &day);
    g_date_time_unref(gdt);

    const std::string room_id = self->current_room_id_;
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    // ctx freed by on_jump_dialog_destroy_; do NOT access ctx after this point.

    if (room_id.empty())
    {
        return;
    }

    // Reject pre-epoch dates to avoid uint64_t wrap-around.
    if (year < 1970)
    {
        if (self->status_bar_)
        {
            gtk_label_set_text(
                GTK_LABEL(self->status_bar_),
                _("Jump to date: please select a date from 1970 onwards"));
        }
        return;
    }

    GTimeZone* utc_tz = g_time_zone_new_utc();
    GDateTime* midnight = g_date_time_new(utc_tz, year, month, day, 0, 0, 0.0);
    g_time_zone_unref(utc_tz);
    const gint64 unix_s = g_date_time_to_unix(midnight);
    g_date_time_unref(midnight);

    const uint64_t ts_ms = static_cast<uint64_t>(unix_s) * 1000ULL;

    self->run_async_mut_(
        [self, room_id, ts_ms]
        {
            auto res = self->client_->timestamp_to_event(room_id, ts_ms, "f");
            if (!res.ok)
            {
                auto* e = new IdleJumpError{self, res.message, self->alive_};
                g_idle_add(
                    [](gpointer p) -> gboolean
                    {
                        auto* d = static_cast<IdleJumpError*>(p);
                        if (auto a = d->alive.lock(); a && *a)
                        {
                            if (d->window->status_bar_)
                            {
                                gtk_label_set_text(
                                    GTK_LABEL(d->window->status_bar_),
                                    d->message.c_str());
                            }
                        }
                        delete d;
                        return G_SOURCE_REMOVE;
                    },
                    e);
                return;
            }
            auto* d = new IdleJumpResult{self, room_id, res.message, self->alive_};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* data = static_cast<IdleJumpResult*>(p);
                    if (auto a = data->alive.lock(); a && *a)
                    {
                        MainWindow* w = data->window;
                        std::string rid = std::move(data->room_id);
                        std::string eid = std::move(data->event_id);
                        delete data;
                        w->begin_focused_subscription_(rid, eid);
                        w->run_async_mut_(
                            [w, rid, eid]
                            {
                                w->client_->subscribe_room_at(rid, eid);
                            });
                        return G_SOURCE_REMOVE;
                    }
                    delete data;
                    return G_SOURCE_REMOVE;
                },
                d);
        });
}

void MainWindow::on_jump_dialog_destroy_(GtkWidget*, gpointer user_data)
{
    delete static_cast<JumpDlgCtx*>(user_data);
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

void MainWindow::on_space_children_cache_ready_ui_()
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
    if (account_manager_.anim_cache().empty())
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
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
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

void MainWindow::repaint_anim_frame_()
{
    // GTK4 has no partial-widget invalidation (gtk_widget_queue_draw_area was
    // removed; the render-node model only supports whole-widget queue_draw),
    // so we can't scope the repaint to the animated rects the way Qt6/macOS
    // do. We still avoid the per-frame measure + arrange pass: a plain repaint
    // is enough since frame swaps never change layout. Pickers keep their
    // existing invalidation.
    if (main_app_surface_)
    {
        main_app_surface_->host().request_repaint();
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (gif_popup_surface_ && gif_popup_visible_())
        gif_popup_surface_->update_anim_regions();
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
    if (space_stack_.empty())
    {
        if (!search_pending_text_.empty())
        {
            if (main_app_)
            {
                main_app_->set_space_nav(false);
            }
            show_rooms(rooms_);
            return;
        }
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
        {
            if (!r.is_space)
            {
                continue;
            }
            auto sc_it = space_children_cache_.find(r.id);
            if (sc_it != space_children_cache_.end())
            {
                for (const auto& id : sc_it->second)
                {
                    in_space.insert(id);
                }
            }
        }
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        for (const auto& r : rooms_)
        {
            if (r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        apply_space_child_counts_(filtered);
        if (main_app_)
        {
            main_app_->set_space_nav(false);
        }
        show_rooms(filtered);
    }
    else
    {
        const std::string& space_id = space_stack_.back();
        static const std::vector<std::string> kNoChildren;
        const auto sc_it = space_children_cache_.find(space_id);
        const auto& child_ids =
            sc_it != space_children_cache_.end() ? sc_it->second : kNoChildren;
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
            {
                filtered.push_back(r);
            }
        }
        if (main_app_)
        {
            for (const auto& r : rooms_)
            {
                if (r.id == space_id)
                {
                    ensure_room_avatar_(r);
                    main_app_->set_space_nav(true, r.name, r.avatar_url);
                    break;
                }
            }
        }
        show_rooms(filtered);
    }
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
    struct Data
    {
        std::function<void()> fn;
        std::weak_ptr<bool> alive;
    };
    auto* d = new Data{std::move(fn), alive_};
    g_idle_add(
        [](gpointer p) -> gboolean
        {
            auto* data = static_cast<Data*>(p);
            if (auto a = data->alive.lock(); a && *a)
                data->fn();
            delete data;
            return G_SOURCE_REMOVE;
        },
        d);
}

void MainWindow::post_to_ui_after_(int ms, std::function<void()> fn)
{
    struct Data
    {
        std::function<void()> fn;
        std::weak_ptr<bool> alive;
    };
    auto* d = new Data{std::move(fn), alive_};
    g_timeout_add(
        static_cast<guint>(ms),
        [](gpointer p) -> gboolean
        {
            auto* data = static_cast<Data*>(p);
            if (auto a = data->alive.lock(); a && *a)
                data->fn();
            delete data;
            return G_SOURCE_REMOVE;
        },
        d);
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
                           kind == MediaKind::MediaThumbnail);

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
                            if (main_app_surface_)
                            {
                                main_app_surface_->relayout();
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
                        if (kind == MediaKind::MediaThumbnail && room_view_)
                        {
                            room_view_->notify_image_ready(cache_key);
                        }
                    }
                    else
                    {
                        account_manager_.image_cache().store(cache_key, std::move(img));
                        if (kind == MediaKind::Tile && room_view_)
                        {
                            room_view_->message_list()->invalidate_data();
                        }
                        else if (kind == MediaKind::MediaImage && room_view_)
                        {
                            room_view_->notify_image_ready(cache_key);
                        }
                    }
                    if (main_app_surface_)
                    {
                        main_app_surface_->relayout();
                    }
                    if (kind == MediaKind::MediaImage &&
                        shortcode_popup_visible_() && shortcode_popup_surface_)
                    {
                        shortcode_popup_surface_->relayout();
                    }
                    if (is_avatar && account_picker_surface_ &&
                        account_picker_popover_ &&
                        gtk_widget_get_visible(account_picker_popover_))
                    {
                        account_picker_surface_->relayout();
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
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
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
            struct Ctx
            {
                MainWindow* mw;
                tesseract::views::MediaInfo info;
                std::weak_ptr<bool> alive;            // main window liveness
                tesseract::views::ComposeBar* target; // null → main compose bar
                std::shared_ptr<bool> target_alive;   // pop-out liveness
            };
            auto* ctx = new Ctx{this, std::move(info), alive_, target,
                                std::move(target_alive)};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (c->target)
                    {
                        // Pop-out window: post to its compose bar while it lives.
                        if (c->target_alive && *c->target_alive)
                            c->target->update_pending_attachment(c->info);
                    }
                    else if (auto a = c->alive.lock(); a && *a)
                    {
                        if (c->mw->room_view_)
                            c->mw->room_view_->compose_bar()
                                ->update_pending_attachment(c->info);
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& video_url)
{
    const std::string eid = event_id;
    run_async_(
        [this, eid, src = video_url]() mutable
        {
            auto bytes = client_->fetch_source_bytes(src);
            if (bytes.empty())
            {
                return;
            }
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
                return;
            }
            GstMapInfo map;
            if (!gst_buffer_map(buf, &map, GST_MAP_READ))
            {
                gst_sample_unref(sample);
                return;
            }
            std::vector<uint8_t> frame_bytes(map.data, map.data + map.size);
            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
            // BGRA → cairo surface on the main thread.
            struct Ctx
            {
                MainWindow* self;
                std::string key;
                std::vector<uint8_t> pixels;
                int w, h;
                std::weak_ptr<bool> alive;
            };
            auto* ctx =
                new Ctx{this, "thumb::" + eid, std::move(frame_bytes), w, h, alive_};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (auto a = c->alive.lock(); a && *a)
                    {
                        if (!c->self->account_manager_.image_cache().contains(c->key))
                        {
                            // Create an owned cairo surface and blit the BGRA pixels in.
                            cairo_surface_t* surf = cairo_image_surface_create(
                                CAIRO_FORMAT_ARGB32, c->w, c->h);
                            if (surf &&
                                cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
                            {
                                int dst_stride =
                                    cairo_image_surface_get_stride(surf);
                                unsigned char* dst =
                                    cairo_image_surface_get_data(surf);
                                int src_stride = c->w * 4;
                                for (int row = 0; row < c->h; ++row)
                                {
                                    std::memcpy(
                                        dst + row * dst_stride,
                                        c->pixels.data() + row * src_stride,
                                        static_cast<std::size_t>(src_stride));
                                }
                                cairo_surface_mark_dirty(surf);
                                c->self->account_manager_.image_cache().store(
                                    c->key, tk::cairo_pango::make_image(surf));
                                cairo_surface_destroy(surf);
                                if (c->self->main_app_surface_)
                                {
                                    c->self->main_app_surface_->relayout();
                                }
                            }
                            else if (surf)
                            {
                                cairo_surface_destroy(surf);
                            }
                        }
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
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

void MainWindow::open_join_room_dialog_ui_(const std::string& prefill)
{
    open_join_room_dialog();
    if (!prefill.empty() && join_room_shared_)
    {
        join_room_shared_->set_alias_text(prefill);
        if (join_room_alias_field_)
            join_room_alias_field_->set_text(prefill);
    }
}

void MainWindow::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    if (!main_app_)
        return;
    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // Reconfigure the overlay (clears prior callbacks) before re-creating the
    // native fields, then wire the shared callbacks via ShellBase.
    ov->reset(mode);

    enc_passphrase_field_ = main_app_surface_->host().make_text_field();
    enc_passphrase_field_->set_password(true);
    enc_key_field_ = main_app_surface_->host().make_text_field();
    enc_key_field_->set_password(false);

    wire_encryption_setup_callbacks_(*ov, main_app_surface_->host(),
                                     enc_passphrase_field_.get(),
                                     enc_key_field_.get());

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
                                   std::vector<uint8_t> image_bytes)
{
    handle_notification(user_id, room_id, room_name, sender, body, is_mention,
                        std::move(avatar_bytes), std::move(image_bytes));
}

void MainWindow::handle_notification(const std::string& user_id,
                                     const std::string& room_id,
                                     const std::string& room_name,
                                     const std::string& sender,
                                     const std::string& body, bool is_mention,
                                     std::vector<uint8_t> avatar_bytes,
                                     std::vector<uint8_t> image_bytes)
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
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_emoticon_packs();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
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
    if (sync_progress_shown_)
    {
        sync_progress_shown_ = false;
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    }
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

void MainWindow::on_add_account_activate_(GSimpleAction* /*action*/,
                                          GVariant* /*parameter*/,
                                          gpointer user_data)
{
    gtk_popover_popdown(
        GTK_POPOVER(static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->begin_add_account();
}

void MainWindow::on_logout_activate_(GSimpleAction* /*action*/,
                                     GVariant* /*parameter*/,
                                     gpointer user_data)
{
    gtk_popover_popdown(
        GTK_POPOVER(static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->logout_active_account();
}

void MainWindow::on_settings_activate_(GSimpleAction* /*action*/,
                                       GVariant* /*param*/, gpointer self)
{
    static_cast<MainWindow*>(self)->open_settings_();
}

void MainWindow::on_quit_user_activate_(GSimpleAction* /*action*/,
                                        GVariant* /*parameter*/,
                                        gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    gtk_popover_popdown(GTK_POPOVER(self->user_popover_));
    self->tray_.reset();
    g_application_quit(G_APPLICATION(self->app_));
}

void MainWindow::open_settings_()
{
    settings_widget_->populate(
        my_display_name_, my_user_id_, my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        { return account_manager_.thumbnail_cache().peek(mxc); },
        tesseract::Settings::instance().theme_pref,
        tesseract::Settings::instance().notifications_enabled);
    settings_widget_->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    settings_widget_->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
    settings_widget_->set_autoscroll_unread_pref(
        tesseract::Settings::instance().autoscroll_unread_rooms);
    if (settings_controller_)
        settings_widget_->set_controller(settings_controller_.get(),
                                         my_display_name_);

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
    // the already-populated popover at the caret.
    if (!shortcode_popover_ || !shortcode_popup_surface_)
    {
        return;
    }
    int w = int(tesseract::views::ShortcodePopup::kWidth);
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    gtk_widget_set_size_request(shortcode_popup_surface_->widget(), w, h);
    GdkRectangle rect{int(cursor_local.x), int(cursor_local.y),
                      int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(shortcode_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(shortcode_popover_));
}

void MainWindow::hide_shortcode_popup_()
{
    if (shortcode_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(shortcode_popover_));
    }
}

// ── Slash-command popup ────────────────────────────────────────────────────

void MainWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    if (!slash_popover_ || !slash_popup_surface_)
    {
        return;
    }
    int w = int(tesseract::views::SlashCommandPopup::kWidth);
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    gtk_widget_set_size_request(slash_popup_surface_->widget(), w, h);
    GdkRectangle rect{int(cursor_local.x), int(cursor_local.y),
                      int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(slash_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(slash_popover_));
}

void MainWindow::hide_slash_popup_()
{
    if (slash_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(slash_popover_));
    }
}

void MainWindow::show_gif_popup_()
{
    if (!gif_popover_ || !gif_popup_widget_ || !room_text_area_ ||
        !main_app_surface_ || !gif_popup_surface_)
    {
        return;
    }
    // Full-width strip spanning the compose bar, floating just above it (like
    // the attachment preview band). content_size() drives only the height and
    // the empty/status check; the width comes from the compose bar. The popover
    // (no arrow, POS_TOP) is centred on the bar rect, so a full-bar-width
    // size_request makes it span the column above the bar.
    const tk::Rect cb = room_view_ ? room_view_->compose_bar_rect() : tk::Rect{};
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    const int w = int(cb.w);
    const int h = int(sz.h);
    gtk_widget_set_size_request(gif_popup_surface_->widget(), w, h);

    GdkRectangle rect{int(cb.x), int(cb.y), int(cb.w), int(cb.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(gif_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(gif_popover_));
}

void MainWindow::hide_gif_popup_()
{
    if (gif_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(gif_popover_));
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
    if (!mention_popover_ || !mention_popup_surface_)
    {
        return;
    }
    int w = int(tesseract::views::MentionPopup::kWidth);
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);
    gtk_widget_set_size_request(mention_popup_surface_->widget(), w, h);
    GdkRectangle rect{int(cursor_local.x), int(cursor_local.y),
                      int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(mention_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(mention_popover_));
}

void MainWindow::hide_mention_popup_()
{
    if (mention_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(mention_popover_));
    }
}

// ---------------------------------------------------------------------------
// Emoji picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::EmojiPicker. The search row is a native
// GtkEntry overlaid by the Surface; selection routes back through the
// shared widget's on_selected callback.
// ---------------------------------------------------------------------------

void MainWindow::build_emoji_popover()
{
    emoji_popover_ = gtk_popover_new();
    gtk_widget_set_parent(emoji_popover_, main_app_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(emoji_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(emoji_popover_), TRUE);

    emoji_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(client_);
    emoji_picker_shared_->on_selected = [this](const std::string& glyph)
    {
        emoji_selected(glyph);
    };
    emoji_picker_shared_->on_emoticon_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        emoticon_selected(img);
    };
    // Async fetch for custom emoticon images — mirrors the sticker picker.
    emoji_picker_shared_->set_image_provider(
        make_picker_image_provider_(false));
    emoji_picker_surface_->set_root(std::move(shared));

    // Native GtkEntry overlay for the search row. The shared widget paints
    // the affordance; the entry handles IME + selection natively.
    emoji_picker_search_field_ =
        emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder(_("Search emoji"));
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (emoji_picker_shared_)
            {
                emoji_picker_shared_->set_search_query(q);
            }
            if (emoji_picker_surface_)
            {
                emoji_picker_surface_->relayout();
            }
        });
    emoji_picker_surface_->set_on_layout(
        [this]
        {
            if (emoji_picker_search_field_ && emoji_picker_shared_)
            {
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

void MainWindow::build_sticker_popover()
{
    sticker_popover_ = gtk_popover_new();
    gtk_widget_set_parent(sticker_popover_, main_app_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(sticker_popover_), TRUE);

    sticker_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (current_room_id_.empty())
        {
            return;
        }
        std::string body = img.body.empty() ? img.shortcode : img.body;
        if (thread_panel_ == ThreadPanel::Open && !current_thread_root_.empty())
            client_->send_thread_sticker(current_room_id_,
                                         current_thread_root_, body,
                                         img.url, img.info_json);
        else
            client_->send_sticker(current_room_id_, body, img.url, img.info_json);
        if (sticker_popover_)
        {
            gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        }
    };
    // Share the same caches the message list reads from. Animated
    // entries take priority; static entries are the second hop; on
    // miss kick off an async fetch via the shared `ensure_picker_image_`
    // so the next paint after the worker posts back finds the bitmap.
    sticker_picker_shared_->set_image_provider(
        make_picker_image_provider_(true));
    sticker_picker_surface_->set_root(std::move(shared));

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder(_("Search stickers"));
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (sticker_picker_shared_)
            {
                sticker_picker_shared_->set_search_query(q);
            }
            if (sticker_picker_surface_)
            {
                sticker_picker_surface_->relayout();
            }
        });
    sticker_picker_surface_->set_on_layout(
        [this]
        {
            if (sticker_picker_search_field_ && sticker_picker_shared_)
            {
                sticker_picker_search_field_->set_rect(
                    sticker_picker_shared_->search_field_rect());
            }
        });

    GtkWidget* surface_widget = sticker_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 360, 420);
    gtk_popover_set_child(GTK_POPOVER(sticker_popover_), surface_widget);
}

void MainWindow::toggle_sticker_picker()
{
    if (!sticker_popover_)
    {
        return;
    }
    if (gtk_widget_get_visible(sticker_popover_))
    {
        gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        return;
    }
    GtkWidget* desired_parent =
        main_app_surface_ ? main_app_surface_->widget() : nullptr;
    if (desired_parent &&
        gtk_widget_get_parent(sticker_popover_) != desired_parent)
    {
        gtk_widget_unparent(sticker_popover_);
        gtk_widget_set_parent(sticker_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), nullptr);
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

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

    if (!self->room_view_ || !self->sticker_ctx_menu_)
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
    self->open_quick_switch_();
    return TRUE;
}

gboolean MainWindow::on_nav_back_shortcut_(GtkWidget*, GVariant*,
                                           gpointer user_data)
{
    static_cast<MainWindow*>(user_data)->navigate_history_back();
    return TRUE;
}

gboolean MainWindow::on_nav_fwd_shortcut_(GtkWidget*, GVariant*,
                                          gpointer user_data)
{
    static_cast<MainWindow*>(user_data)->navigate_history_forward();
    return TRUE;
}

void MainWindow::open_quick_switch_()
{
    if (!main_app_ || !main_app_->quick_switcher())
        return;
    main_app_->show_quick_switch(true);
    if (main_app_surface_)
        main_app_surface_->relayout();
    if (quick_switch_field_)
    {
        quick_switch_field_->set_text("");
        quick_switch_field_->set_focused(true);
    }
}

void MainWindow::close_quick_switch_()
{
    if (main_app_)
        main_app_->show_quick_switch(false);
    if (quick_switch_field_)
        quick_switch_field_->set_visible(false);
    if (main_app_surface_)
        main_app_surface_->relayout();
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
        // Quick switcher is the topmost modal — close it first.
        if (self->main_app_ && self->main_app_->quick_switcher() &&
            self->main_app_->quick_switcher()->is_open())
        {
            self->close_quick_switch_();
            return TRUE;
        }
        if (self->vid_viewer_ && self->vid_viewer_->is_open())
        {
            self->vid_viewer_->close();
            if (self->main_app_)
            {
                self->main_app_->show_video_viewer(false);
            }
            if (self->main_app_surface_)
            {
                self->main_app_surface_->relayout();
            }
            return TRUE;
        }
        if (self->img_viewer_ && self->img_viewer_->is_open())
        {
            self->img_viewer_->close();
            if (self->main_app_)
            {
                self->main_app_->show_image_viewer(false);
            }
            if (self->main_app_surface_)
            {
                self->main_app_surface_->relayout();
            }
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

void MainWindow::toggle_emoji_picker()
{
    if (!emoji_popover_)
    {
        return;
    }
    if (gtk_widget_get_visible(emoji_popover_))
    {
        gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        return;
    }
    // Compose-bar path: ensure the popover is parented to the main surface
    // and clear any prior `pointing_to` from a reaction popup.
    GtkWidget* desired_parent =
        main_app_surface_ ? main_app_surface_->widget() : nullptr;
    if (desired_parent &&
        gtk_widget_get_parent(emoji_popover_) != desired_parent)
    {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), nullptr);
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

void MainWindow::popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect)
{
    if (!emoji_popover_ || !parent)
    {
        return;
    }
    // Reparent the popover to the target widget so `pointing_to` is
    // interpreted in that widget's coordinate space.
    if (gtk_widget_get_parent(emoji_popover_) != parent)
    {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, parent);
    }
    GdkRectangle r{
        .x = static_cast<int>(local_rect.x),
        .y = static_cast<int>(local_rect.y),
        .width = static_cast<int>(local_rect.w),
        .height = static_cast<int>(local_rect.h),
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

void MainWindow::popup_sticker_at_rect(GtkWidget* parent, tk::Rect local_rect)
{
    if (!sticker_popover_ || !parent)
    {
        return;
    }
    if (gtk_widget_get_parent(sticker_popover_) != parent)
    {
        gtk_widget_unparent(sticker_popover_);
        gtk_widget_set_parent(sticker_popover_, parent);
    }
    GdkRectangle r{
        .x = static_cast<int>(local_rect.x),
        .y = static_cast<int>(local_rect.y),
        .width = static_cast<int>(local_rect.w),
        .height = static_cast<int>(local_rect.h),
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

void MainWindow::emoji_selected(const std::string& glyph)
{
    // Reaction mode: a "+" chip set pending_reaction_event_id_ before
    // opening the picker. Route the glyph through send_reaction
    // (Rust-side toggle) and skip the compose insert.
    if (!pending_reaction_event_id_.empty())
    {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty())
        {
            client_->send_reaction(current_room_id_, ev, glyph);
        }
        if (emoji_popover_)
        {
            gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        }
        return;
    }
    if (!room_text_area_)
    {
        return;
    }
    room_text_area_->insert_at_cursor(glyph);
    if (room_view_)
    {
        room_view_->set_current_text(room_text_area_->text());
    }
    room_text_area_->set_focused(true);
    // The shared picker already calls recent_emoji_bump before invoking
    // this callback. Keep the popover open so users can pick several.
}

void MainWindow::emoticon_selected(const tesseract::ImagePackImage& img)
{
    if (img.url.empty())
    {
        return;
    }
    // Reaction mode (parallel to emoji_selected): send an MSC4027
    // custom-image reaction with the mxc key + `:shortcode:`.
    if (!pending_reaction_event_id_.empty())
    {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty())
        {
            client_->send_reaction_custom(current_room_id_, ev, img.url,
                                          ":" + img.shortcode + ":");
        }
        if (emoji_popover_)
        {
            gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        }
        return;
    }
    // Compose mode: insert `:shortcode:` text into the compose field.
    if (!room_text_area_)
    {
        return;
    }
    room_text_area_->insert_at_cursor(":" + img.shortcode + ":");
    if (room_view_)
    {
        room_view_->set_current_text(room_text_area_->text());
    }
    room_text_area_->set_focused(true);
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

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_client(client_);
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_client(client_);
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

    if (result.ok)
    {
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Signed out"));
    }

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
            std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
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

// ---------------------------------------------------------------------------
// Join room dialog — transient GtkWindow hosting JoinRoomView.
// ---------------------------------------------------------------------------

void MainWindow::build_join_room_dialog()
{
    join_room_dialog_window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(join_room_dialog_window_),
                         _("Join a Room"));
    gtk_window_set_modal(GTK_WINDOW(join_room_dialog_window_), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(join_room_dialog_window_),
                                 GTK_WINDOW(window_));
    gtk_window_set_resizable(GTK_WINDOW(join_room_dialog_window_), FALSE);
    gtk_window_set_default_size(
        GTK_WINDOW(join_room_dialog_window_),
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredW),
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredH));

    join_room_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto jrv = std::make_unique<tesseract::views::JoinRoomView>();
    join_room_shared_ = jrv.get();

    join_room_shared_->set_avatar_provider(make_avatar_image_provider_());

    join_room_shared_->on_lookup_requested = [this](const std::string& alias)
    {
        if (!client_ || alias.empty())
        {
            return;
        }
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Loading);
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        uint32_t gen = join_room_gen_;
        auto snap = client_;
        run_async_(
            [this, alias, gen, snap]
            {
                tesseract::RoomSummary s = snap->get_room_summary(alias);
                post_to_ui_(
                    [this, s = std::move(s), gen]
                    {
                        if (!join_room_shared_ || join_room_gen_ != gen)
                        {
                            return;
                        }
                        if (s.ok())
                        {
                            join_room_shared_->set_preview(s);
                        }
                        else
                        {
                            join_room_shared_->set_error(_("Room not found."));
                        }
                        if (join_room_surface_)
                        {
                            join_room_surface_->relayout();
                        }
                    });
            });
    };

    join_room_shared_->on_join_requested =
        [this](const std::string& room_id_or_alias)
    {
        if (!client_ || room_id_or_alias.empty())
        {
            return;
        }
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Joining);
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        uint32_t gen = join_room_gen_;
        auto snap = client_;
        run_async_mut_(
            [this, room_id_or_alias, gen, snap]
            {
                std::string canonical_id = snap->join_room(room_id_or_alias);
                post_to_ui_(
                    [this, canonical_id, gen]
                    {
                        if (!join_room_shared_ || join_room_gen_ != gen)
                        {
                            return;
                        }
                        if (!canonical_id.empty())
                        {
                            if (join_room_dialog_window_)
                            {
                                gtk_widget_set_visible(join_room_dialog_window_,
                                                       FALSE);
                            }
                            navigate_to_room(canonical_id);
                        }
                        else
                        {
                            join_room_shared_->set_error(_("Join failed."));
                            if (join_room_surface_)
                            {
                                join_room_surface_->relayout();
                            }
                        }
                    });
            });
    };

    join_room_shared_->on_cancel = [this]
    {
        if (join_room_dialog_window_)
        {
            gtk_widget_set_visible(join_room_dialog_window_, FALSE);
        }
    };

    join_room_surface_->set_root(std::move(jrv));

    join_room_alias_field_ = join_room_surface_->host().make_text_field();
    join_room_alias_field_->set_placeholder(_("#room:server.org"));
    join_room_alias_field_->set_on_changed(
        [this](const std::string& text)
        {
            if (join_room_shared_)
            {
                join_room_shared_->set_alias_text(text);
            }
        });
    join_room_surface_->set_on_layout(
        [this]
        {
            if (join_room_alias_field_ && join_room_shared_)
            {
                join_room_alias_field_->set_rect(
                    join_room_shared_->alias_field_rect());
                join_room_alias_field_->set_visible(
                    join_room_shared_->alias_field_visible());
            }
        });

    GtkWidget* surface_widget = join_room_surface_->widget();
    gtk_window_set_child(GTK_WINDOW(join_room_dialog_window_), surface_widget);
}

void MainWindow::open_join_room_dialog()
{
    if (!join_room_dialog_window_)
    {
        return;
    }

    ++join_room_gen_; // invalidate any in-flight lookup/join callbacks

    if (join_room_shared_)
    {
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Idle);
        join_room_shared_->set_alias_text("");
    }
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_text("");
    }
    if (join_room_surface_)
    {
        join_room_surface_->relayout();
    }

    gtk_window_present(GTK_WINDOW(join_room_dialog_window_));
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_focused(true);
    }
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
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id)
                {
                    continue;
                }
                name = r.name;
                const std::string& av_mxc = r.effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = account_manager_.thumbnail_cache().peek(av_mxc);
                }
                break;
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
        if (!active.compose_draft.empty())
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(active.compose_draft);
            }
            if (room_view_)
            {
                room_view_->set_current_text(active.compose_draft);
            }
        }
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

std::string MainWindow::get_compose_draft_()
{
    if (!room_view_ || !room_view_->compose_bar())
    {
        return {};
    }
    return room_view_->compose_bar()->current_text();
}

void MainWindow::set_compose_draft_(const std::string& draft)
{
    if (room_text_area_)
    {
        room_text_area_->set_text(draft);
    }
    if (room_view_)
    {
        room_view_->set_current_text(draft);
    }
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
    account_manager_.set_dedicated(account->user_id, win);
    gtk_window_present(GTK_WINDOW(win->widget()));
}

// ─────────────────────────────────────────────────────────────────────────────

} // namespace gtk4
