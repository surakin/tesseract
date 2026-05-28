#pragma once
#include <QMainWindow>
#include <QHash>
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

#include "app/EventHandlerBase.h"
#include "app/SettingsController.h"
#include "app/ShellBase.h"
#include "tk/anim_image_cache.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorQt.h"
#include "LinuxQtTrayIcon.h"
#include "views/AccountPicker.h"
#include "views/format.h"
#include "views/MainAppWidget.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"
#include "views/MentionEngine.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandEngine.h"
#include "views/SlashCommandPopup.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QLocalServer>
#include <QThreadPool>

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
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Call once after show() to bring the window to the foreground on launch.
    /// Reads XDG_ACTIVATION_TOKEN from the environment (set by Wayland-aware
    /// launchers) and uses it; falls back to activateWindow() on X11 or when
    /// no token is available.
    void activateOnStartup();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void changeEvent(QEvent* ev) override;

private slots:
    void onLoginSucceeded();
    void onSendClicked();
    void onSpaceBack();
    void onRecoveryVerifyClicked();
    void onRecoverFinished(bool ok, QString error);
    void onDismissRecoveryBanner();
    void onUserStripContextMenu(const QPoint& pos);
    void onLoginCancelled();
    void onAccountSelected(const std::string& user_id);
    void onPaginateFinished(QString roomId, bool reached_start);
    /// Frame-tick driver for animated inline media in the timeline.
    /// Advances frames in `anim_cache_` and repaints `msgSurface_`
    /// when at least one frame changes.
    void onMessageAnimTick_();

    void on_portal_setting_changed_(const QString& ns, const QString& key,
                                    const QDBusVariant& value);
    void onActivateRequested();

signals:
    void recoverFinished(bool ok, QString error);

private:
    void doLogin();
    void doLogout();
    void openSettings();
    void setupLocalServer_();

    // ── EventHandlerBase UI-thread hook overrides (Qt6) ──────────────────────
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
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
    void on_server_info_ready_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;

    // ---- Multi-account orchestration ----

    /// Detach the room/message/compose surfaces from `accounts_[old]` (if
    /// any) and rebind them to `accounts_[new_idx]`. Single chokepoint for
    /// foreground swaps; called by both the picker and the post-login
    /// flow. Rewrites `accounts.json::active_user_id`.
    void switchActiveAccount(int new_idx);

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
    /// when `accounts_.size() < 2`.
    void openAccountPicker(const QPoint& global_anchor);

    /// Refresh the `AccountPicker` row set from `accounts_`. Called after
    /// add/logout/switch so the popover reflects current state next open.
    void rebuildAccountPicker();

    void navigate_to_room(const std::string& room_id);
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void populateUserStrip();
    void maybeShowRecoveryBanner();
    void showRooms(const std::vector<tesseract::RoomInfo>& rooms);
    void refreshRoomList();
    void onRoomSelected(const std::string& room_id);
    // Resolve any media bytes the row references and decode them into
    // tk::Images held in `tk_avatars_` / `tk_images_`. Shared by every
    // positional-callback path (insert / update / reset). Overrides the
    // ShellBase hook to also record decode-size hints (mediaImageSizes_).
    void prep_row_media_(const tesseract::Event& ev) override;
    void clearMessages();
    /// Kick off a back-pagination worker thread for `room_id`. Early-exit
    /// if a pagination is already in flight for this room or its history
    /// has been fully fetched. Hooked to `RoomView::on_near_top`.
    void requestMoreHistory(const std::string& room_id);
    /// Show a QCalendarWidget dialog; on acceptance resolves the chosen date
    /// to an event via MSC3030 and switches the timeline to focused mode.
    void openJumpToDateDialog();

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
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_media_bytes_ready_(const std::string& cache_key, MediaKind kind,
                               std::vector<uint8_t> bytes) override;

    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void repaint_pickers_() override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    float get_message_scroll_fraction_() override;
    void set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_() override;
    void set_compose_draft_(const std::string&) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;

    // Called from set_on_file_drop when a video is dropped. Creates a
    // QMediaPlayer on the UI thread (Qt Multimedia constraint), extracts
    // the first frame as a JPEG thumbnail and the media duration, then
    // calls update_pending_attachment on the current compose_bar().
    void extract_drop_video_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes);

    // Called from set_on_file_drop when an audio file is dropped. Creates
    // a QMediaPlayer on the UI thread, waits for LoadedMedia status to read
    // the duration, then calls update_pending_attachment on the current
    // compose_bar().
    void extract_drop_audio_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes);
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;

    /// Run `fn` on `mediaPool_`. No-ops when shutdown is in progress, and
    /// the runnable itself rechecks the flag before invoking `fn` so a
    /// worker that pulled from the queue just after the flag flipped
    /// bails before crossing the FFI boundary into `client_`.
    void runOnPool_(std::function<void()> fn);

    /// Kick a full-resolution fetch for the image viewer (decoded at native
    /// pixels, not the scaled inline thumbnail size). Idempotent — no-op if
    /// already cached, animated (anim_cache_ has the original frames), or
    /// already in flight. Shared by the image-click and avatar-click paths.
    void ensureViewerFullres_(const std::string& url);

    /// Shutdown coordination. `~MainWindow` flips this flag, clears the
    /// pool of queued runnables, and waits (bounded) for in-flight
    /// workers to drain before calling `client_.stop_sync()`. Without
    /// this, a worker mid-`client_.fetch_*` racing against `~ClientFfi`
    /// is a data race on `&mut self` in Rust that surfaces as a
    /// `panic_in_cleanup` abort through cxx's `prevent_unwind` guard.
    std::atomic<bool> shuttingDown_{false};
    QThreadPool mediaPool_;
    /// Pinned `(max_w, max_h)` for in-flight `MediaImage` fetches so the
    /// UI-thread decode can scale them. RoomAvatar / UserAvatar use the
    /// shell-wide constants and don't need pinning.
    std::unordered_map<std::string, std::pair<int, int>> mediaImageSizes_;

    /// Full-resolution images for the image viewer lightbox. Separate from
    /// tk_images_ which stores inline-scaled (320×200) thumbnails. Keyed by
    /// the same source URL / MediaSource JSON as tk_images_.
    std::unordered_map<std::string, std::unique_ptr<tk::Image>>
        viewerFullresCache_;
    std::unordered_set<std::string> viewerFullresInFlight_;

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;
    static constexpr int kAvatarCacheSize = tesseract::visual::kAvatarCacheSize;
    static constexpr int kMaxImageWidth =
        tesseract::visual::kMaxInlineImageWidth;
    static constexpr int kMaxImageHeight =
        tesseract::visual::kMaxInlineImageHeight;
    static constexpr int kMaxStickerSize = tesseract::visual::kStickerSize;
    static constexpr int kMsgMaxWidth = 520;

    // Single surface hosting the full main-app widget tree (sidebar + chat
    // panel + lightbox overlays). Three NativeTextField / NativeTextArea
    // overlays are positioned via set_on_layout().
    tk::qt6::Surface* mainAppSurface_ = nullptr;
    tesseract::views::MainAppWidget* mainApp_ = nullptr; // borrowed

    // Native overlays wired to mainAppSurface_.
    std::unique_ptr<tk::NativeTextField> recoveryKeyField_;
    std::unique_ptr<tk::NativeTextField> roomSearchField_;
    std::unique_ptr<tk::NativeTextArea> roomTextArea_;
    std::unique_ptr<tk::NativeTextArea> topicTextArea_;
    bool topicTextAreaVisible_ = false; // mirrors topicTextArea_ visibility for transition detection

    // Sync-progress status text (initial room hydration + key backfill).
    // Single-shot timer that defers entering the "Syncing rooms…" message
    // by 300 ms so quiet restored sessions (Init→Running in <500 ms) don't
    // flash the status bar.
    QTimer* syncStatusDebounce_ = nullptr;
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

    std::unique_ptr<tesseract::SettingsController> settings_controller_;

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
    // Every signed-in account lives in `accounts_` as its own `AccountSession`
    // (its own `tesseract::Client`, its own `EventBridge`). All accounts
    // start sync at restore/login time and keep syncing in the background
    // so notifications fire regardless of which one is foreground. UI
    // surfaces (room list / message list / compose / recovery banner) are
    // bound only to the active account; handle_*_ui_() methods filter by
    // user_id to ignore traffic from inactive accounts (their `RoomInfo`
    // snapshots are cached in `per_account_rooms_` so a fast
    // switch_active_account doesn't have to wait for the next push).
    // NOTE: accounts_, active_account_index_, per_account_rooms_,
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
    QTimer* presence_tick_timer_ = nullptr;

    // ── Slash-command popup ──────────────────────────────────────────────────
    tesseract::views::SlashCommandEngine  slash_engine_;

    QWidget*                              slash_popup_frame_   = nullptr;
    std::unique_ptr<tk::qt6::Surface>     slash_popup_surface_ = nullptr;
    tesseract::views::SlashCommandPopup*  slash_popup_widget_  = nullptr;

    void show_slash_popup_(
        const std::vector<tesseract::views::SlashCommandSuggestion>& items,
        tk::Rect cursor_local);
    void hide_slash_popup_();
    bool slash_popup_visible_() const
    {
        return slash_popup_frame_ && slash_popup_frame_->isVisible();
    }

    // ── Shortcode popup ──────────────────────────────────────────────────────
    tesseract::views::ShortcodeEngine shortcode_engine_;
    tesseract::views::ShortcodeMatch shortcode_active_match_{};
    std::vector<tesseract::views::ShortcodeSuggestion>
        shortcode_current_suggestions_;

    QWidget* shortcode_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> shortcode_popup_surface_ = nullptr;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;

    void show_shortcode_popup_(
        const std::vector<tesseract::views::ShortcodeSuggestion>& suggestions,
        tk::Rect cursor_rect);
    void hide_shortcode_popup_();
    bool shortcode_popup_visible_() const
    {
        return shortcode_popup_frame_ && shortcode_popup_frame_->isVisible();
    }

    // ── @mention popup ───────────────────────────────────────────────────────
    tesseract::views::MentionEngine mention_engine_;
    tesseract::views::MentionMatch mention_active_match_{};
    std::vector<tesseract::views::MentionCandidate> mention_current_candidates_;
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;
    std::string members_fetching_room_;

    QWidget* mention_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> mention_popup_surface_ = nullptr;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;

    void show_mention_popup_(
        const std::vector<tesseract::views::MentionCandidate>& candidates,
        tk::Rect cursor_rect);
    void hide_mention_popup_();
    bool mention_popup_visible_() const
    {
        return mention_popup_frame_ && mention_popup_frame_->isVisible();
    }
    void accept_mention_(const tesseract::views::MentionCandidate& c);
    bool handle_mention_on_changed_(const std::string& s, int cursor);

    void read_portal_color_scheme_();

    // Cached org.freedesktop.appearance color-scheme portal value.
    // -1 = not yet read, 0 = no preference, 1 = dark, 2 = light.
    int portal_color_scheme_ = -1;
};

} // namespace qt6
