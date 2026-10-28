#pragma once
#include <QMainWindow>
class QMoveEvent;
#include <QHash>
#include <QLabel>
#include <QPixmap>
#include <QStackedWidget>
#include <QVariant>
#include <QStatusBar>
#include <QTimer>
#include <QDBusVariant>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>

#include "app/AccountManager.h"
#include "app/EventHandlerBase.h"
#include "views/settings/SettingsController.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "tk/i18n.h"
#include "LinuxQtTrayIcon.h"
#include "QtKRunnerPlugin.h"
#include "QtMprisPlayer.h"
#include "views/AccountPicker.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/MentionController.h"
#include "views/media_viewer_items.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "InflightDotWidget.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QSocketNotifier>
#include "platform/single_instance.h"

class QTimer;

namespace qt6
{


/// Qt file-dialog filter string for a media save: "<Group> (*.a *.b);;All files (*.*)",
/// or just the all-files entry when the spec has no specific group.
inline QString save_filter_for(const tesseract::views::MediaSaveSpec& spec)
{
    const QString all = QString::fromStdString(tk::tr("All files")) + " (*.*)";
    if (spec.filter_name.empty())
    {
        return all;
    }
    QStringList patterns;
    for (const auto& p : spec.patterns)
    {
        patterns << QString::fromStdString(p);
    }
    return QString::fromStdString(spec.filter_name) + " (" + patterns.join(' ') +
           ");;" + all;
}

/// Thin QObject wrapper around EventHandlerBase so Qt can own it as a
/// child and the SDK can pass it as an IEventHandler*. All marshalling
/// is handled by EventHandlerBase::post_to_ui_ (QMetaObject::invokeMethod
/// queued) — no signals needed here.
class EventBridge final : public QObject, public tesseract::EventHandlerBase
{
    Q_OBJECT
public:
    explicit EventBridge(tesseract::ShellBase* shell, QObject* parent = nullptr)
        : QObject(parent), tesseract::EventHandlerBase(shell)
    {
    }
};

// ---------------------------------------------------------------------------

class MainWindow final : public QMainWindow, public tesseract::ShellBase
{
    Q_OBJECT
public:
    // start_hidden: true when launched via the OS autostart mechanism
    // (--autostart). The window is not shown until doLogin()'s async
    // restore completes: if a saved session silently restores, it stays
    // hidden (tray-only); if there's nothing to restore (or it fails), the
    // no-accounts branch force-shows it so the user can log in.
    explicit MainWindow(tesseract::AccountManager& account_manager,
                        QWidget* parent = nullptr, bool start_hidden = false
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                        , bool screenshot_mode = false
#endif
                        );
    ~MainWindow() override;

    /// Call once after show() to bring the window to the foreground on launch.
    /// Reads XDG_ACTIVATION_TOKEN from the environment (set by Wayland-aware
    /// launchers) and uses it; falls back to activateWindow() on X11 or when
    /// no token is available.
    void activateOnStartup();
    void openMatrixLink(const std::string& uri) { open_matrix_link(uri); }
    /// Command-line launch action (see ShellBase::dispatch_launch_action_).
    void dispatchLaunchAction(tesseract::LaunchAction action, std::string room_id)
    {
        dispatch_launch_action_(action, std::move(room_id));
    }

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    /// Seed the deterministic, network-free fixture, capture both themes, and
    /// exit the Qt event loop. Only present in screenshot-mode builds.
    void captureScreenshots(const std::string& output_dir);
    // ScreenshotHost adapter hooks (see QtScreenshotHost in MainWindow.cpp).
    void apply_screenshot_theme_(bool dark);
    void refresh_for_screenshot_();
#endif

    bool is_main_window_visible_() const override
    {
        return isVisible() && !isMinimized();
    }
    bool is_main_window_focused_() const override { return isActiveWindow(); }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void changeEvent(QEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;
    std::vector<tk::Rect> get_screen_work_areas_() const override;

private slots:
    void onUserStripContextMenu(const QPoint& pos);
    void do_quit_();
    /// Frame-tick driver for animated inline media in the timeline.
    /// Advances frames in `anim_cache_` and repaints `msgSurface_`
    /// when at least one frame changes.
    void onMessageAnimTick_();
    void onInflightTick_();

    void on_portal_setting_changed_(const QString& ns, const QString& key,
                                    const QDBusVariant& value);

signals:

private:
    void doLogin();
    void doLogout();
    void openSettings();
    // Switches to mainAppSurface_ and tears down the login view if it exists —
    // the single choke point for "login (or whatever else) is no longer
    // showing, main app is". ShellBase::ensure_login_view_ creates the login
    // page on first need (through create_login_surface_ui_) and
    // teardown_login_view_ destroys it, rather than keeping its native
    // controls and widget tree alive for the whole session. See
    // ui/shared/app/DeferredTeardown.h for why teardown is deferred rather
    // than synchronous.
    void show_main_content_() override;
    // Close the settings page: back to the login view if it was opened from
    // there, otherwise to the main content.
    void leaveSettings_();
    void setupActivationListener_();

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    bool screenshot_mode_ = false;
    std::unique_ptr<tesseract::screenshot::ScreenshotHost> screenshot_host_;
#endif

    // Forward room picker — opened by "Forward message" action.
    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;

    // ── EventHandlerBase UI-thread hook overrides (Qt6) ──────────────────────
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_low_power_mode_ui_(bool active) override;
    void refreshLowPowerIcon_();
    bool tray_available_() const override;
    void open_app_settings_ui_() override { openSettings(); }
    void close_app_settings_ui_() override;
    void rebind_login_client_ui_() override;
    void raise_main_window_ui_() override;
    void bring_main_window_forward_for_overlay_() override;
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    void on_startup_restore_progress_ui_(const std::string& status) override;

    // ---- Multi-account orchestration ----

    std::unique_ptr<tk::SurfaceBase> create_login_surface_ui_() override;
    void show_login_page_ui_() override;
    bool open_login_url_ui_(const std::string& url) override;
    QWidget* login_native_() const
    {
        return static_cast<tk::qt6::Surface*>(login_surface_.get());
    }
    void start_platform_services_ui_() override;

    void navigate_to_room(const std::string& room_id);
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    // Push ShellBase::compose_window_title_()'s string to the OS window title.
    void apply_window_title_ui_(const std::string& title) override;
    /// Kick off a back-pagination worker thread for `room_id`. Early-exit
    /// if a pagination is already in flight for this room or its history
    /// has been fully fetched. Hooked to `RoomView::on_near_top`.
    // ShellBase virtual hooks (Qt6 implementations).
    void apply_native_theme_(const tk::Theme& t) override;
    void after_theme_applied_(const tk::Theme& t) override;
    void for_each_shell_surface_(
        const std::function<void(tk::SurfaceBase&)>& fn) override;
    tk::ThemeMode os_color_scheme_() const override;
    std::optional<tk::Color> os_accent_color_() const override;

    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void repaint_pickers_native_() override;
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
    bool has_save_file_dialog_() const override { return true; }
    void pick_media_save_path_(const tesseract::views::MediaSaveSpec& spec,
                               std::function<void(std::string)> cb) override;
    void pick_save_file_(std::string title, std::string suggested_name,
                         std::function<void(std::string)> cb) override;
    void prompt_passphrase_(std::string title,
                            std::function<void(std::string)> cb) override;
    void pick_open_file_(std::string title,
                         std::function<void(std::string)> cb) override;
    void pick_folder_(std::string title,
                      std::function<void(std::string)> cb) override;
    void show_message_dialog_(std::string title, std::string text,
                              bool is_error) override;
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

    // Platform probe called by ShellBase::extract_drop_media_ for video/audio.
    // When `target` is non-null the result is posted to that compose bar (a
    // pop-out window's), guarded by `target_alive`; otherwise it goes to the
    // main window's compose bar.
    // Creates a QMediaPlayer on the UI thread (Qt Multimedia constraint),
    // extracts the first frame as a JPEG thumbnail and the media duration,
    // then posts via post_pending_attachment_.
    void probe_drop_video_(std::uint32_t pending_gen,
                           std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> target_alive) override;

    // Creates a QMediaPlayer on the UI thread, waits for LoadedMedia status to
    // read the duration, then posts via post_pending_attachment_.
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
    void start_search_provider_if_needed_();
    void start_mpris_if_needed_();
    bool is_ctrl_held_() const override;
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


    /// Shutdown coordination. `~MainWindow` flips this flag, clears the
    /// pool of queued runnables, and waits (bounded) for in-flight
    /// workers to drain before calling `client_.stop_sync()`. Without
    /// this, a worker mid-`client_.fetch_*` racing against `~ClientFfi`
    /// is a data race on `&mut self` in Rust that surfaces as a
    /// `panic_in_cleanup` abort through cxx's `prevent_unwind` guard.

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;
    static constexpr int kAvatarCacheSize = tesseract::visual::kAvatarCacheSize;
    static constexpr int kMsgMaxWidth = tesseract::visual::kMsgMaxWidth;

    // Single surface hosting the full main-app widget tree (sidebar + chat
    // panel + lightbox overlays).
    tk::qt6::Surface* mainAppSurface_ = nullptr;

    bool explicitly_quitting_ = false;  // set before quit actions to bypass hide-to-tray in closeEvent
    // Restore to maximized (not plain windowed) when leaving media-viewer
    // full-screen if that's how the window was before.
    bool fs_was_maximized_ = false;
    // True when constructed with start_hidden=true (--autostart) and no
    // saved session has yet forced the window visible. See doLogin().
    bool start_hidden_ = false;

    InflightDotWidget* inflightDot_ = nullptr;
    QLabel* lowPowerLabel_ = nullptr;
    // Rich-text label for status messages carrying hyperlinks (see
    // app/status_links.h). Created lazily; hidden while plain messages
    // use statusBar()->showMessage().
    QLabel* statusLinkLabel_ = nullptr;
    void show_sync_status_text_(const std::string& text) override;
    void on_sync_status_override_kept_() override;

    // Holds an xdg-activation token to be consumed by the next
    // navigate_to_room() call. Set by notification/second-instance handlers
    // before they call navigate_to_room so the compositor grants focus.
    QString pending_wayland_token_;

    /// Raise the window and activate it. On Wayland, submits `token` via
    /// xdg_activation_v1_activate() directly (bypassing Qt). Falls back to
    /// activateWindow() on X11 or when xdg-activation-v1 is unavailable.
    void activateWindowWithToken_(const QString& token);

    bool main_window_foreground_for_tray_() const override;
    std::vector<std::uint8_t> encode_selfie_jpeg_(const std::uint8_t* bgra,
                                                  std::uint32_t w,
                                                  std::uint32_t h) override;
    std::unique_ptr<tk::SurfaceBase> create_settings_surface_ui_() override;
    tk::SurfaceBase* main_surface_() override;
    void grab_main_surface_focus_() override;
    // The settings page as the stack's widget.
    QWidget* settings_native_() const
    {
        return static_cast<tk::qt6::Surface*>(settings_surface_.get());
    }
    void hide_main_window_for_tray_() override;
    void show_main_window_for_tray_(const std::string& token) override;

    // Listens for activation requests forwarded by a later launch of either
    // Linux backend (Qt/GTK share the same lock + socket protocol — see
    // ui/shared/platform/single_instance.h) and raises this window.
    std::unique_ptr<tk::ActivationListener> activationListener_;
    QSocketNotifier* activationNotifier_ = nullptr;

    QStackedWidget* contentStack_ = nullptr;
    tk::qt6::Surface* brandingSurface_ = nullptr;
    tesseract::views::BrandView* brandingView_ = nullptr; // borrowed, owned by brandingSurface_

    // ---- Multi-account state ----
    //
    // Every signed-in account lives in `account_manager_` (inherited from
    // ShellBase) as its own `AccountSession` (its own `tesseract::Client`,
    // its own `EventBridge`). All accounts start sync at restore/login time
    // and keep syncing in the background so notifications fire regardless of
    // which one is foreground. UI surfaces (room list / message list / compose)
    // are bound only to `active_account_`; handle_*_ui_() methods filter by
    // user_id to ignore traffic from inactive accounts (their `RoomInfo`
    // snapshots are cached in `per_account_rooms_` so a fast
    // switch_active_account_ doesn't have to wait for the next push).
    // NOTE: account_manager_, active_account_, per_account_rooms_,
    // pending_login_client_, pending_login_temp_dir_, add_account_return_uid_,
    // pending_login_is_add_account_, client_ are all inherited from ShellBase.
    // handle_*_ui_() callbacks identify the originating account via the
    // user_id parameter passed by EventHandlerBase — no bridge_ alias needed.

    std::unique_ptr<LinuxQtTrayIcon> tray_;
    std::unique_ptr<QtKRunnerPlugin> krunner_;
    std::unique_ptr<QtMprisPlayer> mpris_;
    // rooms_, current_room_id_, pending_restore_room_, my_user_id_,
    // my_display_name_, my_avatar_url_, tk_avatars_, tk_images_,
    // voice_prefetched_, video_thumb_in_flight_, reply_details_requested_,
    // anim_cache_, space_stack_, pagination_, kPaginationBatch are all
    // inherited from ShellBase.

    // Animated inline-media (GIF / WebP / APNG). `tk_anim_timer_` fires at
    // ~60 Hz and calls anim_cache_.advance(); a true return triggers repaint.
    QTimer* tk_anim_timer_ = nullptr;
    QTimer* tk_inflight_timer_ = nullptr;
    QTimer* presence_tick_timer_ = nullptr;

    // ── GIF picker (/gif <query>) ────────────────────────────────────────────
    // Decoded first-frame previews for the strip, keyed by Tenor preview URL.
    std::unique_ptr<tk::Image> make_image_from_bgra_(
        int w, int h, const std::uint8_t* bgra) override;
    bool make_image_from_bgra_on_worker_() const override { return true; }

    // Asynchronous portal reads; each answer re-applies the theme if it changed.
    void read_portal_setting_async_(
        const QString& key, std::function<void(const QVariant&)> apply);
    void read_portal_color_scheme_();
    void read_portal_accent_color_();
    void apply_portal_color_scheme_(int scheme);
    void apply_portal_accent_(const QVariant& value);

    // Cached org.freedesktop.appearance color-scheme portal value.
    // -1 = not yet read, 0 = no preference, 1 = dark, 2 = light.
    int portal_color_scheme_ = -1;

    // Cached org.freedesktop.appearance accent-color portal value
    // (portal_accent_valid_ == false -> portal reported none, fall back to
    // QPalette::Highlight in os_accent_color_()).
    bool      portal_accent_valid_ = false;
    tk::Color portal_accent_{};
};

} // namespace qt6
