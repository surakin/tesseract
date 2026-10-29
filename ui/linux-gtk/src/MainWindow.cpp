#include "MainWindow.h"
#include "AuxWindow.h"
#include "CallWindow.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorGtk.h"
#include "views/BrandView.h"
#include "tk/i18n.h"
#include "app/jxl_decode.h"
#include "tk/image_sniff.h"
#include "platform/linux/linux_autostart.h"
#include "LinuxPowerMonitorGtk.h"
#include "LinuxScreenLockGtk.h"
#include "app/DeferredTeardown.h"
#include "views/SlashCommands.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif

#include "tk/canvas_cairo.h"
#include "tk/inflight_dot.h"
#include "tk/status_icons.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include "platform/video_decode.h"
#include "views/media_drop.h"
#include "views/SettingsView.h"
#include "views/settings/PronounsEditor.h"
#include "views/settings/TimezonePicker.h"
#include "views/shortcut_registry.h"
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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    // (refresh_user_strip_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status_();
    on_inflight_ui_();
}

bool MainWindow::tray_available_() const
{
    return tray_ && tray_->is_available();
}

void MainWindow::on_inflight_ui_()
{
    if (!inflight_dot_)
        return;
    gtk_widget_queue_draw(inflight_dot_);
    const std::string tip = inflight_tooltip_text_();
    gtk_widget_set_tooltip_text(inflight_dot_, tip.c_str());
    gtk_widget_trigger_tooltip_query(inflight_dot_);
}

void MainWindow::refresh_low_power_icon_()
{
    if (!low_power_label_)
        return;
    const int scale = gtk_widget_get_scale_factor(low_power_label_);
    const int phys = std::max(1, 14 * (scale > 0 ? scale : 1));
    auto rgba = tk::rasterize_svg_rgba(tk::low_power_icon_svg(), phys,
                                       current_theme_.palette.text_secondary);
    if (static_cast<int>(rgba.size()) != phys * phys * 4)
    {
        gtk_image_clear(GTK_IMAGE(low_power_label_));
        return;
    }
    GBytes* bytes = g_bytes_new(rgba.data(), rgba.size());
    GdkTexture* tex = gdk_memory_texture_new(
        phys, phys, GDK_MEMORY_R8G8B8A8, bytes, static_cast<gsize>(phys) * 4);
    g_bytes_unref(bytes);
    if (tex)
    {
        gtk_image_set_from_paintable(GTK_IMAGE(low_power_label_),
                                     GDK_PAINTABLE(tex));
        g_object_unref(tex);
    }
}

void MainWindow::on_low_power_mode_ui_(bool active)
{
    if (low_power_label_)
        gtk_widget_set_visible(low_power_label_, active ? TRUE : FALSE);
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
    refresh_sync_status_();
}

void MainWindow::on_startup_restore_progress_ui_(const std::string& status)
{
    if (branding_view_)
    {
        branding_view_->set_status(status);
    }
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
    set_power_monitor_(std::make_unique<LinuxPowerMonitorGtk>());
    set_autostart_(std::make_unique<LinuxAutostart>("tesseract-matrix-gtk"));

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
        static_cast<int>(tesseract::visual::kMinWindowWidth),
        static_cast<int>(tesseract::visual::kMinWindowHeight));
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

    // The login page is created lazily by ShellBase::ensure_login_view_
    // (through create_login_surface_ui_) before each show.

    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // Apply saved window size (GTK4/Wayland: position is compositor-managed).
    {
        const auto& saved = tesseract::Settings::instance().main_window_geometry;
        if (saved.valid && saved.w > 0 && saved.h > 0)
            gtk_window_set_default_size(GTK_WINDOW(window_), saved.w, saved.h);
    }

    // Save window size to Settings on every resize (debounced 500 ms).
    // GTK4 fires notify::default-width/-height as the user resizes the window.
    // Also the only per-window resize hook GTK4 offers with app context, so
    // it doubles as the popup-dismiss hook below: a popup's screen position
    // is captured once when it opens and never recomputed, so leaving one
    // open across a resize would strand it away from whatever control
    // anchored it — close everything instead.
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
                       self->dismiss_popups_on_resize_();
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
                       self->dismiss_popups_on_resize_();
                   }),
        this);

    // Single surface hosting the full main-app widget tree.
    main_app_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    wire_main_surface_(static_cast<float>(gtk_widget_get_scale_factor(main_app_surface_->widget())));

    // The per-frame media-prefetch item cap stays lower here than on Qt6/Win32/macOS purely as a glycin
    // subprocess contention guard (gdk-pixbuf routes decode through a
    // sandboxed subprocess — see decode_image_'s override in this file) —
    // the deadline in run_media_prefetch_impl_ already bounds worst-case
    // UI-thread stall time uniformly across all 4 platforms, so this is a
    // tuning knob, not a correctness requirement.
    media_prefetch_max_items_ = 6;
    {
        auto main_app_owner = create_main_app_();

        create_main_room_pane_();

        // Wire provider callbacks (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // Wire UserInfo callbacks (replaces native GTK user-strip gestures).
        main_app_->user_info()->on_primary = [this](tk::Point world)
        {
            open_account_picker_();
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
                [this] { logout_active_account_ui_(); },
                [this] { tray_.reset(); g_application_quit(G_APPLICATION(app_)); },
                verify_session_menu_callback_());

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

        // Mention-pill avatar provider + on_fetch_room_members (which
        // populates the cache it reads) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), using
        // RoomPane's own cached_room_members_/cached_members_room_ instead
        // of this window's now-removed copy of those fields.
        wire_room_view_services_();

        // The composer's popups (@mention, /command, :shortcode:, /gif) and the
        // text area's change/submit/navigation wiring are the shared
        // ComposerPopups.
        create_compose_popups_(main_app_surface_->host());
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

        // on_send / on_send_reply / on_send_edit / on_send_image /
        // on_send_video / on_send_audio / on_send_file / on_send_gallery
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_), a
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
        // on_delete_requested / on_copy_event_source_requested /
        // on_reaction_toggled already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_) — equivalent bodies, so the
        // duplicate assignments that used to be here are gone. See
        // RoomPane.cpp's delete_event_/copy_event_source_to_clipboard_/
        // toggle_reaction_ for the shared implementation (toggle_reaction_
        // includes the same MSC4027 shortcode lookup this block used to do
        // inline).
        setup_link_clicked_(room_view_);
        room_view_->on_show_copy_menu = [this]()
        {
            // Rebuilt per show: the label is "Copy" or "Copy link" depending
            // on what was right-clicked, and a GMenu label is fixed at build.
            if (copy_ctx_menu_)
            {
                gtk_widget_unparent(copy_ctx_menu_);
                copy_ctx_menu_ = nullptr;
            }
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
            using HC = tesseract::views::MessageListView::HoverCursor;
            const HC hc = tesseract::views::MessageListView::hover_cursor_for(url);
            GtkWidget* w = main_app_surface_->widget();
            gtk_widget_set_cursor_from_name(
                w, hc == HC::Text ? "text" : hc == HC::Pointer ? "pointer"
                                                               : "default");
        };
        main_app_->space_root()->on_link_hovered = [this](const std::string& url)
        {
            GtkWidget* w = main_app_surface_->widget();
            gtk_widget_set_cursor_from_name(w, url.empty() ? "default"
                                                           : "pointer");
        };
        main_app_->on_sidebar_cursor =
            [this](tesseract::views::MainAppWidget::SidebarCursor c)
        {
            using SC = tesseract::views::MainAppWidget::SidebarCursor;
            const char* name = c == SC::Resize   ? "col-resize"
                               : c == SC::Toggle ? "pointer"
                                                 : "default";
            gtk_widget_set_cursor_from_name(main_app_surface_->widget(), name);
        };
        // on_receipt_needed / on_member_pronoun_needed / on_near_top /
        // on_near_bottom / on_return_to_live / on_scroll_to_original already
        // provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_ + RoomPane::request_pagination_back_,
        // which now also calls set_paginating(true)/reset_near_top_latch()
        // to match this window's old request_more_history spinner behavior
        // — see RoomPane.cpp).
        // on_date_jump already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_) — this call was fully redundant with
        // it (and silently overrode it, since it ran after attach()).
        // on_threads_button_clicked / on_thread_open_requested /
        // on_thread_close_requested / on_thread_send / on_thread_send_reply
        // already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_) — equivalent bodies, including the
        // mention/emoticon draft rebuild on send. See RoomPane.cpp's
        // apply_thread_transition_/on_thread_send/on_thread_send_reply for
        // the shared implementation.
        // on_pin_requested / on_unpin_requested already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ ->
        // pin_event_/unpin_event_, equivalent to ShellBase::on_pin_requested/
        // on_unpin_requested's bodies).
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
        // on_ignore_user already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_).

        // on_image_clicked / on_avatar_clicked already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), which
        // uses this window's own Deps.grab_surface_focus
        // (gtk_widget_grab_focus) in place of the direct call this window
        // used to make.

        // on_video_clicked (both room_view_'s and room_media_view_'s
        // gallery-reuse alias) already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_, which aliases
        // room_media_view()->on_image_clicked/on_video_clicked to the same
        // handlers it installs on room_view()).

        // The room-list search field is wired in
        // ShellBase::wire_main_app_widget_() (shared across all four shells).

        main_app_surface_->set_root(std::move(main_app_owner));
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

    // The settings surface is constructed lazily — see open_settings_().

    // Escape key: close viewer overlays. Attached to the window so it fires
    // regardless of which widget holds focus.
    //
    // Also carries the Ctrl-release detection that commits the Ctrl+Tab MRU
    // room switcher — same "attached to window_" reasoning as Escape above,
    // and GTK4's key-released signal is exactly the release-side counterpart
    // GtkShortcutController can't provide (it only ever fires on key-down/
    // repeat, which is why the Ctrl+Tab/Ctrl+Shift+Tab shortcuts below use
    // that mechanism for advancing the cycle but not for committing it).
    {
        GtkEventController* key_ctl = gtk_event_controller_key_new();
        g_signal_connect(key_ctl, "key-pressed",
                         G_CALLBACK(on_window_key_pressed_), this);
        g_signal_connect(key_ctl, "key-released",
                         G_CALLBACK(on_window_key_released_), this);
        gtk_widget_add_controller(window_, key_ctl);
    }

    // Global shortcuts (Ctrl+K, Ctrl+F, Ctrl+Tab, Alt+Left, Ctrl+/, ...),
    // bound from the shared registry. A global-scope GtkShortcutController
    // fires even while a native entry / text view holds focus (the
    // bubble-phase key controller above would let the focused widget
    // swallow them first). Capture phase, so these also win over the focused
    // text widget's own key bindings (GtkText/GtkTextView bind Ctrl+/ to
    // select-all), the way Qt's application shortcuts do. Each forwards the chord's KeyEvent into
    // the widget tree, and MainAppWidget decides what it does. Ctrl+Tab's
    // commit-on-Ctrl-release is handled by key_ctl's key-released signal
    // above — GtkShortcutController only ever fires on key-down/repeat.
    {
        GtkEventController* sc = gtk_shortcut_controller_new();
        gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(sc),
                                          GTK_SHORTCUT_SCOPE_GLOBAL);
        gtk_event_controller_set_propagation_phase(sc, GTK_PHASE_CAPTURE);
        for (const auto& def : tesseract::views::shortcuts())
        {
            if (def.scope == tesseract::views::ShortcutScope::Contextual)
                continue;
            for (const auto& chord : def.chords)
            {
                GtkShortcutTrigger* trigger = gtk_trigger_from_chord_(chord);
                if (!trigger)
                    continue;
                auto* binding = new ShortcutBinding_{this, tk::to_key_event(chord)};
                GtkShortcut* shortcut = gtk_shortcut_new(
                    trigger,
                    gtk_callback_action_new(
                        on_registry_shortcut_, binding,
                        [](gpointer data) { delete static_cast<ShortcutBinding_*>(data); }));
                gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                                     shortcut);
            }
        }
        gtk_widget_add_controller(window_, sc);
    }

    // Status bar floats below the main stack (outside the stack so it is
    // always visible regardless of which page is shown).
    status_bar_ = gtk_label_new("");
    refresh_sync_status_(); // "Not logged in" until a session restores
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
        status_row_ = status_row;
        low_power_label_ = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(low_power_label_), 14);
        gtk_widget_set_margin_end(low_power_label_, 4);
        gtk_widget_set_margin_bottom(low_power_label_, 2);
        gtk_widget_set_tooltip_text(
            low_power_label_, tk::tr("Low power mode — background sync paused").c_str());
        gtk_widget_set_visible(low_power_label_, FALSE);
        refresh_low_power_icon_();
        gtk_box_append(GTK_BOX(status_row), status_bar_);
        gtk_box_append(GTK_BOX(status_row), low_power_label_);
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
        on_low_power_mode_ui_(low_power_active());
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
                                    self->notify_window_active_(active);
                                    if (active)
                                        self->start_anim_tick_if_needed_();
                                }),
                     this);

    // 30 s periodic tick — granular enough for a 5 min idle threshold without
    // burning CPU. Returns G_SOURCE_CONTINUE to auto-reschedule; the source
    // id is stashed so ~MainWindow can g_source_remove() it (matching the
    // existing tk_anim_tick_id_ pattern).
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
    read_portal_accent_color_();

    apply_current_theme_();

    // Re-apply when the OS dark-mode setting changes (System mode only).
    // Store the handler ID so after_theme_applied_ can block it while writing
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

namespace
{

class GtkScreenshotHost final : public tesseract::screenshot::ScreenshotHost
{
public:
    explicit GtkScreenshotHost(MainWindow& window) : window_(window) {}

    void apply_theme(tesseract::screenshot::ScreenshotTheme theme) override
    {
        window_.apply_screenshot_theme_(
            theme == tesseract::screenshot::ScreenshotTheme::Dark);
    }
    void refresh() override { window_.refresh_for_screenshot_(); }
    bool save_png(const std::string& filename) override
    {
        return window_.save_screenshot_(filename.c_str());
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        g_timeout_add(
            static_cast<guint>(ms),
            +[](gpointer data) -> gboolean
            {
                std::unique_ptr<std::function<void()>> step(
                    static_cast<std::function<void()>*>(data));
                (*step)();
                return G_SOURCE_REMOVE;
            },
            new std::function<void()>(std::move(fn)));
    }
    void finish(bool ok) override { window_.finish_screenshots_(ok); }

private:
    MainWindow& window_;
};

} // namespace

void MainWindow::apply_screenshot_theme_(bool dark)
{
    apply_theme_ui_(dark ? tk::Theme::dark() : tk::Theme::light());
}

void MainWindow::refresh_for_screenshot_()
{
    main_app_surface_->relayout();
}

void MainWindow::finish_screenshots_(bool ok)
{
    // g_application_run's status can't be set from here; a failed capture
    // must still fail the CI step.
    if (!ok)
        std::exit(EXIT_FAILURE);
    g_application_quit(G_APPLICATION(app_));
}

void MainWindow::start_screenshot_mode()
{
    if (!seed_screenshot_fixture_(main_app_surface_->factory()))
    {
        g_printerr("Could not load screenshot assets\n");
        finish_screenshots_(false);
        return;
    }
    // show_rooms() keeps GTK's spaces-last ordering.
    show_rooms(rooms_);
    refresh_user_strip_();
    show_main_content_();
    gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 768);

    std::error_code ec;
    std::filesystem::create_directories(screenshot_dir_, ec);
    if (ec)
    {
        g_printerr("Could not create screenshot directory: %s\n",
                   screenshot_dir_.string().c_str());
        finish_screenshots_(false);
        return;
    }

    gtk_window_present(GTK_WINDOW(window_));
    screenshot_host_ = std::make_unique<GtkScreenshotHost>(*this);
    start_screenshot_director_(*screenshot_host_, "gtk4");
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
        [this](std::string token) { tray_toggle_(token); },
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
        // tray_ didn't exist yet when the account switch that led here last
        // broadcast a rebuild, so its per-account submenu is still empty.
        broadcast_rebuild_tray_();
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
    // Only the window that owns the app-wide tray icon consults CloseAction —
    // spawned per-account windows have no tray_ and always close for real
    // below. HideToTray/Minimize are intercepted here (returning TRUE); Quit
    // falls through so GTK destroys the window. Hiding is only reachable when
    // the tray icon actually exists, so CloseAction::HideToTray can never
    // strand the app unreachable.
    if (self->tray_ && self->tray_->is_available())
    {
        switch (tesseract::Settings::instance().close_action)
        {
        case tesseract::Settings::CloseAction::HideToTray:
            gtk_widget_set_visible(self->window_, FALSE);
            self->update_video_playback_suspension_();
            return TRUE; // stop default destruction
        case tesseract::Settings::CloseAction::Minimize:
            gtk_window_minimize(GTK_WINDOW(self->window_));
            // gtk_window_minimize changes neither visibility nor focus, so
            // nothing else drives this — call it explicitly as the hide path
            // above does.
            self->update_video_playback_suspension_();
            return TRUE;
        case tesseract::Settings::CloseAction::Quit:
            break;
        }
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
    // GTK init time, or if the app itself wrote it in after_theme_applied_.
    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    return prefer_dark ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
}

namespace
{
// Asks the portal for one org.freedesktop.appearance key without blocking: a
// portal that is still starting can hold the call for seconds, and the change
// signal only fires on a later change, so a bounded sync read could never be
// corrected. `done` runs on the main loop with the unwrapped value, or nullptr
// when the call failed (no portal, unknown key).
template <typename F>
void portal_read_async(GDBusConnection* bus, const char* key, F&& done)
{
    using Closure = std::decay_t<F>;
    auto* c = new Closure(std::forward<F>(done));
    // ReadOne (portal interface v2+), not the deprecated Read: Read
    // double-wraps its return value in an extra D-Bus variant layer.
    g_dbus_connection_call(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "ReadOne",
        g_variant_new("(ss)", "org.freedesktop.appearance", key),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer p)
        {
            auto* closure = static_cast<Closure*>(p);
            GVariant* reply = g_dbus_connection_call_finish(
                G_DBUS_CONNECTION(source), res, nullptr);
            GVariant* value = nullptr;
            if (reply)
            {
                g_variant_get(reply, "(v)", &value);
            }
            (*closure)(value);
            if (value)
            {
                g_variant_unref(value);
            }
            if (reply)
            {
                g_variant_unref(reply);
            }
            delete closure;
        },
        c);
}
} // namespace

void MainWindow::read_portal_color_scheme_()
{
    if (!portal_bus_)
    {
        return;
    }
    portal_read_async(portal_bus_, "color-scheme",
                      guarded([this](GVariant* value)
                              { apply_portal_color_scheme_(value); }));
}

// The portal reports 0 = no preference, 1 = dark, 2 = light. Applies the new
// OS theme when it differs from the one on screen and the user follows the OS.
void MainWindow::apply_portal_color_scheme_(GVariant* value)
{
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
    {
        return;
    }
    portal_color_scheme_ = g_variant_get_uint32(value);
    if (tesseract::Settings::instance().theme_pref ==
            tesseract::Settings::ThemePreference::System &&
        os_color_scheme_() != current_theme_.mode)
    {
        apply_current_theme_();
    }
}

std::optional<tk::Color> MainWindow::os_accent_color_() const
{
    if (!portal_accent_valid_)
    {
        return std::nullopt; // no libadwaita fallback — System renders as Blue.
    }
    return portal_accent_;
}

namespace
{
// Unpacks the (ddd) sRGB struct org.freedesktop.appearance's "accent-color"
// portal key returns. Components are in [0,1]; out-of-range signals "unset".
// Guards the variant's type first: an older/different portal implementation
// could hand back something other than (ddd), and g_variant_get would abort
// the process on a type mismatch.
bool parse_portal_accent_(GVariant* value, tk::Color& out)
{
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE("(ddd)")))
    {
        return false;
    }
    gdouble r = -1.0, g = -1.0, b = -1.0;
    g_variant_get(value, "(ddd)", &r, &g, &b);
    if (r < 0.0 || r > 1.0 || g < 0.0 || g > 1.0 || b < 0.0 || b > 1.0)
    {
        return false;
    }
    out = tk::Color::rgba(static_cast<std::uint8_t>(std::lround(r * 255.0)),
                          static_cast<std::uint8_t>(std::lround(g * 255.0)),
                          static_cast<std::uint8_t>(std::lround(b * 255.0)), 255);
    return true;
}
} // namespace

void MainWindow::read_portal_accent_color_()
{
    if (!portal_bus_)
    {
        return;
    }
    portal_read_async(portal_bus_, "accent-color",
                      guarded([this](GVariant* value)
                              { apply_portal_accent_(value); }));
}

void MainWindow::apply_portal_accent_(GVariant* value)
{
    if (!value)
    {
        return;
    }
    portal_accent_valid_ = parse_portal_accent_(value, portal_accent_);
    on_system_accent_changed_();
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
    if (std::string(ns) != "org.freedesktop.appearance")
    {
        if (value)
            g_variant_unref(value);
        return;
    }
    if (std::string(key) == "color-scheme")
    {
        self->apply_portal_color_scheme_(value);
    }
    else if (std::string(key) == "accent-color")
    {
        self->apply_portal_accent_(value);
    }
    if (value)
        g_variant_unref(value);
}

void MainWindow::after_theme_applied_(const tk::Theme& t)
{
    refresh_low_power_icon_(); // re-tint the status-bar glyph

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

void MainWindow::for_each_shell_surface_(
    const std::function<void(tk::SurfaceBase&)>& fn)
{
    if (branding_surface_)
        fn(*branding_surface_);
    if (main_app_surface_)
        fn(*main_app_surface_);
}

MainWindow::~MainWindow()
{
    release_settings_surface_();
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

    // Popup surfaces anchor to main_app_surface_: they go first.
    release_popups_();

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
    // This captures the window in a raw pointer, not a weak guard, so it must
    // not outlive it (M85).
    if (tk_inflight_tick_id_)
    {
        g_source_remove(tk_inflight_tick_id_);
        tk_inflight_tick_id_ = 0;
    }
    // The dark-theme handler is on the global GtkSettings, which outlives this
    // window, and it was connected with this window as its data.
    if (prefer_dark_notify_id_)
    {
        g_signal_handler_disconnect(gtk_settings_get_default(), prefer_dark_notify_id_);
        prefer_dark_notify_id_ = 0;
    }
    // Send this window's deferred "New messages" moves before the clients
    // stop (request_stop() would cancel them).
    flush_pending_fully_read_now_();
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
        // Hang up an active call while the SDK can still send the leave. Once
        // request_stop() and stop_sync() run, the call would only end when the
        // session is destroyed, and the others would still see us in it.
        end_call(true);
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
    media_prefetch_pool_.drain();
    // The login page is bound to pending_login_client_: release it before
    // pending_login_client_ and the accounts vector.
    release_login_surface_();
    pending_login_client_.reset();
}

// ---------------------------------------------------------------------------

void MainWindow::close_app_settings_ui_()
{
    if (!settings_surface_)
        return;
    stop_search_index_stats_poll_();
    leave_settings_();
}

void MainWindow::rebind_login_client_ui_()
{
    if (!login_view_)
        return;
    login_view_->set_client(reset_pending_login_client_());
    // Swap only. Settings is open on top of the login view, and reset() would
    // refocus its homeserver field mid-typing; the homeserver check is re-run
    // by leaving Settings (take_login_recheck_()).
}

void MainWindow::leave_settings_()
{
    // Opened from the login screen: go back to it (the view is still alive
    // in the stack) instead of the main content, which would tear it down.
    if (take_settings_from_login_() && login_view_)
    {
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
        // A proxy change made in Settings replaced the client: re-run the
        // homeserver check against it.
        if (take_login_recheck_())
            login_view_->reset();
    }
    else
        show_main_content_();
    set_app_settings_open_(false);
    teardown_settings_view_();
}

void MainWindow::show_main_content_()
{
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    teardown_login_view_();
    mark_main_content_ready_();
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
            show_login_after_restore_(restore, [this] { do_login(); });
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

void MainWindow::activate_for_notification_(const std::string& token)
{
    // Set xdg_activation_v1 token (non-empty on modern Wayland)
    // before gtk_window_present so the compositor grants focus.
    if (!token.empty())
    {
        gtk_window_set_startup_id(GTK_WINDOW(window_), token.c_str());
    }
}

void MainWindow::install_account_notifier_(tesseract::AccountSession& session)
{
    // Per-account notifier: click switches to this account then navigates.
    const std::string notif_uid = session.user_id;
    session.notifier = std::make_unique<LinuxNotifierGtk>(
        [this, notif_uid](std::string room_id, std::string event_id,
                          std::string token)
        {
            on_notification_clicked_(notif_uid, std::move(room_id),
                                     std::move(event_id), std::move(token));
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

tesseract::AuxWindowBase* MainWindow::create_aux_window_(std::string title,
                                                         std::unique_ptr<tk::Widget> root,
                                                         int width, int height)
{
    return new gtk4::AuxWindow(this, title, std::move(root), width, height, current_theme_);
}

bool MainWindow::has_save_file_dialog_() const
{
    return true;
}

namespace
{
// One-shot context for a GtkFileDialog result.
struct PathCtx
{
    std::function<void(std::string)> cb;
};

void deliver_file_result(GFile* file, PathCtx* c)
{
    if (file)
    {
        char* path = g_file_get_path(file);
        if (path)
        {
            c->cb(std::string(path));
            g_free(path);
        }
        g_object_unref(file);
    }
    delete c;
}
} // namespace

void MainWindow::prompt_passphrase_(std::string title,
                                    std::function<void(std::string)> cb)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        title.c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
        tk::tr("_Cancel").c_str(), GTK_RESPONSE_CANCEL, tk::tr("_OK").c_str(),
        GTK_RESPONSE_OK, nullptr);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    G_GNUC_END_IGNORE_DEPRECATIONS
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), tk::tr("Passphrase").c_str());
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
                const char* text = gtk_editable_get_text(GTK_EDITABLE(c->entry));
                if (text && text[0] != '\0')
                    c->cb(std::string(text));
            }
            delete c;
            gtk_window_destroy(GTK_WINDOW(d));
        }),
        ctx);
}

void MainWindow::pick_save_file_(std::string title, std::string suggested_name,
                                 std::function<void(std::string)> cb)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, title.c_str());
    gtk_file_dialog_set_initial_name(dlg, suggested_name.c_str());
    gtk_file_dialog_save(dlg, GTK_WINDOW(window_), nullptr,
        +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
        {
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_save_finish(
                GTK_FILE_DIALOG(dialog_obj), res, &err);
            if (err) g_error_free(err);
            deliver_file_result(file, static_cast<PathCtx*>(data));
        },
        new PathCtx{std::move(cb)});
    g_object_unref(dlg);
}

void MainWindow::pick_open_file_(std::string title,
                                 std::function<void(std::string)> cb)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, title.c_str());
    gtk_file_dialog_open(dlg, GTK_WINDOW(window_), nullptr,
        +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
        {
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_open_finish(
                GTK_FILE_DIALOG(dialog_obj), res, &err);
            if (err) g_error_free(err);
            deliver_file_result(file, static_cast<PathCtx*>(data));
        },
        new PathCtx{std::move(cb)});
    g_object_unref(dlg);
}

void MainWindow::pick_folder_(std::string title,
                              std::function<void(std::string)> cb)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, title.c_str());
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(window_), nullptr,
        +[](GObject* dialog_obj, GAsyncResult* res, gpointer data)
        {
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_select_folder_finish(
                GTK_FILE_DIALOG(dialog_obj), res, &err);
            if (err) g_error_free(err);
            deliver_file_result(file, static_cast<PathCtx*>(data));
        },
        new PathCtx{std::move(cb)});
    g_object_unref(dlg);
}

void MainWindow::show_message_dialog_(std::string title, std::string text,
                                      bool is_error)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(window_), GTK_DIALOG_MODAL,
        is_error ? GTK_MESSAGE_ERROR : GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
        text.c_str());
    gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_signal_connect(dlg, "response",
                     G_CALLBACK(+[](GtkDialog* d, int, gpointer)
                     { gtk_window_destroy(GTK_WINDOW(d)); }),
                     nullptr);
    gtk_window_present(GTK_WINDOW(dlg));
}

void MainWindow::apply_window_title_ui_(const std::string& title)
{
    if (!window_)
        return;
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
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

void MainWindow::on_space_children_cache_ready_ui_()
{
    refresh_room_list_();
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refresh_room_list_();
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
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
    if (!tk::decode_size_allowed(bytes, /*animated=*/false))
    {
        return nullptr;
    }
    if (bytes.empty())
    {
        return nullptr;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    // Same cap as canvas_cairo.cpp decode_image: formats the header probe
    // doesn't know are scaled down to the still budget.
    g_signal_connect(loader, "size-prepared",
                     G_CALLBACK(+[](GdkPixbufLoader* l, int w, int h, gpointer)
                                {
                                    const std::uint64_t px = std::uint64_t(std::max(w, 0)) *
                                                             std::uint64_t(std::max(h, 0));
                                    if (px <= tk::kMaxStillDecodePixels)
                                        return;
                                    const double s =
                                        std::sqrt(double(tk::kMaxStillDecodePixels) / double(px));
                                    gdk_pixbuf_loader_set_size(l, std::max(1, int(w * s)),
                                                               std::max(1, int(h * s)));
                                }),
                     nullptr);
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
// tk::kAnimDecodeMaxFrames to keep runaway / never-ending GIFs from blowing
// memory. Most animated stickers ship ≤ 30 frames.
//
// `max_w`/`max_h` (0/0 = unbounded) downscale each frame (aspect-ratio-
// preserving, independent axis limits — cairo-native cairo_scale+cairo_paint,
// no GdkPixbuf round trip) to fit within that box as it's produced —
// GdkPixbufAnimationIter always returns one full-native-size composited
// pixbuf per frame (no seam to request a smaller decode), so this is the
// earliest point a frame can be shrunk; it's discarded again before the next
// iteration, so peak memory is one native-res pixbuf (owned by the iterator)
// + one scaled surface at a time, not N native-res frames.
//
// `on_first_frame`/`on_extra_frame` (both null by default) stream frames out
// as they're produced instead of collecting them into the returned struct:
// when both are supplied, whichever frame is the first to actually decode
// (and, if applicable, downscale) successfully goes to `on_first_frame`
// regardless of its original position in the source, and every frame after
// that to `on_extra_frame` (with a running delivery index) — so a bad-but-
// recoverable first frame doesn't leave a later one silently paired with
// on_extra_frame despite on_first_frame never firing. The returned struct's
// `frames`/`delays_ms` stay empty in this mode — check the return value
// itself (present vs nullopt) for success, not whether frames is non-empty.
struct DecodedAnimation
{
    std::vector<cairo_surface_t*> frames; // caller owns each
    std::vector<int> delays_ms;
};

// Downscale `surf` (aspect-ratio-preserving) to fit within max_w x max_h if
// it's larger; a no-op if it already fits or max_w/max_h <= 0. Takes
// ownership of `surf` either way (destroys it once no longer needed) and
// returns the surface to actually use — nullptr on a scale failure (rare:
// cairo status failure), matching a decode failure to the caller. Shared by
// decode_animation's own emit() and GtkAnimSession::decode_next_batch()
// below.
cairo_surface_t* scale_cairo_surface_to_fit(cairo_surface_t* surf, int max_w,
                                            int max_h)
{
    if (max_w <= 0 || max_h <= 0)
    {
        return surf;
    }
    const int w = cairo_image_surface_get_width(surf);
    const int h = cairo_image_surface_get_height(surf);
    if (w <= max_w && h <= max_h)
    {
        return surf;
    }
    const float scale = std::min(static_cast<float>(max_w) / w,
                                 static_cast<float>(max_h) / h);
    const int tw = std::max(1, static_cast<int>(w * scale));
    const int th = std::max(1, static_cast<int>(h * scale));
    cairo_surface_t* dst =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(dst);
        cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_t* cr = cairo_create(dst);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surf, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(dst);
    cairo_surface_destroy(surf);
    return dst;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
std::optional<DecodedAnimation> decode_animation(
    const std::vector<uint8_t>& bytes, int max_w = 0, int max_h = 0,
    const std::function<void(cairo_surface_t*, int)>* on_first_frame = nullptr,
    const std::function<void(int, cairo_surface_t*, int)>* on_extra_frame =
        nullptr)
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
    bool any_frame = false;
    const bool streaming = on_first_frame && on_extra_frame;
    bool first_emitted = false;
    int extra_index = 0;
    // Downscale (aspect-ratio-preserving, cairo-native — see the doc
    // comment above for why this beats the GdkPixbuf round trip an earlier
    // version used) then hand `surf` (already owned by the caller) to
    // on_first_frame/on_extra_frame if streaming, else accumulate into
    // `out` as before. `any_frame` only becomes true once a frame actually
    // survives (decode + downscale), never before — so a decode/downscale
    // failure on every frame correctly leaves `out` empty AND `any_frame`
    // false, matching this function's own nullopt-on-failure contract.
    auto emit = [&](cairo_surface_t* surf, int delay)
    {
        surf = scale_cairo_surface_to_fit(surf, max_w, max_h);
        if (!surf)
        {
            return;
        }
        any_frame = true;
        if (streaming)
        {
            if (!first_emitted)
            {
                first_emitted = true;
                (*on_first_frame)(surf, delay);
            }
            else
            {
                (*on_extra_frame)(extra_index++, surf, delay);
            }
            return;
        }
        out.frames.push_back(surf);
        out.delays_ms.push_back(delay);
    };
    for (int i = 0; i < tk::kAnimDecodeMaxFrames; ++i)
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
            emit(surf, 100); // arbitrary tail-hold
            break;
        }
        delay = tk::normalize_frame_delay_ms(delay);
        emit(surf, delay);

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
    if (!any_frame)
    {
        return std::nullopt;
    }
    return out;
}

// tk::AnimDecodeSession for GTK4: GdkPixbufAnimationIter is a strictly
// forward-only cursor (mirrors Qt6's QImageReader and, for GIF, even
// Windows' WIC decoder — see those sessions' identical comments), so this
// keeps loader_/anim_/iter_ alive across decode_next_batch() calls rather
// than reopening per call. restart() is the one place a full rebuild is
// unavoidable: it tears down and reopens a fresh GdkPixbufLoader from
// owned_bytes_ (a small, retained copy of the *compressed* source — not a
// second copy of decoded frame data) rather than trying to rewind iter_,
// which gdk-pixbuf's animation API has no way to do.
//
// Also unlike Qt6 (QImageReader::imageCount()) or Windows (WIC
// GetFrameCount()), gdk-pixbuf's animation API exposes no "how many frames
// total" query — the only way to know is to iterate until advance() says
// there's no more. So total_frames is always reported as 0 ("unknown") to
// AnimImageCache; see decode_animation_windowed's doc comment for why
// that's fine.
class GtkAnimSession final : public tk::AnimDecodeSession
{
public:
    // Takes ownership of `loader`/`anim` (already loader_write+close'd and
    // confirmed a non-static-image animation by the caller) for the
    // session's first iterator. `owned_bytes` (a copy of the original
    // compressed source) is kept only for restart()'s later rebuild.
    GtkAnimSession(GdkPixbufLoader* loader, GdkPixbufAnimation* anim,
                   std::shared_ptr<const std::vector<std::uint8_t>> owned_bytes,
                   int max_w, int max_h)
        : owned_bytes_(std::move(owned_bytes)), loader_(loader), anim_(anim),
          max_w_(max_w), max_h_(max_h)
    {
        open_iter_();
    }

    ~GtkAnimSession() override { close_(); }

    std::size_t memory_bytes() const override
    {
        return owned_bytes_ ? owned_bytes_->size() : 0;
    }

    int decode_next_batch(
        int n,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>&
            on_frame) override
    {
        int produced = 0;
        for (int attempted = 0;
             attempted < n && !exhausted_ &&
             cursor_ < tk::kAnimDecodeMaxFrames;
             ++attempted)
        {
            if (!iter_)
            {
                exhausted_ = true;
                break;
            }
            GdkPixbuf* pb = gdk_pixbuf_animation_iter_get_pixbuf(iter_);
            if (!pb)
            {
                exhausted_ = true;
                break;
            }
            cairo_surface_t* surf = pixbuf_to_premultiplied_argb32(pb);
            if (!surf)
            {
                exhausted_ = true;
                break;
            }
            int delay = gdk_pixbuf_animation_iter_get_delay_time(iter_);
            // -1 means there's no upcoming frame (last frame of a
            // non-looping animation) — capture it and stop, same as
            // decode_animation's identical check.
            bool tail = false;
            if (delay < 0)
            {
                delay = 100;
                tail = true;
            }
            else
            {
                delay = tk::normalize_frame_delay_ms(delay);
            }
            surf = scale_cairo_surface_to_fit(surf, max_w_, max_h_);
            if (!surf)
            {
                exhausted_ = true;
                break;
            }
            const int idx = cursor_++;
            on_frame(idx, tk::cairo_pango::make_image(surf), delay);
            cairo_surface_destroy(surf);
            ++produced;
            if (tail)
            {
                exhausted_ = true;
                break;
            }
            clock_.tv_usec += delay * 1000;
            while (clock_.tv_usec >= G_USEC_PER_SEC)
            {
                clock_.tv_sec += 1;
                clock_.tv_usec -= G_USEC_PER_SEC;
            }
            if (!gdk_pixbuf_animation_iter_advance(iter_, &clock_))
            {
                // Iterator decided no new frame would be shown — same stop
                // condition decode_animation's loop uses.
                exhausted_ = true;
                break;
            }
        }
        if (cursor_ >= tk::kAnimDecodeMaxFrames)
        {
            exhausted_ = true;
        }
        return produced;
    }

    bool exhausted() const override { return exhausted_; }

    void restart() override
    {
        close_();
        // A write/close failure here is an extremely unlikely transient
        // glitch (the bytes are already retained in memory, not re-read
        // from disk/network) — worth one immediate retry before permanently
        // freezing this entry's playback (see try_open_loader_'s doc
        // comment).
        if (!try_open_loader_() && !try_open_loader_())
        {
            loader_ = nullptr;
            anim_ = nullptr;
            cursor_ = 0;
            exhausted_ = true;
            return;
        }
        cursor_ = 0;
        exhausted_ = false;
        open_iter_();
    }

private:
    // Rebuilds loader_/anim_ from owned_bytes_. Returns false (loader_ left
    // null) on failure, leaving the caller free to retry once before giving
    // up — see restart()'s doc comment.
    bool try_open_loader_()
    {
        loader_ = gdk_pixbuf_loader_new();
        GError* err = nullptr;
        if (!gdk_pixbuf_loader_write(loader_, owned_bytes_->data(),
                                     owned_bytes_->size(), &err) ||
            !gdk_pixbuf_loader_close(loader_, &err))
        {
            if (err)
            {
                g_error_free(err);
            }
            g_object_unref(loader_);
            loader_ = nullptr;
            return false;
        }
        anim_ = gdk_pixbuf_loader_get_animation(loader_);
        return true;
    }

    void open_iter_()
    {
        clock_ = GTimeVal{0, 0};
        iter_ = anim_ ? gdk_pixbuf_animation_get_iter(anim_, &clock_) : nullptr;
        if (!iter_)
        {
            exhausted_ = true;
        }
    }

    void close_()
    {
        if (iter_)
        {
            g_object_unref(iter_);
            iter_ = nullptr;
        }
        if (loader_)
        {
            g_object_unref(loader_);
            loader_ = nullptr;
        }
        anim_ = nullptr; // borrowed from loader_ — gone once loader_ is
    }

    std::shared_ptr<const std::vector<std::uint8_t>> owned_bytes_;
    GdkPixbufLoader* loader_ = nullptr;
    GdkPixbufAnimation* anim_ = nullptr; // borrowed from loader_
    GdkPixbufAnimationIter* iter_ = nullptr;
    GTimeVal clock_{0, 0};
    int max_w_, max_h_;
    int cursor_ = 0;
    bool exhausted_ = false;
};

// Windowed variant of decode_animation's streaming mode. Unlike Qt6/Windows,
// there's no frame-count-based "short enough, don't bother windowing"
// threshold: gdk-pixbuf's animation API can't report a total frame count
// without fully iterating it (see GtkAnimSession's doc comment), so every
// animated GTK4 image goes through the session/windowed path uniformly —
// which is fine, since AnimImageCache never trims/restarts a session that
// exhausts before its resident window would need to shrink, so a short
// animation behaves identically to the old fully-resident approach anyway,
// just wrapped in the (here, cheap) session machinery.
bool decode_animation_windowed(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>&
        on_extra_frame)
{
    if (bytes.empty())
    {
        return false;
    }

    auto owned_bytes = std::make_shared<std::vector<std::uint8_t>>(
        bytes.begin(), bytes.end());

    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, owned_bytes->data(),
                                 owned_bytes->size(), &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return false;
    }
    if (!gdk_pixbuf_loader_close(loader, &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return false;
    }
    GdkPixbufAnimation* anim = gdk_pixbuf_loader_get_animation(loader);
    if (!anim || gdk_pixbuf_animation_is_static_image(anim))
    {
        g_object_unref(loader);
        return false;
    }

    // Ownership of loader/anim transfers into the session from here.
    auto session = std::make_shared<GtkAnimSession>(loader, anim, owned_bytes,
                                                     max_w, max_h);
    bool got_first = false;
    session->decode_next_batch(
        tk::kAnimDecodeInitialBatchFrames,
        [&](int idx, std::unique_ptr<tk::Image> img, int delay_ms)
        {
            if (!got_first)
            {
                got_first = true;
                on_first_frame(std::move(img), delay_ms, session, 0);
            }
            else
            {
                on_extra_frame(idx, std::move(img), delay_ms);
            }
        });
    return got_first;
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
    if (self->inflight_tick_())
        return G_SOURCE_CONTINUE;
    // The source is going away: forget its id, so a later start re-arms it.
    self->tk_inflight_tick_id_ = 0;
    return G_SOURCE_REMOVE;
}

void MainWindow::start_inflight_tick_()
{
    if (!tk_inflight_tick_id_)
        tk_inflight_tick_id_ = g_timeout_add(16, on_tk_inflight_tick_, this);
}

void MainWindow::stop_inflight_tick_()
{
    // Remove the source, not just forget its id: a tick left queued would fire
    // on this window after it is freed (M85).
    if (tk_inflight_tick_id_)
        g_source_remove(tk_inflight_tick_id_);
    tk_inflight_tick_id_ = 0;
}

void MainWindow::repaint_inflight_spinner_()
{
    if (inflight_dot_)
        gtk_widget_queue_draw(inflight_dot_);
}

void MainWindow::update_main_anim_regions_()
{
    // GTK4 has no partial-widget invalidation, but each animating image gets
    // its own small overlay widget (see host_gtk.cpp's live_overlays_), and
    // update_anim_regions() queues a redraw of just those overlays.
    if (main_app_surface_)
    {
        main_app_surface_->update_anim_regions();
    }
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
    main_app_->room_list_view()->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
    {
        main_app_->room_list_view()->set_selected_room(current_room_id_);
    }
    main_app_surface_->relayout();
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

std::unique_ptr<tk::Image>
MainWindow::decode_avatar_(const std::vector<std::uint8_t>& bytes)
{
    // Stills only: skips the animation probe the generic decode_image_ does.
    cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
    if (!surface)
    {
        return nullptr;
    }
    auto img = tk::cairo_pango::make_image(surface);
    cairo_surface_destroy(surface);
    return img;
}

void MainWindow::ingest_tile_media_(const tk::CacheKey& cache_key,
                                    std::vector<std::uint8_t> bytes)
{
    run_async_(
        [this, cache_key, bytes = std::move(bytes)]()
        {
            cairo_surface_t* surface = decode_image_to_cairo_surface(bytes);
            if (!surface)
            {
                return;
            }
            post_to_ui_alive_(
                [this, cache_key, surface]()
                {
                    if (account_manager_.image_cache().contains(cache_key))
                    {
                        cairo_surface_destroy(surface);
                        return;
                    }
                    auto img = tk::cairo_pango::make_image(surface);
                    cairo_surface_destroy(surface);
                    account_manager_.image_cache().store(cache_key, std::move(img));
                    schedule_relayout_();
                    repaint_settings_ui_();
                    notify_secondary_media_ready_(cache_key.id, MediaKind::Tile);
                });
        });
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, tk::tr("Select image").c_str());
    GtkFileFilter* filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, tk::tr("Images").c_str());
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
                // The file is chosen by the user but read here on the UI thread,
                // so refuse anything too large to be a still worth sending
                // before reading it. Same limit as the Qt picker.
                constexpr goffset kMaxPickedImageBytes = 64LL * 1024 * 1024;
                GFileInfo* info = g_file_query_info(
                    file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                    G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
                const goffset size = info ? g_file_info_get_size(info) : 0;
                if (info)
                    g_object_unref(info);

                gsize len = 0;
                char* raw = nullptr;
                GError* load_err = nullptr;
                if (size > kMaxPickedImageBytes)
                    c->self->show_status_message_(tk::tr("That image is too large to send."));
                else
                    g_file_load_contents(file, nullptr, &raw, &len, nullptr, &load_err);
                if (!load_err && raw && len > 0)
                {
                    std::vector<uint8_t> bytes(raw, raw + len);
                    // The type comes from the content, not the extension: a
                    // mislabelled file would otherwise go out with the wrong MIME type.
                    const std::string_view sniffed = tk::sniff_image_mime(bytes);
                    if (sniffed.empty())
                    {
                        c->self->show_status_message_(
                            tk::tr("That file isn't a PNG, JPEG, GIF or WebP image."));
                    }
                    else
                    {
                        const std::string mime(sniffed);
                        auto callback = std::move(c->cb);
                        c->self->post_to_ui_(
                            [callback = std::move(callback),
                             bytes = std::move(bytes), mime]() mutable
                            { callback(std::move(bytes), mime); });
                    }
                }
                g_free(raw);
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
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                          int max_h)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_image(bytes, max_w, max_h, tk::cairo_pango::make_image_rgba);
    // Untrusted bytes: refuse over-budget images before any codec allocates (tk/image_sniff.h).
    if (!tk::decode_size_allowed(bytes, tk::bytes_may_be_animated(bytes)))
        return DecodedImage{};
    // decode_image_to_cairo_surface / decode_animation are in this
    // file's anonymous namespace and are thread-safe (GdkPixbuf + cairo).
    // tk::cairo_pango::make_image refcounts the surface (thread-safe).
    DecodedImage d;
    if (auto anim = decode_animation(bytes, max_w, max_h))
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

bool MainWindow::decode_image_streamed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed(bytes, max_w, max_h, tk::cairo_pango::make_image_rgba,
                                               on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    bool got_first = false;
    std::function<void(cairo_surface_t*, int)> first_cb =
        [&](cairo_surface_t* surf, int delay_ms)
    {
        got_first = true;
        on_first_frame(tk::cairo_pango::make_image(surf), delay_ms);
        cairo_surface_destroy(surf);
    };
    std::function<void(int, cairo_surface_t*, int)> extra_cb =
        [&](int idx, cairo_surface_t* surf, int delay_ms)
    {
        on_frame(idx, tk::cairo_pango::make_image(surf), delay_ms);
        cairo_surface_destroy(surf);
    };
    decode_animation(bytes, max_w, max_h, &first_cb, &extra_cb);
    return got_first;
}

bool MainWindow::decode_image_streamed_windowed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed_windowed(bytes, max_w, max_h, tk::cairo_pango::make_image_rgba,
                                                        on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    return decode_animation_windowed(bytes, max_w, max_h, on_first_frame,
                                     on_frame);
}

std::int64_t MainWindow::monotonic_ms_()
{
    return g_get_monotonic_time() / 1000;
}

void MainWindow::start_anim_tick_()
{
    start_anim_tick_if_needed_();
}

void MainWindow::probe_drop_video_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
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

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::probe_drop_audio_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
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

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::decode_video_first_frame_(
    std::vector<std::uint8_t> bytes,
    std::function<void(std::vector<std::uint8_t>)> done)
{
    // Runs the GStreamer decode + PNG-encode pipeline against `bytes`
    // off-thread and invokes `done(png)` on the UI thread (empty png on any
    // failure).
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
}

void MainWindow::cache_rgba_image_(const tk::CacheKey& key, int w, int h,
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

bool MainWindow::is_main_window_focused_() const
{
    return window_ && gtk_window_is_active(GTK_WINDOW(window_));
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (main_app_)
    {
        main_app_->room_list_view()->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::push_room_list_state(tesseract::RoomListState state)
{
    push_room_list_state_(state);
    refresh_sync_status_();
}

void MainWindow::show_sync_status_text_(const std::string& text)
{
    if (status_bar_)
    {
        gtk_label_set_text(GTK_LABEL(status_bar_), text.c_str());
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

std::unique_ptr<tk::SurfaceBase> MainWindow::create_login_surface_ui_()
{
    auto surface = std::make_unique<tk::gtk4::Surface>(current_theme_);
    gtk_stack_add_named(GTK_STACK(content_stack_), surface->widget(), "login");
    return surface;
}

void MainWindow::unmount_login_surface_ui_()
{
    if (login_surface_)
        gtk_stack_remove(GTK_STACK(content_stack_), login_native_()->widget());
}

void MainWindow::show_login_page_ui_()
{
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_settings_surface_ui_()
{
    auto surface = std::make_unique<tk::gtk4::Surface>(current_theme_);
    GtkWidget* w = surface->widget();
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_vexpand(w, TRUE);
    gtk_stack_add_named(GTK_STACK(content_stack_), w, "settings");
    return surface;
}

void MainWindow::unmount_settings_surface_ui_()
{
    if (settings_surface_)
        gtk_stack_remove(GTK_STACK(content_stack_), settings_native_()->widget());
}

void MainWindow::settings_view_created_ui_(tesseract::views::SettingsView& view)
{
    // GTK's native GtkEntry needs compact mode for a snug visual fit inside
    // these rows; every other backend uses the default chrome.
    if (auto* f = view.name_field())
        f->set_compact(true);
    if (auto* pe = view.pronouns_editor())
        pe->set_compact(true);
    if (auto* f = view.tz_field())
        f->set_compact(true);
    if (auto* f = view.bio_field())
        f->set_compact(true);
}

void MainWindow::grab_main_surface_focus_()
{
    if (main_app_surface_)
        gtk_widget_grab_focus(main_app_surface_->widget());
}

tk::SurfaceBase* MainWindow::main_surface_()
{
    return main_app_surface_.get();
}

void MainWindow::update_settings_anim_regions_()
{
    // The settings page reports no animated-image damage, so repaint it whole
    // to advance its animations.
    if (app_settings_open_ && settings_surface_)
        settings_surface_->host().request_repaint();
}

void MainWindow::repaint_settings_ui_()
{
    if (app_settings_open_ && settings_surface_)
        gtk_widget_queue_draw(settings_native_()->widget());
}

void MainWindow::open_settings_()
{
    if (!prepare_settings_view_())
        return;

    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "settings");
    set_app_settings_open_(true);
    start_search_index_stats_poll_();
}

void MainWindow::do_logout()
{
    logout_active_account_ui_();
}

// ---------------------------------------------------------------------------
// Shortcode popup — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::ShortcodePopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::dismiss_popups_on_resize_()
{
    if (main_app_surface_) main_app_surface_->host().dismiss_active_popup();
    if (room_view_) room_view_->dismiss_popups();
    hide_compose_popups_();
    close_account_picker_();
}

// ── Slash-command popup ────────────────────────────────────────────────────

tesseract::ShellBase::DecodedImage MainWindow::decode_inline_fallback_(
    const std::vector<std::uint8_t>& bytes, int /*max_w*/, int /*max_h*/)
{
    // Stills only: the windowed decoder already declined this image, so the
    // animation probe in decode_image_ would just repeat that decode.
    DecodedImage d;
    if (cairo_surface_t* surf = decode_image_to_cairo_surface(bytes))
    {
        d.still = tk::cairo_pango::make_image(surf);
        cairo_surface_destroy(surf);
    }
    return d;
}

std::unique_ptr<tk::Image> MainWindow::make_image_from_bgra_(
    int w, int h, const std::uint8_t* bgra)
{
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
    {
        if (surf)
        {
            cairo_surface_destroy(surf);
        }
        return nullptr;
    }
    unsigned char* dst = cairo_image_surface_get_data(surf);
    const int dst_stride = cairo_image_surface_get_stride(surf);
    const int src_stride = w * 4;
    for (int y = 0; y < h; ++y)
    {
        std::memcpy(dst + y * dst_stride, bgra + y * src_stride,
                    std::min(src_stride, dst_stride));
    }
    cairo_surface_mark_dirty(surf);
    auto img = tk::cairo_pango::make_image(surf);
    cairo_surface_destroy(surf);
    return img;
}

// ── @mention popup ─────────────────────────────────────────────────────────

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
    g_menu_append(menu, tk::tr("Add to Saved Stickers").c_str(), "sticker.save");

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
    g_menu_append(menu,
                  (room_view_ ? room_view_->copy_menu_label()
                              : tk::tr("Copy")).c_str(),
                  "copy-sel.copy");

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
        self->room_view_->copy_context_menu_target();
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
        const auto& strip = self->main_app_->user_info()->bounds();
        if (x >= strip.x && x < strip.right() && y >= strip.y &&
            y < strip.bottom())
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

// A registry chord as a GTK trigger; null for keys not bound here.
GtkShortcutTrigger* MainWindow::gtk_trigger_from_chord_(const tk::KeyChord& chord)
{
    guint keyval = 0;
    unsigned mods = 0;
    if (chord.mods & (tk::ModPrimary | tk::ModCtrl))
        mods |= GDK_CONTROL_MASK;
    if (chord.mods & tk::ModAlt)
        mods |= GDK_ALT_MASK;
    if (chord.mods & tk::ModMeta)
        mods |= GDK_META_MASK;
    // Shift+Tab is its own keyval (below), so it carries no Shift bit.
    if ((chord.mods & tk::ModShift) && chord.key != tk::Key::Tab)
        mods |= GDK_SHIFT_MASK;
    switch (chord.key)
    {
    case tk::Key::Character:
        // GTK normalizes a shifted key event to the unshifted lowercase
        // keyval before matching, so letter triggers use the lowercase keyval.
        if (chord.text.size() == 1)
            keyval = gdk_unicode_to_keyval(static_cast<guint32>(
                std::tolower(static_cast<unsigned char>(chord.text.front()))));
        break;
    case tk::Key::Tab:
        // GTK reports Shift+Tab as the distinct keyval GDK_KEY_ISO_Left_Tab
        // (as host_gtk.cpp's key_from_gdk does), not Tab plus the shift bit.
        keyval = (chord.mods & tk::ModShift) ? GDK_KEY_ISO_Left_Tab : GDK_KEY_Tab;
        break;
    case tk::Key::Left: keyval = GDK_KEY_Left; break;
    case tk::Key::Right: keyval = GDK_KEY_Right; break;
    case tk::Key::F1: keyval = GDK_KEY_F1; break;
    default: break;
    }
    if (keyval == 0 || keyval == GDK_KEY_VoidSymbol)
        return nullptr;
    return gtk_keyval_trigger_new(keyval, static_cast<GdkModifierType>(mods));
}

gboolean MainWindow::on_registry_shortcut_(GtkWidget*, GVariant*, gpointer user_data)
{
    auto* binding = static_cast<ShortcutBinding_*>(user_data);
    if (binding->self->main_app_)
        binding->self->main_app_->dispatch_key_down(binding->event);
    return TRUE;
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
    if (keyval == GDK_KEY_Escape && self->handle_escape_key_())
        return TRUE;
    return FALSE;
}

void MainWindow::on_window_key_released_(GtkEventControllerKey*, guint keyval,
                                         guint, GdkModifierType,
                                         gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)
    {
        if (self->main_app_)
        {
            self->main_app_->commit_mru_cycle();
        }
    }
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
        self->show_status_message_(res.message, 6000);
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

std::vector<std::uint8_t> MainWindow::encode_selfie_jpeg_(
    const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
{
    // GdkPixbuf expects RGBA; swap R and B channels.
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = bgra[i + 2]; // R
        rgba[i + 1] = bgra[i + 1]; // G
        rgba[i + 2] = bgra[i + 0]; // B
        rgba[i + 3] = bgra[i + 3]; // A
    }
    std::vector<std::uint8_t> jpeg;
    GdkPixbuf* pb = gdk_pixbuf_new_from_data(
        rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8, static_cast<int>(w),
        static_cast<int>(h), static_cast<int>(w * 4), nullptr, nullptr);
    if (!pb)
        return jpeg;
    GError* gerr = nullptr;
    gchar* data = nullptr;
    gsize sz = 0;
    if (gdk_pixbuf_save_to_buffer(pb, &data, &sz, "jpeg", &gerr, "quality", "90",
                                  nullptr))
    {
        jpeg.assign(reinterpret_cast<const std::uint8_t*>(data),
                    reinterpret_cast<const std::uint8_t*>(data) + sz);
        g_free(data);
    }
    if (gerr)
        g_error_free(gerr);
    g_object_unref(pb);
    return jpeg;
}

bool MainWindow::main_window_foreground_for_tray_() const
{
    return gtk_widget_get_visible(window_) &&
           gtk_window_is_active(GTK_WINDOW(window_));
}

void MainWindow::hide_main_window_for_tray_()
{
    gtk_widget_set_visible(window_, FALSE);
    update_video_playback_suspension_();
}

void MainWindow::show_main_window_for_tray_(const std::string& token)
{
    // Feed the compositor-granted xdg-activation token (from the tray host's
    // ProvideXdgActivationToken call) to the next present, so Wayland
    // focus-stealing prevention lets the window come forward — same mechanism
    // as the notification-click path.
    if (!token.empty())
        gtk_window_set_startup_id(GTK_WINDOW(window_), token.c_str());
    gtk_window_present(GTK_WINDOW(window_));
    update_video_playback_suspension_();
}

void MainWindow::start_platform_services_ui_()
{
    start_tray_if_needed_();
    start_search_provider_if_needed_();
    start_mpris_if_needed_();
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────


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

void MainWindow::set_window_fullscreen_(bool on)
{
    if (!window_)
        return;
    // Drop the status strip too — it looks wrong along the bottom of
    // full-screen media.
    if (status_row_)
        gtk_widget_set_visible(status_row_, !on);
    if (on)
        gtk_window_fullscreen(GTK_WINDOW(window_));
    else
        gtk_window_unfullscreen(GTK_WINDOW(window_));
}

bool MainWindow::spawn_relaunch_(const std::vector<std::string>& args)
{
    // Inside an AppImage the running binary lives on a mount that goes away
    // with this process; relaunch the .AppImage itself.
    std::string program;
    if (const char* appimage = g_getenv("APPIMAGE"); appimage && *appimage)
    {
        program = appimage;
    }
    else if (gchar* self = g_file_read_link("/proc/self/exe", nullptr))
    {
        program = self;
        g_free(self);
    }
    if (program.empty())
        return false;

    std::vector<gchar*> argv;
    argv.push_back(const_cast<gchar*>(program.c_str()));
    for (const auto& a : args)
        argv.push_back(const_cast<gchar*>(a.c_str()));
    argv.push_back(nullptr);

    GError* error = nullptr;
    if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr,
                       nullptr, nullptr, &error))
    {
        g_warning("relaunch failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return false;
    }
    return true;
}

void MainWindow::quit_app_()
{
    tray_.reset();
    g_application_quit(G_APPLICATION(app_));
}

void MainWindow::rebuild_tray_()
{
    if (!tray_ || !tray_->is_available())
        return;

    auto items = build_tray_items_();
    tray_->rebuild_menu(std::move(items));
}

bool MainWindow::click_modifier_held_() const
{
    // tk pointer events don't carry modifiers, so the GTK host captures Ctrl
    // from the click gesture itself.
    return main_app_surface_ && main_app_surface_->host().pointer_ctrl_held();
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
