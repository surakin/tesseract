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
#include <QVBoxLayout>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/visual.h>

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/ComposeBar.h"
#include "views/format.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    void on_image_packs_updated() override;
    void on_account_prefs_updated(const std::string& json) override;

signals:
    void timelineReset(QString roomId, std::vector<tesseract::Event*> snapshot);
    void messageInserted(QString roomId, std::size_t index, tesseract::Event* event);
    void messageUpdated(QString roomId, std::size_t index, tesseract::Event* event);
    void messageRemoved(QString roomId, std::size_t index);
    void roomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void syncError(QString context, QString description, bool soft_logout);
    void backupProgress(tesseract::BackupProgress progress);
    void imagePacksUpdated();
    void accountPrefsUpdated(QString json);
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
    void onRecoveryVerifyClicked();
    void onRecoverFinished(bool ok, QString error);
    void onDismissRecoveryBanner();
    void onUserStripContextMenu(const QPoint& pos);
    void onPaginateFinished(QString roomId, bool reached_start);
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

    QWidget*             userStrip_       = nullptr;
    QLabel*              userAvatarLabel_ = nullptr;
    QLabel*              userNameLabel_   = nullptr;
    std::string          myDisplayName_;
    std::string          myAvatarUrl_;

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

    tesseract::Client             client_;
    std::unique_ptr<EventBridge>  bridge_;
    std::vector<tesseract::RoomInfo> rooms_;
    std::string                   currentRoomId_;
    std::string                   pendingRestoreRoom_;
    std::string                   myUserId_;
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
