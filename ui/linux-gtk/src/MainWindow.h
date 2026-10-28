#pragma once
#include <gtk/gtk.h>
#include <gio/gio.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>
#include "GtkSniTrayIcon.h"
#include "GtkSearchProviderGtk.h"
#include "GtkMprisPlayer.h"

#include "app/AccountManager.h"
#include "app/EventHandlerBase.h"
#include "views/settings/SettingsController.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "tk/key_chord.h"
#include "views/AccountPicker.h"
#include "views/BrandView.h"
#include "views/ComposePopups.h"
#include "views/MainAppWidget.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"

#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gtk4
{


// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase
{
public:
    // start_hidden: true when launched via the OS autostart mechanism
    // (--autostart). The window is not shown until do_login()'s async
    // restore completes: if a saved session silently restores, it stays
    // hidden (tray-only); if there's nothing to restore (or it fails), the
    // no-accounts branch force-shows it so the user can log in.
    explicit MainWindow(tesseract::AccountManager& account_manager,
                        GtkApplication* app, bool start_hidden = false
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                        , std::filesystem::path screenshot_dir = {}
#endif
                        );
    ~MainWindow();

    GtkWidget* widget() const
    {
        return window_;
    }
    // The GtkApplication this shell belongs to. Pop-out windows associate
    // themselves with it (gtk_window_set_application) so they are proper
    // application windows — without this a bare gtk_window_new() pop-out routes
    // popover keyboard grabs incorrectly (composer input dies while a popup is
    // open).
    GtkApplication* application() const
    {
        return app_;
    }
    void present()
    {
        gtk_window_present(GTK_WINDOW(window_));
    }

    /// Command-line launch action (see ShellBase::dispatch_launch_action_).
    void dispatch_launch_action(tesseract::LaunchAction action, std::string room_id)
    {
        dispatch_launch_action_(action, std::move(room_id));
    }

    // Bring the main window forward and open the quick switcher. Pop-out room
    // windows route here on Ctrl+K — their own shortcut controller is scoped
    // to the pop-out window, while the switcher widget lives in the main one.
    void request_quick_switch_from_popout()
    {
        present();
        open_quick_switch_();
    }

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    void start_screenshot_mode();
    // ScreenshotHost adapter hooks (see GtkScreenshotHost in MainWindow.cpp).
    void apply_screenshot_theme_(bool dark);
    void refresh_for_screenshot_();
    bool save_screenshot_(const char* filename);
    void finish_screenshots_(bool ok);
#endif

private:
    // ── EventHandlerBase UI-thread hook overrides (GTK4) ──────────────────────
    // timeline-reset + message insert/update/remove are now concrete in
    // ShellBase (they drive room_view_ + request_relayout_ directly).
    bool is_main_window_visible_() const override;
    bool is_main_window_focused_() const override;
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_low_power_mode_ui_(bool active) override;
    void refresh_low_power_icon_();
    bool tray_available_() const override;
    void open_app_settings_ui_() override { open_settings_(); }
    void close_app_settings_ui_() override;
    void rebind_login_client_ui_() override;
    void raise_main_window_ui_() override { present(); }
    void draw_inflight_dot_(cairo_t* cr);
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    void on_startup_restore_progress_ui_(const std::string& status) override;
    std::vector<tk::Rect> get_screen_work_areas_() const override;

    // user_id identifies which account's snapshot this is (for caching).
    void push_rooms(std::string user_id,
                    std::vector<tesseract::RoomInfo> rooms);
    void push_room_list_state(tesseract::RoomListState state);
    static void on_login_clicked(GtkButton*, gpointer user_data);
    void build_sticker_context_menu();

private:
    static void on_msg_right_click_(GtkGestureClick* gesture, int n_press,
                                    double x, double y, gpointer user_data);
    static void on_sticker_save_activate_(GSimpleAction* action,
                                          GVariant* parameter,
                                          gpointer user_data);
    static void on_copy_action_(GSimpleAction* action, GVariant* parameter,
                                gpointer user_data);
    void open_settings_();
    // Switches to the "main" stack page and tears down the login view if it
    // exists — the single choke point for "login (or whatever else) is no
    // longer showing, main app is". ShellBase::ensure_login_view_ creates the
    // login page on first need (through create_login_surface_ui_) and
    // teardown_login_view_ destroys it, rather than keeping its native
    // controls and widget tree alive for the whole session. See
    // ui/shared/app/DeferredTeardown.h for why teardown is deferred rather
    // than synchronous.
    void show_main_content_() override;
    // Close the settings page: back to the login view if it was opened from
    // there, otherwise to the main content.
    void leave_settings_();

    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;

    static gboolean on_window_key_pressed_(GtkEventControllerKey*, guint keyval,
                                           guint, GdkModifierType,
                                           gpointer user_data);
    // Commits the Ctrl+Tab MRU room switcher on Ctrl release — see the
    // key_ctl setup in the ctor for why this is attached to window_.
    static void on_window_key_released_(GtkEventControllerKey*, guint keyval,
                                        guint, GdkModifierType,
                                        gpointer user_data);
    // Global shortcuts bound from views/shortcut_registry.h: each native
    // binding carries the KeyEvent it forwards to main_app_->
    // dispatch_key_down (owned by its GtkCallbackAction, freed with it).
    struct ShortcutBinding_
    {
        MainWindow* self;
        tk::KeyEvent event;
    };
    static GtkShortcutTrigger* gtk_trigger_from_chord_(const tk::KeyChord& chord);
    static gboolean on_registry_shortcut_(GtkWidget*, GVariant*, gpointer user_data);
    static gboolean on_window_close_request_(GtkWindow* window,
                                             gpointer user_data);

    void start_tray_if_needed_();
    void start_search_provider_if_needed_();
    void start_mpris_if_needed_();

    // Multi-account management.
    std::unique_ptr<tk::SurfaceBase> create_login_surface_ui_() override;
    void unmount_login_surface_ui_() override;
    void show_login_page_ui_() override;
    tk::gtk4::Surface* login_native_() const
    {
        return static_cast<tk::gtk4::Surface*>(login_surface_.get());
    }
    void start_platform_services_ui_() override;
    bool main_window_foreground_for_tray_() const override;
    std::vector<std::uint8_t> encode_selfie_jpeg_(const std::uint8_t* bgra,
                                                  std::uint32_t w,
                                                  std::uint32_t h) override;
    void repaint_settings_ui_() override;
    void update_settings_anim_regions_() override;
    std::unique_ptr<tk::SurfaceBase> create_settings_surface_ui_() override;
    void unmount_settings_surface_ui_() override;
    void settings_view_created_ui_(tesseract::views::SettingsView& view) override;
    tk::SurfaceBase* main_surface_() override;
    void grab_main_surface_focus_() override;
    tk::gtk4::Surface* settings_native_() const
    {
        return static_cast<tk::gtk4::Surface*>(settings_surface_.get());
    }
    void hide_main_window_for_tray_() override;
    void show_main_window_for_tray_(const std::string& token) override;

    void show_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    // Push ShellBase::compose_window_title_()'s string to the OS window title.
    void apply_window_title_ui_(const std::string& title) override;
    void ensure_row_media(const tesseract::Event& ev);
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();
    void do_logout();
    bool has_save_file_dialog_() const override;
    void prompt_passphrase_(std::string title,
                            std::function<void(std::string)> cb) override;
    void pick_save_file_(std::string title, std::string suggested_name,
                         std::function<void(std::string)> cb) override;
    void pick_open_file_(std::string title,
                         std::function<void(std::string)> cb) override;
    void pick_folder_(std::string title,
                      std::function<void(std::string)> cb) override;
    void show_message_dialog_(std::string title, std::string text,
                              bool is_error) override;
    void navigate_to_room(const std::string& room_id);

    // ShellBase virtual hooks (GTK4 implementations).
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void after_theme_applied_(const tk::Theme& t) override;
    void for_each_shell_surface_(
        const std::function<void(tk::SurfaceBase&)>& fn) override;
    tk::ThemeMode os_color_scheme_() const override;
    std::optional<tk::Color> os_accent_color_() const override;
    // XDG Desktop Portal (org.freedesktop.appearance/color-scheme) — the
    // source of truth for the OS light/dark preference. GtkSettings'
    // gtk-application-prefer-dark-theme is app-controlled (we write it
    // ourselves in after_theme_applied_) and isn't kept in sync with the desktop
    // by plain GTK4 (only libadwaita apps get that), so it's kept only as a
    // last-resort fallback until the portal answers (or when it is unreachable).
    // The reads are asynchronous; the answer re-applies the theme.
    void read_portal_color_scheme_();
    void apply_portal_color_scheme_(GVariant* value);
    // org.freedesktop.appearance/accent-color — portal-only, no fallback
    // (there's no GTK property carrying an accent colour outside
    // libadwaita, which this build doesn't link). An accent-less desktop
    // keeps rendering AccentTheme::System identically to Blue.
    void read_portal_accent_color_();
    void apply_portal_accent_(GVariant* value);
    static void on_portal_setting_changed_(GDBusConnection*, const char*,
                                           const char*, const char*,
                                           const char*, GVariant* parameters,
                                           gpointer user_data);
    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void on_space_children_cache_ready_ui_() override;
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    std::unique_ptr<tk::Image>
    decode_avatar_(const std::vector<std::uint8_t>& bytes) override;
    void ingest_tile_media_(const tk::CacheKey& key,
                            std::vector<std::uint8_t> bytes) override;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    bool decode_image_streamed_(
        const std::vector<uint8_t>& bytes, int max_w, int max_h,
        const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
        override;
    bool decode_image_streamed_windowed_(
        const std::vector<uint8_t>& bytes, int max_w, int max_h,
        const std::function<void(std::unique_ptr<tk::Image>, int,
                                 std::shared_ptr<tk::AnimDecodeSession>,
                                 std::size_t)>& on_first_frame,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
        override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void update_main_anim_regions_() override;
    void start_inflight_tick_() override;
    void stop_inflight_tick_() override;
    void repaint_inflight_spinner_() override;

    // Room-selection hooks (ShellBase::on_room_selected_).

    // Tab management hooks.
    void decode_video_first_frame_(
        std::vector<std::uint8_t> bytes,
        std::function<void(std::vector<std::uint8_t>)> done) override;

    // Platform probe called by ShellBase::extract_drop_media_ for video/audio:
    // extracts media metadata on a background thread, then posts via
    // g_idle_add. When `target` is non-null the result goes to that compose bar
    // (a pop-out window's), guarded by `target_alive`; otherwise to the main
    // window's.
    void probe_drop_video_(std::uint32_t pending_gen,
                           std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> target_alive) override;
    void probe_drop_audio_(std::uint32_t pending_gen,
                           std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> target_alive) override;
    void cache_rgba_image_(const tk::CacheKey& key, int w, int h,
                           std::vector<uint8_t> rgba) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void activate_for_notification_(const std::string& token) override;
    void set_window_fullscreen_(bool on) override;
    void rebuild_tray_() override;
    bool spawn_relaunch_(const std::vector<std::string>& args) override;
    void quit_app_() override;
    bool is_ctrl_held_() const override;
    bool click_modifier_held_() const override;
    void spawn_main_window_(
        std::shared_ptr<tesseract::AccountSession> account) override;
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string& uid) override;
    void install_account_notifier_(
        tesseract::AccountSession& session) override;
    void install_account_up_connector_(
        tesseract::AccountSession& session) override;
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override;
    tesseract::CallWindowBase* create_call_window_() override;
    tesseract::AuxWindowBase* create_aux_window_(std::string title,
                                                 std::unique_ptr<tk::Widget> root,
                                                 int width, int height) override;

    void start_anim_tick_if_needed_();
    static gboolean on_tk_anim_tick_(gpointer user_data);
    static gboolean on_tk_inflight_tick_(gpointer user_data);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;

    GtkApplication* app_ = nullptr;
    // True when constructed with start_hidden=true (--autostart) and no
    // saved session has yet forced the window visible. See do_login().
    bool start_hidden_ = false;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    std::filesystem::path screenshot_dir_;
    std::unique_ptr<tesseract::screenshot::ScreenshotHost> screenshot_host_;
#endif
    GtkWidget* window_ = nullptr;
    GtkWidget* content_stack_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> branding_surface_;
    tesseract::views::BrandView* branding_view_ = nullptr; // borrowed, owned by branding_surface_

    // Single surface hosting the full main-app widget tree.
    // main_app_ / room_view_ live in ShellBase (assigned in the constructor).
    std::unique_ptr<tk::gtk4::Surface> main_app_surface_;

    // Closes any open popup (emoji/sticker/receipt pickers, the compose
    // autocomplete popups, the account picker, and any generic
    // Host-registered popup) — called from the notify::default-width/-height
    // handlers, GTK4's only per-window resize hook with app context. A
    // popup's screen position is captured once when it opens and never
    // recomputed, so leaving one open across a resize would strand it away
    // from whatever control anchored it.
    void dismiss_popups_on_resize_();

    // ── GIF picker (/gif <query>) ────────────────────────────────────────────
    std::unique_ptr<tk::Image> make_image_from_bgra_(
        int w, int h, const std::uint8_t* bgra) override;
    bool make_image_from_bgra_on_worker_() const override { return true; }
    DecodedImage decode_inline_fallback_(const std::vector<std::uint8_t>& bytes,
                                         int max_w, int max_h) override;

    GtkWidget* sticker_ctx_menu_ = nullptr;
    GSimpleActionGroup* sticker_ctx_actions_ = nullptr;
    std::string ctx_sticker_event_id_;

    GtkWidget* copy_ctx_menu_ = nullptr;
    GSimpleActionGroup* copy_ctx_actions_ = nullptr;
    void build_copy_context_menu_();
    std::string ctx_sticker_mxc_url_;
    std::string ctx_sticker_body_;
    std::string ctx_sticker_info_json_;
    GtkWidget* status_bar_ = nullptr;
    GtkWidget* status_row_ = nullptr; // status_bar_ + inflight dot; hidden in full-screen
    GtkWidget* inflight_dot_ = nullptr;
    GtkWidget* low_power_label_ = nullptr;

    void show_sync_status_text_(const std::string& text) override;

    GtkWidget*       user_popover_      = nullptr;

    GtkCssProvider* theme_css_provider_ = nullptr;
    gulong prefer_dark_notify_id_ = 0;

    GDBusConnection* portal_bus_ = nullptr;
    guint portal_setting_changed_sub_ = 0;
    // -1 = not yet read / portal unreachable, 0 = no preference,
    // 1 = prefer dark, 2 = prefer light (org.freedesktop.appearance values).
    int portal_color_scheme_ = -1;

    // Cached org.freedesktop.appearance accent-color portal value
    // (portal_accent_valid_ == false -> portal reported none).
    bool      portal_accent_valid_ = false;
    tk::Color portal_accent_{};

    std::unique_ptr<GtkSniTrayIcon> tray_;
    std::unique_ptr<GtkSearchProviderGtk> search_provider_;
    std::unique_ptr<GtkMprisPlayer> mpris_;

    guint tk_anim_tick_id_ = 0;
    guint tk_inflight_tick_id_ = 0;
    guint presence_tick_id_ = 0;

};

} // namespace gtk4
