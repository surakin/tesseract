#pragma once
#include <QMainWindow>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QStackedWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "LinuxNotifier.h"
#include "LinuxQtTrayIcon.h"
#include "views/AccountPicker.h"
#include "views/ComposeBar.h"
#include "views/format.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"
#include "views/UserInfo.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QThreadPool>

class EmojiPicker;
class StickerPicker;
class QTimer;

namespace qt6 {

class LoginView;

/// Qt signal/slot bridge for SDK callbacks (runs on background thread → queued).
class EventBridge final : public QObject, public tesseract::IEventHandler {
    Q_OBJECT
public:
    explicit EventBridge(QObject* parent = nullptr) : QObject(parent) {}

    /// Called once by `MainWindow::attachNewAccount` after the user_id is
    /// known so `on_session_saved` (which runs on the SDK background
    /// thread on token refresh) can route the refreshed JSON to the right
    /// `SessionStore::save_account(user_id, …)` file.
    void set_user_id(std::string id) { user_id_ = std::move(id); }
    const std::string& user_id() const { return user_id_; }

private:
    std::string user_id_;
public:

    // IEventHandler – called on the sync thread.
    // We marshal each callback to the UI thread via a queued connection
    // by emitting a signal whose payload is a raw `Event*` (ownership
    // transferred to the slot) — Qt's meta-object system can't carry
    // `std::unique_ptr<Event>` through signals without bespoke metatype
    // wiring, so we release the pointer into the queue and `delete` it
    // in the slot.
    void on_timeline_reset(const std::string& room_id,
                            std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void on_message_inserted(const std::string& room_id,
                              std::size_t index,
                              std::unique_ptr<tesseract::Event> event) override;
    void on_message_updated(const std::string& room_id,
                             std::size_t index,
                             std::unique_ptr<tesseract::Event> event) override;
    void on_message_removed(const std::string& room_id,
                             std::size_t index) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;
    void on_room_list_state(tesseract::RoomListState state) override {
        emit roomListStateChanged(static_cast<std::uint8_t>(state));
    }
    void on_image_packs_updated() override;
    void on_account_prefs_updated(const std::string& json) override;
    void on_notification(const std::string& room_id, const std::string& room_name,
                         const std::string& sender,  const std::string& body,
                         bool is_mention,
                         const std::vector<uint8_t>& avatar_bytes) override {
        emit notificationTriggered(QString::fromStdString(room_id),
                                    QString::fromStdString(room_name),
                                    QString::fromStdString(sender),
                                    QString::fromStdString(body),
                                    is_mention,
                                    QByteArray(reinterpret_cast<const char*>(avatar_bytes.data()),
                                               static_cast<qsizetype>(avatar_bytes.size())));
    }

signals:
    void timelineReset(QString roomId, std::vector<tesseract::Event*> snapshot);
    void messageInserted(QString roomId, std::size_t index, tesseract::Event* event);
    void messageUpdated(QString roomId, std::size_t index, tesseract::Event* event);
    void messageRemoved(QString roomId, std::size_t index);
    void roomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void syncError(QString context, QString description, bool soft_logout);
    void backupProgress(tesseract::BackupProgress progress);
    void roomListStateChanged(std::uint8_t state);
    void imagePacksUpdated();
    void accountPrefsUpdated(QString json);
    void notificationTriggered(QString roomId, QString roomName,
                               QString sender, QString body, bool is_mention,
                               QByteArray avatarBytes);
};

// ---------------------------------------------------------------------------

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void onLoginSucceeded();
    void onSendClicked();
    void onTimelineReset(QString roomId, std::vector<tesseract::Event*> snapshot);
    void onMessageInserted(QString roomId, std::size_t index, tesseract::Event* event);
    void onMessageUpdated(QString roomId, std::size_t index, tesseract::Event* event);
    void onMessageRemoved(QString roomId, std::size_t index);
    void onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void onSyncError(QString context, QString description, bool soft_logout);
    void onSpaceBack();
    void onBackupProgress(tesseract::BackupProgress progress);
    void onRoomListStateChanged(std::uint8_t state);
    void onRecoveryVerifyClicked();
    void onRecoverFinished(bool ok, QString error);
    void onDismissRecoveryBanner();
    void onUserStripContextMenu(const QPoint& pos);
    void onUserStripLeftClick(const QPoint& pos);
    void onLoginCancelled();
    void onAccountSelected(const std::string& user_id);
    void onPaginateFinished(QString roomId, bool reached_start);
    void onNotificationTriggered(QString roomId, QString roomName,
                                  QString sender, QString body, bool is_mention,
                                  QByteArray avatarBytes);
    /// Frame-tick driver for animated inline media in the timeline.
    /// Advances frames in `tk_anim_images_` and repaints `msgSurface_`
    /// when at least one frame changes.
    void onMessageAnimTick_();
    /// Decode + cache worker-thread media fetch results. `kind` is one
    /// of the MediaKind enum values cast to int (QMetaType can carry int
    /// across queued connections without registering custom types).
    void onMediaBytesLoaded_(QString cache_key, int kind, QByteArray bytes);

signals:
    void recoverFinished(bool ok, QString error);
    /// Emitted on worker threads by `requestRoomAvatar_` /
    /// `requestUserAvatar_` / `requestMediaImage_` to bounce fetched
    /// bytes back onto the UI thread via QueuedConnection.
    void mediaBytesLoaded_(QString cache_key, int kind, QByteArray bytes);

private:
    void     doLogin();
    void     doLogout();

    // ---- Multi-account orchestration ----

    /// Wire `b`'s signals to the MainWindow slots. Called once per account
    /// when it's attached to `accounts_`. Slots filter on `sender()` to
    /// route only the active account's traffic to the UI.
    void     wireBridge(EventBridge* b);

    /// Detach the room/message/compose surfaces from `accounts_[old]` (if
    /// any) and rebind them to `accounts_[new_idx]`. Single chokepoint for
    /// foreground swaps; called by both the picker and the post-login
    /// flow. Rewrites `accounts.json::active_user_id`.
    void     switchActiveAccount(int new_idx);

    /// Right-click → "Add Account…" path. Records the current active index
    /// in `add_account_return_idx_`, sets `loginView_` to
    /// `LoginView::Mode::AddAccount`, creates a fresh `pending_login_client_`
    /// scoped to its own data dir, swaps the LoginView in. Cancel restores
    /// the old active account; success pushes the new `AccountSession`.
    void     beginAddAccount();

    /// Right-click → "Log Out <name>". Stops sync on the active account,
    /// clears its on-disk state, removes it from `accounts_`, rewrites
    /// `accounts.json`, and either switches to the next account or shows
    /// the LoginView in `Mode::Initial`.
    void     logoutActiveAccount();

    /// Left-click on the avatar opens the AccountPicker popover. No-op
    /// when `accounts_.size() < 2`.
    void     openAccountPicker(const QPoint& global_anchor);

    /// Refresh the `AccountPicker` row set from `accounts_`. Called after
    /// add/logout/switch so the popover reflects current state next open.
    void     rebuildAccountPicker();

    void     navigate_to_room(const std::string& room_id);
    void     populateUserStrip();
    void     maybeShowRecoveryBanner();
    void     showRooms(const std::vector<tesseract::RoomInfo>& rooms);
    void     refreshRoomList();
    void     onRoomSelected(const std::string& room_id);
    // Resolve any media bytes the row references and decode them into
    // tk::Images held in `tk_avatars_` / `tk_images_`. Shared by every
    // positional-callback path (insert / update / reset).
    void     ensureRowMedia(const tesseract::Event& ev);
    void     clearMessages();
    /// Kick off a back-pagination worker thread for `room_id`. Early-exit
    /// if a pagination is already in flight for this room or its history
    /// has been fully fetched. Hooked to `MessageListView::on_near_top`.
    void     requestMoreHistory(const std::string& room_id);
    void     updateRoomHeader(const tesseract::RoomInfo& info);
    void     updateTopicElision();
    QPixmap  makeCirclePixmap(const QPixmap& src, int size);
    QPixmap  makeInitialsPixmap(const QString& name, int size);

    // Convert a polymorphic SDK Event into the flat MessageRowData the
    // shared MessageListView consumes; downloads referenced media bytes
    // on demand and stashes decoded tk::Images in tk_images_.
    tesseract::views::MessageRowData toRowData(const tesseract::Event& ev);
    void                              ensureUserAvatar(const std::string& mxc);
    void                              ensureRoomAvatar(const tesseract::RoomInfo& r);
    void                              ensureMediaImage(const std::string& url,
                                                         int max_w, int max_h);
    void                              ensureReplyDetails(const std::string& event_id);

    /// Async media-bytes fetch. The synchronous Rust FFI does a
    /// `tokio::block_on` per call — running it on the UI thread freezes
    /// the event loop on accounts with many rooms (one round-trip per
    /// avatar serialised on first sync). These helpers spawn the fetch
    /// on a QThreadPool worker and emit `mediaBytesLoaded_`; the queued
    /// connection lands the bytes back on the UI thread for decode.
    enum class MediaKind : int {
        RoomAvatar = 0,   // tk_avatars_[mxc], scale to kRoomAvatarSize, invalidate roomSurface_
        UserAvatar = 1,   // tk_avatars_[mxc], scale to kMsgAvatarSize,  invalidate msgSurface_
        MediaImage = 2,   // tk_images_/tk_anim_images_[url], invalidate msgSurface_
    };
    void requestRoomAvatar_(const std::string& room_id,
                             const std::string& mxc);
    void requestUserAvatar_(const std::string& mxc);
    void requestMediaImage_(const std::string& url, int max_w, int max_h);
    /// Run `fn` on `mediaPool_`. No-ops when shutdown is in progress, and
    /// the runnable itself rechecks the flag before invoking `fn` so a
    /// worker that pulled from the queue just after the flag flipped
    /// bails before crossing the FFI boundary into `client_`.
    void runOnPool_(std::function<void()> fn);

    /// Shutdown coordination. `~MainWindow` flips this flag, clears the
    /// pool of queued runnables, and waits (bounded) for in-flight
    /// workers to drain before calling `client_.stop_sync()`. Without
    /// this, a worker mid-`client_.fetch_*` racing against `~ClientFfi`
    /// is a data race on `&mut self` in Rust that surfaces as a
    /// `panic_in_cleanup` abort through cxx's `prevent_unwind` guard.
    std::atomic<bool>               shuttingDown_{false};
    QThreadPool                     mediaPool_;
    std::unordered_set<std::string> mediaFetchesInFlight_;
    /// Pinned `(max_w, max_h)` for in-flight `MediaImage` fetches so the
    /// UI-thread decode can scale them. RoomAvatar / UserAvatar use the
    /// shell-wide constants and don't need pinning.
    std::unordered_map<std::string, std::pair<int,int>> mediaImageSizes_;

    static constexpr int kRoomAvatarSize  = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize   = tesseract::visual::kMsgAvatarSize;
    static constexpr int kMaxImageWidth   = tesseract::visual::kMaxInlineImageWidth;
    static constexpr int kMaxImageHeight  = tesseract::visual::kMaxInlineImageHeight;
    static constexpr int kMaxStickerSize  = tesseract::visual::kStickerSize;
    static constexpr int kMsgMaxWidth     = 520;

    // Recovery banner — replaces the legacy QFrame + QLineEdit + buttons
    // with a tk::qt6::Surface hosting the shared widget. The password
    // field is a NativeTextField overlay.
    tk::qt6::Surface*                       recoverySurface_   = nullptr;
    tesseract::views::RecoveryBanner*       recoveryShared_    = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    recoveryKeyField_;
    bool                                    recoveryBannerDismissed_ = false;

    // Sync-progress status text (initial room hydration + key backfill).
    // Cached so refreshSyncStatus() can recompute on either signal.
    tesseract::RoomListState lastRoomListState_ = tesseract::RoomListState::Init;
    tesseract::BackupState   lastBackupState_   = tesseract::BackupState::Unknown;
    std::uint64_t            lastImportedKeys_  = 0;
    // Single-shot timer that defers entering the "Syncing rooms…" message
    // by 300 ms so quiet restored sessions (Init→Running in <500 ms) don't
    // flash the status bar.
    QTimer*                  syncStatusDebounce_ = nullptr;
    bool                     syncProgressShown_  = false;
    void refreshSyncStatus();

    QWidget*             userStrip_       = nullptr;
    QLabel*              userAvatarLabel_ = nullptr;
    QLabel*              userNameLabel_   = nullptr;
    QLabel*              userIdLabel_     = nullptr;   // smaller, dimmer Matrix ID line under display name

    tk::qt6::Surface*               roomSurface_    = nullptr;
    tesseract::views::RoomListView* roomListView_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField> roomSearchField_;
    QWidget*             roomNavBar_      = nullptr;
    QPushButton*         backButton_      = nullptr;
    QLabel*              spaceNameLabel_  = nullptr;
    QWidget*             roomHeader_      = nullptr;
    QLabel*              roomHeaderAvatar_= nullptr;
    QLabel*              roomHeaderName_  = nullptr;
    QLabel*              roomHeaderTopic_ = nullptr;
    tk::qt6::Surface*                  msgSurface_      = nullptr;
    tesseract::views::MessageListView* messageListView_ = nullptr;  // borrowed
    // Compose bar — tk::qt6::Surface hosting the shared ComposeBar widget
    // with a NativeTextArea overlaid on its text_area_rect.
    tk::qt6::Surface*                       composeSurface_  = nullptr;
    tesseract::views::ComposeBar*           composeShared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextArea>     composeTextArea_;
    EmojiPicker*                            emojiPicker_     = nullptr;
    ::StickerPicker*                        stickerPicker_   = nullptr;

    // Full-window image/sticker lightbox overlay.
    QWidget*                                imgViewerHost_    = nullptr;
    tk::qt6::Surface*                       imgViewerSurface_ = nullptr;
    tesseract::views::ImageViewerOverlay*   imgViewer_        = nullptr;  // borrowed

    // Full-window video lightbox overlay.
    QWidget*                                vidViewerHost_    = nullptr;
    tk::qt6::Surface*                       vidViewerSurface_ = nullptr;
    tesseract::views::VideoViewerOverlay*   vidViewer_        = nullptr;  // borrowed

    // When the user opens the emoji picker from a message's "+" chip
    // (rather than from the compose bar), this holds the target event
    // id. The picker's `onSelected` checks this — non-empty routes the
    // glyph to `send_reaction` instead of inserting into compose, then
    // clears it.
    std::string                             pendingReactionEventId_;
    std::string                             roomSearchPendingText_;

    QStackedWidget*      contentStack_    = nullptr;
    LoginView*           loginView_       = nullptr;
    QWidget*             mainContent_     = nullptr;

    // Account-switcher popover anchored under the user strip. Opened by
    // left-click on the avatar when `accounts_.size() >= 2`; a single
    // account is a no-op. The popover is a frameless `Qt::Popup` `QFrame`
    // hosting a `tk::qt6::Surface` rendering the shared `AccountPicker`.
    QFrame*                              accountPickerPopover_  = nullptr;
    tk::qt6::Surface*                    accountPickerSurface_  = nullptr;
    tesseract::views::AccountPicker*     accountPicker_         = nullptr;

    // ---- Multi-account state ----
    //
    // Every signed-in account lives in `accounts_` as its own `AccountSession`
    // (its own `tesseract::Client`, its own `EventBridge`). All accounts
    // start sync at restore/login time and keep syncing in the background
    // so notifications fire regardless of which one is foreground. UI
    // surfaces (room list / message list / compose / recovery banner) are
    // bound only to the active account; slots filter via `sender()` to
    // ignore traffic from inactive bridges (their `RoomInfo` snapshots are
    // cached in `per_account_rooms_` so a fast switch_active_account doesn't
    // have to wait for the next push).
    std::vector<std::unique_ptr<tesseract::AccountSession>> accounts_;
    int                                                     active_account_index_ = -1;

    // Cached `RoomInfo` snapshots per account (keyed by user_id). Updated
    // from every `onRoomsUpdated` callback; read on switch_active_account
    // so the new active account's room list appears immediately.
    std::unordered_map<std::string, std::vector<tesseract::RoomInfo>> per_account_rooms_;

    // Pending login state. `pending_login_client_` is the unparented
    // `tesseract::Client` instance that `LoginView` drives through OAuth
    // (it doesn't belong to an `AccountSession` until the round-trip
    // succeeds and we know the user_id). `add_account_return_idx_` is the
    // active-account index to restore if the user cancels in
    // `Mode::AddAccount`; -1 means there was no previous active account
    // (initial login).
    std::unique_ptr<tesseract::Client> pending_login_client_;
    std::filesystem::path               pending_login_temp_dir_;   // <config>/accounts/pending-<ts>/
    int                                 add_account_return_idx_ = -1;
    bool                                pending_login_is_add_account_ = false;

    // Non-owning aliases of the active account's client + bridge. Repointed
    // by `switch_active_account`. Both are null when no account is active
    // (the LoginView is up).
    tesseract::Client*  client_ = nullptr;
    EventBridge*        bridge_ = nullptr;

    std::unique_ptr<LinuxQtTrayIcon>    tray_;
    std::vector<tesseract::RoomInfo> rooms_;
    std::string                   currentRoomId_;
    std::string                   pendingRestoreRoom_;
    // Cached identity of the foreground account. Repopulated by
    // `switchActiveAccount` from `accounts_[active_account_index_]` so the
    // sidebar strip, message rows, and the room-list view keep reading
    // them with no awareness of the multi-account layer underneath.
    std::string                   myUserId_;
    std::string                   myDisplayName_;
    std::string                   myAvatarUrl_;
    // Room-header avatar (the 40 px disc shown above the message list)
    // still uses QPixmap because the header itself remains a QLabel-
    // based widget for now. The shared RoomListView / MessageListView
    // read avatars and inline media from the tk_* maps below.
    QHash<QString, QPixmap>       avatarCache_;

    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;

    // Voice-message bytes that have been (or are being) prefetched. The
    // SDK caches the decoded blob, so we only need to mark which source
    // tokens we've already kicked a worker off for to avoid duplicates.
    std::unordered_set<std::string> voice_prefetched_;

    // Video thumbnails being generated client-side (event_id set).
    std::unordered_set<std::string> video_thumb_in_flight_;

    // Replied-to event IDs for which we have already called
    // fetch_reply_details this subscription session. Cleared on room switch
    // so a fresh subscription re-requests any still-unresolved context.
    std::unordered_set<std::string> reply_details_requested_;

    /// Animated inline-media entries for the timeline (GIF / animated
    /// WebP / APNG). Same shape as `StickerPicker::AnimatedEntry`; kept
    /// per-MainWindow so message-list rows and the sticker picker can
    /// both animate independently. `tk_anim_timer_` ticks at 60 Hz and
    /// repaints `msgSurface_` when at least one frame advances.
    struct AnimatedImage {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                         delays_ms;
        std::size_t                              current        = 0;
        std::int64_t                             next_advance_ms = 0;
    };
    std::unordered_map<std::string, AnimatedImage> tk_anim_images_;
    QTimer*                                         tk_anim_timer_ = nullptr;
    QString                       currentTopicText_;
    std::vector<std::string>      spaceStack_;

    // Per-room back-pagination state — keyed by Matrix room ID.
    // `in_flight` is true while a worker thread is awaiting the SDK call;
    // `reached_start` is true once `paginate_back_with_status` reports the
    // timeline has no further history. Both gate `requestMoreHistory`.
    struct PaginationState { bool in_flight = false; bool reached_start = false; };
    std::unordered_map<std::string, PaginationState> pagination_;

    static constexpr std::uint16_t kPaginationBatch = 50;
};

} // namespace qt6
