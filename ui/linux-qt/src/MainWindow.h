#pragma once
#include <QMainWindow>
class QMoveEvent;
#include <QHash>
#include <QLabel>
#include <QPixmap>
#include <QStackedWidget>
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
#include "app/SettingsController.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "LinuxQtTrayIcon.h"
#include "views/AccountPicker.h"
#include "views/MainAppWidget.h"
#include "views/MentionController.h"
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

#include <QLocalServer>

class EmojiPicker;
class StickerPicker;
class JoinRoomDialog;
class QTimer;

namespace qt6
{

class LoginView;
class SettingsWidget;

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
    explicit MainWindow(tesseract::AccountManager& account_manager, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Call once after show() to bring the window to the foreground on launch.
    /// Reads XDG_ACTIVATION_TOKEN from the environment (set by Wayland-aware
    /// launchers) and uses it; falls back to activateWindow() on X11 or when
    /// no token is available.
    void activateOnStartup();
    void openMatrixLink(const std::string& uri) { open_matrix_link(uri); }

    bool is_main_window_visible_() const override
    {
        return isVisible() && !isMinimized();
    }

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
    void onLoginSucceeded();
    void onSendClicked();
    void onSpaceBack();
    void onUserStripContextMenu(const QPoint& pos);
    void do_quit_();
    void onLoginCancelled();
    void onAccountSelected(const std::string& user_id);
    void onPaginateFinished(QString roomId, bool reached_start);
    /// Frame-tick driver for animated inline media in the timeline.
    /// Advances frames in `anim_cache_` and repaints `msgSurface_`
    /// when at least one frame changes.
    void onMessageAnimTick_();
    void onInflightTick_();

    void on_portal_setting_changed_(const QString& ns, const QString& key,
                                    const QDBusVariant& value);
    void onActivateRequested();

signals:

private:
    void doLogin();
    // Bind the UI to the now-active account `uid` and finish startup
    // (settings controller, status, main surface, tray). Shared by the cold
    // restore path and the secondary-window bind path in doLogin().
    void finishLoginUi_(const std::string& uid);
    void doLogout();
    void openSettings();
    void setupLocalServer_();

    // Ctrl+K quick switcher — open focuses the native search field; close
    // hides it and relayouts.
    void openQuickSwitch_();
    void closeQuickSwitch_();

    // Ctrl+Shift+F message search — open focuses the native search field; close
    // hides it and relayouts.
    void openMessageSearch_();
    void closeMessageSearch_();

    // Forward room picker — opened by "Forward message" action.
    void closeForwardPicker_();
    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;

    // Ctrl+F per-room "find in conversation" search bar.
    void openFindInRoom_();
    void closeFindInRoom_();

    // ── EventHandlerBase UI-thread hook overrides (Qt6) ──────────────────────
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void refresh_user_strip_() override;
    void request_relogin_(const std::string& user_id) override;
    void
    handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void refresh_pickers_packs_() override;
    void handle_verification_request_ui_(std::string flow_id,
                                         std::string user_id,
                                         std::string device_id,
                                         bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(std::string flow_id,
                                           std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_notification_ui_(std::string user_id, std::string room_id,
                                 std::string room_name, std::string sender,
                                 std::string body, bool is_mention,
                                 std::vector<uint8_t> avatar_bytes,
                                 std::vector<uint8_t> image_bytes) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_server_info_ready_ui_() override;
    void on_own_extended_profile_ready_ui_() override;
    void on_profile_field_result_ui_(const std::string& key, bool ok,
                                     const std::string& error) override;
    void update_typing_bar_(const std::string& text, bool visible) override;
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    bool is_room_search_active_() const override
    {
        return !roomSearchPendingText_.empty();
    }

    // ---- Multi-account orchestration ----

    /// Detach the room/message/compose surfaces from the current active account
    /// (if any) and rebind them to the account identified by `user_id`. Single
    /// chokepoint for foreground swaps; called by both the picker and the
    /// post-login flow. Rewrites `accounts.json::active_user_id`.
    void switchActiveAccount(const std::string& user_id);

    /// Right-click → "Add Account…" path. Records the current active index
    /// in `add_account_return_idx_`, sets `loginView_` to
    /// `LoginView::Mode::AddAccount`, creates a fresh `pending_login_client_`
    /// scoped to its own data dir, swaps the LoginView in. Cancel restores
    /// the old active account; success pushes the new `AccountSession`.
    void beginAddAccount();

    /// Right-click → "Log Out <name>". Stops sync on the active account,
    /// clears its on-disk state, removes it from `accounts_`, rewrites
    /// `accounts.json`, and either switches to the next account or shows
    /// the LoginView in `Mode::Initial`.
    void logoutActiveAccount();

    /// Left-click on the avatar opens the AccountPicker popover. No-op
    /// when fewer than 2 accounts are signed in.
    void openAccountPicker(const QPoint& global_anchor);

    /// Refresh the `AccountPicker` row set from `account_manager_`. Called after
    /// add/logout/switch so the popover reflects current state next open.
    void rebuildAccountPicker();

    void navigate_to_room(const std::string& room_id);
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void populateUserStrip();
    void showRooms(const std::vector<tesseract::RoomInfo>& rooms);
    void refreshRoomList();
    void onRoomSelected(const std::string& room_id);
    void clearMessages();
    /// Kick off a back-pagination worker thread for `room_id`. Early-exit
    /// if a pagination is already in flight for this room or its history
    /// has been fully fetched. Hooked to `RoomView::on_near_top`.
    void requestMoreHistory(const std::string& room_id);
    // ShellBase virtual hooks (Qt6 implementations).
    void apply_theme_ui_(const tk::Theme& t) override;
    tk::ThemeMode os_color_scheme_() const override;

    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void on_rooms_updated_() override;
    void on_invites_updated_() override;
    void on_space_children_cache_ready_ui_() override;
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_join_room_outcome_ui_(bool ok, const std::string& room_id) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_media_bytes_ready_(const std::string& cache_key, MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
    void open_join_room_dialog_ui_(const std::string& prefill) override;

    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    void bind_settings_controller_() override;
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void start_inflight_tick_() override;
    void stop_inflight_tick_() override;
    void repaint_inflight_spinner_() override;
    void repaint_pickers_() override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    float get_message_scroll_fraction_() override;
    void set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_() override;
    void set_compose_draft_(const std::string&) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;

    // Unified media probe for a dropped file, passed as the extractor hook to
    // tesseract::views::route_file_drop_to_compose_bar. Detects gif/webp animation and
    // dispatches video/audio to the helpers below. When `target` is non-null
    // the result is posted to that compose bar (a pop-out window's), guarded by
    // `target_alive`; otherwise it goes to the main window's compose bar.
    void extract_drop_media_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes, std::string mime,
                             tesseract::views::ComposeBar* target = nullptr,
                             std::shared_ptr<bool> target_alive = nullptr)
        override;

    // Post an extracted MediaInfo to the right compose bar on the UI thread:
    // `target` (a pop-out, guarded by `alive`) when set, else the main window's.
    void post_pending_attachment_(const tesseract::views::MediaInfo& info,
                                  tesseract::views::ComposeBar* target,
                                  std::shared_ptr<bool> alive);

    // Creates a QMediaPlayer on the UI thread (Qt Multimedia constraint),
    // extracts the first frame as a JPEG thumbnail and the media duration,
    // then posts via post_pending_attachment_.
    void extract_drop_video_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             tesseract::views::ComposeBar* target = nullptr,
                             std::shared_ptr<bool> target_alive = nullptr);

    // Creates a QMediaPlayer on the UI thread, waits for LoadedMedia status to
    // read the duration, then posts via post_pending_attachment_.
    void extract_drop_audio_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             tesseract::views::ComposeBar* target = nullptr,
                             std::shared_ptr<bool> target_alive = nullptr);
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void rebuild_tray_() override;
    bool is_ctrl_held_() const override;
    void switch_active_account_(const std::string& user_id) override;
    void refresh_account_ui_after_switch_() override;
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


    /// Shutdown coordination. `~MainWindow` flips this flag, clears the
    /// pool of queued runnables, and waits (bounded) for in-flight
    /// workers to drain before calling `client_.stop_sync()`. Without
    /// this, a worker mid-`client_.fetch_*` racing against `~ClientFfi`
    /// is a data race on `&mut self` in Rust that surfaces as a
    /// `panic_in_cleanup` abort through cxx's `prevent_unwind` guard.

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;
    static constexpr int kAvatarCacheSize = tesseract::visual::kAvatarCacheSize;
    static constexpr int kMaxImageWidth =
        tesseract::visual::kMaxInlineImageWidth;
    static constexpr int kMaxImageHeight =
        tesseract::visual::kMaxInlineImageHeight;
    static constexpr int kMaxStickerSize = tesseract::visual::kStickerSize;
    static constexpr int kMsgMaxWidth = tesseract::visual::kMsgMaxWidth;

    // Single surface hosting the full main-app widget tree (sidebar + chat
    // panel + lightbox overlays). Three NativeTextField / NativeTextArea
    // overlays are positioned via set_on_layout().
    tk::qt6::Surface* mainAppSurface_ = nullptr;
    tesseract::views::MainAppWidget* mainApp_ = nullptr; // borrowed

    // Borrowed from mainApp_->room_view()->compose_bar()->text_area() — see
    // ComposeBar::text_area(). Quick switcher's, message search's, forward
    // picker's, find-in-room's, room-info topic-edit, Room Settings
    // name/topic, and image-pack name/shortcode/rename/paste-catcher fields
    // are all self-owned too — see QuickSwitcher::search_field() /
    // MessageSearchView::search_field() / ForwardRoomPicker::search_field() /
    // RoomSearchBar::search_field() / RoomInfoPanel::topic_field() /
    // RoomSettingsView::name_field()/topic_field() /
    // ImagePackEditorView::new_pack_name_field()/shortcode_field()/
    // pack_name_field()/paste_catcher().
    tk::TextArea* roomTextArea_ = nullptr;
    bool explicitly_quitting_ = false;  // set before quit actions to bypass hide-to-tray in closeEvent

    // Sync-progress status text (initial room hydration + key backfill).
    // Single-shot timer that defers entering the "Syncing rooms…" message
    // by 300 ms so quiet restored sessions (Init→Running in <500 ms) don't
    // flash the status bar.
    QTimer* syncStatusDebounce_ = nullptr;
    InflightDotWidget* inflightDot_ = nullptr;
    // Rich-text label for status messages carrying hyperlinks (see
    // app/status_links.h). Created lazily; hidden while plain messages
    // use statusBar()->showMessage().
    QLabel* statusLinkLabel_ = nullptr;
    QTimer* markReadTimer_ = nullptr;
    void refreshSyncStatus();

    EmojiPicker* emojiPicker_ = nullptr;
    ::StickerPicker* stickerPicker_ = nullptr;
    JoinRoomDialog* joinRoomDialog_ = nullptr;

    // When the user opens the emoji picker from a message's "+" chip
    // (rather than from the compose bar), this holds the target event
    // id. The picker's `onSelected` checks this — non-empty routes the
    // glyph to `send_reaction` instead of inserting into compose, then
    // clears it.
    std::string pendingReactionEventId_;
    std::string roomSearchPendingText_;

    // Holds an xdg-activation token to be consumed by the next
    // navigate_to_room() call. Set by notification/second-instance handlers
    // before they call navigate_to_room so the compositor grants focus.
    QString pending_wayland_token_;

    /// Raise the window and activate it. On Wayland, submits `token` via
    /// xdg_activation_v1_activate() directly (bypassing Qt). Falls back to
    /// activateWindow() on X11 or when xdg-activation-v1 is unavailable.
    void activateWindowWithToken_(const QString& token);

    QLocalServer* localServer_ = nullptr;

    QStackedWidget* contentStack_ = nullptr;
    tk::qt6::Surface* brandingSurface_ = nullptr;
    LoginView* loginView_ = nullptr;
    SettingsWidget* settingsWidget_ = nullptr;

    // Account-switcher popover anchored under the user strip. Opened by
    // left-click on the avatar when `accounts_.size() >= 2`; a single
    // account is a no-op. The popover is a frameless `Qt::Popup` `QFrame`
    // hosting a `tk::qt6::Surface` rendering the shared `AccountPicker`.
    QFrame* accountPickerPopover_ = nullptr;
    tk::qt6::Surface* accountPickerSurface_ = nullptr;
    tesseract::views::AccountPicker* accountPicker_ = nullptr;

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
    // switchActiveAccount doesn't have to wait for the next push).
    // NOTE: account_manager_, active_account_, per_account_rooms_,
    // pending_login_client_, pending_login_temp_dir_, add_account_return_idx_,
    // pending_login_is_add_account_, client_ are all inherited from ShellBase.
    // handle_*_ui_() callbacks identify the originating account via the
    // user_id parameter passed by EventHandlerBase — no bridge_ alias needed.

    std::unique_ptr<LinuxQtTrayIcon> tray_;
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

    // ── Slash-command popup ──────────────────────────────────────────────────
    QWidget*                              slash_popup_frame_   = nullptr;
    std::unique_ptr<tk::qt6::Surface>     slash_popup_surface_ = nullptr;
    tesseract::views::SlashCommandPopup*  slash_popup_widget_  = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    // Position the (already-populated) popup at the caret, sized for `rows`.
    void show_slash_popup_(tk::Rect cursor_local, int rows);
    void hide_slash_popup_();
    bool slash_popup_visible_() const
    {
        return slash_popup_frame_ && slash_popup_frame_->isVisible();
    }

    // ── GIF picker (/gif <query>) ────────────────────────────────────────────
    QWidget*                          gif_popup_frame_   = nullptr;
    std::unique_ptr<tk::qt6::Surface> gif_popup_surface_ = nullptr;
    tesseract::views::GifPopup*       gif_popup_widget_  = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;
    // Decoded first-frame previews for the strip, keyed by Tenor preview URL.
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> gif_previews_;
    std::unordered_set<std::string> gif_preview_inflight_;
    std::unordered_set<std::string> gif_anim_inflight_;
    std::shared_ptr<bool> gif_alive_ = std::make_shared<bool>(true);
    // Two-stage GIF strip cell provider (body parameterised on a repaint
    // callback). Shared by this window's strip and every pop-out's via the
    // gif_strip_image_ override.
    std::function<const tk::Image*(const tesseract::GifResult&,
                                   const std::function<void()>&)>
        gif_strip_provider_;
    const tk::Image*
    gif_strip_image_(const tesseract::GifResult& result,
                     const std::function<void()>& repaint) override;
    void show_gif_popup_();
    void hide_gif_popup_();
    bool gif_popup_visible_() const
    {
        return gif_popup_frame_ && gif_popup_frame_->isVisible();
    }
    void handle_gif_results_ui_(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results) override;
    void handle_gif_search_failed_ui_(std::uint64_t request_id,
                                      std::string message) override;

    // ── Shortcode popup ──────────────────────────────────────────────────────
    QWidget* shortcode_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> shortcode_popup_surface_ = nullptr;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    void show_shortcode_popup_(tk::Rect cursor_local, int rows);
    void hide_shortcode_popup_();
    bool shortcode_popup_visible_() const
    {
        return shortcode_popup_frame_ && shortcode_popup_frame_->isVisible();
    }

    // ── @mention popup ───────────────────────────────────────────────────────
    // cached_room_members_ is the room-switch member prefetch used by the
    // received-mention-pill avatar provider; the MentionController fetches its
    // own member list independently for autocomplete.
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;

    QWidget* mention_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> mention_popup_surface_ = nullptr;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    void show_mention_popup_(tk::Rect cursor_local, int rows);
    void hide_mention_popup_();
    bool mention_popup_visible_() const
    {
        return mention_popup_frame_ && mention_popup_frame_->isVisible();
    }

    void read_portal_color_scheme_();

    // Cached org.freedesktop.appearance color-scheme portal value.
    // -1 = not yet read, 0 = no preference, 1 = dark, 2 = light.
    int portal_color_scheme_ = -1;
};

} // namespace qt6
