#include "MainWindow.h"
#include "LoginView.h"
#include "EmojiPicker.h"
#include "StickerPicker.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"

#include <QThreadPool>
#include <QMenu>
#include <QAction>
#include <QBuffer>
#include <QImageReader>
#include <QPointer>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>

#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMetaType>
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QIcon>
#include <QScrollBar>
#include <QStringList>
#include <QToolButton>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QTimer>
#include <QStandardItem>
#include <algorithm>
#include <thread>
#include <unordered_set>

Q_DECLARE_METATYPE(tesseract::Event*)
Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)
Q_DECLARE_METATYPE(tesseract::BackupProgress)

namespace qt6 {

// ---------------------------------------------------------------------------
// EventBridge
// ---------------------------------------------------------------------------

void EventBridge::on_timeline_reset(
    const std::string& room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    // Release ownership of each Event into a raw-pointer vector so it
    // can ride a Qt queued connection (`unique_ptr` is not Q_DECLARE_METATYPE-
    // friendly). The slot is responsible for `delete`-ing every entry.
    std::vector<tesseract::Event*> raw;
    raw.reserve(snapshot.size());
    for (auto& p : snapshot) raw.push_back(p.release());
    emit timelineReset(QString::fromStdString(room_id), std::move(raw));
}

void EventBridge::on_message_inserted(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> event)
{
    emit messageInserted(QString::fromStdString(room_id), index, event.release());
}

void EventBridge::on_message_updated(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> event)
{
    emit messageUpdated(QString::fromStdString(room_id), index, event.release());
}

void EventBridge::on_message_removed(
    const std::string& room_id,
    std::size_t index)
{
    emit messageRemoved(QString::fromStdString(room_id), index);
}

void EventBridge::on_rooms_updated(
    const std::vector<tesseract::RoomInfo>& rooms)
{
    emit roomsUpdated(rooms);
}

void EventBridge::on_sync_error(
    const std::string& context,
    const std::string& description,
    bool soft_logout)
{
    emit syncError(QString::fromStdString(context),
                   QString::fromStdString(description),
                   soft_logout);
}

void EventBridge::on_session_saved(const std::string& session_json) {
    // Per-account save: this bridge belongs to exactly one account; the
    // user_id was set at attach time. Empty user_id means the bridge
    // hasn't been adopted into `accounts_` yet (the OAuth round-trip is
    // still in flight) — in that case the post-login attach path saves
    // the session itself.
    if (user_id_.empty()) return;
    tesseract::SessionStore::save_account(user_id_, session_json);
}

void EventBridge::on_backup_progress(const tesseract::BackupProgress& progress) {
    emit backupProgress(progress);
}

// on_room_list_state is inlined in MainWindow.h — it emits a queued
// `roomListStateChanged` signal carrying the u8 code (Qt's meta-object
// system handles std::uint8_t natively across queued connections).

void EventBridge::on_image_packs_updated() {
    emit imagePacksUpdated();
}

void EventBridge::on_account_prefs_updated(const std::string& json) {
    emit accountPrefsUpdated(QString::fromStdString(json));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QPixmap MainWindow::makeCirclePixmap(const QPixmap& src, int size) {
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    p.setClipPath(path);
    p.drawPixmap(0, 0, size, size, src);
    return result;
}

QPixmap MainWindow::makeInitialsPixmap(const QString& name, int size) {
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(0x8E, 0x8E, 0x93));
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, size, size);
    p.setPen(Qt::white);
    QFont f;
    f.setPixelSize(size / 2);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
               name.isEmpty() ? "?" : QString(name[0].toUpper()));
    return result;
}

// ---------------------------------------------------------------------------
// MainWindow constructor
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<tesseract::Event*>();
    qRegisterMetaType<std::vector<tesseract::Event*>>();
    qRegisterMetaType<std::size_t>("std::size_t");
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();
    qRegisterMetaType<tesseract::BackupProgress>();

    setWindowTitle("Tesseract");
    resize(1100, 768);

    contentStack_ = new QStackedWidget(this);
    setCentralWidget(contentStack_);

    loginView_ = new LoginView(contentStack_);
    contentStack_->addWidget(loginView_);
    connect(loginView_, &LoginView::loginSucceeded,
            this,       &MainWindow::onLoginSucceeded);
    connect(loginView_, &LoginView::loginCancelled,
            this,       &MainWindow::onLoginCancelled);

    mainContent_ = new QWidget(contentStack_);
    contentStack_->addWidget(mainContent_);

    auto* hLayout = new QHBoxLayout(mainContent_);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    // ---- Sidebar (room list) ----
    auto* sidePanel = new QWidget(mainContent_);
    sidePanel->setFixedWidth(260);
    sidePanel->setObjectName("sidePanel");
    sidePanel->setStyleSheet("#sidePanel { background-color: #F0F2F5; }");

    auto* sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);

    // Nav bar (shown only when inside a space)
    roomNavBar_ = new QWidget(sidePanel);
    auto* navLayout = new QHBoxLayout(roomNavBar_);
    navLayout->setContentsMargins(4, 4, 4, 4);
    navLayout->setSpacing(4);
    backButton_     = new QPushButton("←", roomNavBar_);
    backButton_->setFixedWidth(32);
    spaceNameLabel_ = new QLabel("", roomNavBar_);
    spaceNameLabel_->setStyleSheet("font-weight: bold; font-size: 11px;");
    navLayout->addWidget(backButton_);
    navLayout->addWidget(spaceNameLabel_, 1);
    roomNavBar_->setVisible(false);
    connect(backButton_, &QPushButton::clicked, this, &MainWindow::onSpaceBack);
    sideLayout->addWidget(roomNavBar_);

    // Shared-toolkit room list. The Surface is a QWidget that hosts a
    // tk::Widget tree painted via QPainter; the RoomListView inside
    // owns the actual layout + paint + selection state.
    roomSurface_ = new tk::qt6::Surface(tk::Theme::light(), sidePanel);
    auto room_view_owner = std::make_unique<tesseract::views::RoomListView>();
    roomListView_ = room_view_owner.get();
    roomListView_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = tk_avatars_.find(mxc);
            return it == tk_avatars_.end() ? nullptr : it->second.get();
        });
    roomListView_->on_room_selected =
        [this](const std::string& room_id) { onRoomSelected(room_id); };
    roomSurface_->set_root(std::move(room_view_owner));
    sideLayout->addWidget(roomSurface_, 1);

    // Search field — host-overlaid NativeTextField (a QLineEdit under
    // the hood) shown only when the list overflows the viewport; the
    // RoomListView itself decides visibility in its arrange() pass.
    roomSearchField_ = roomSurface_->host().make_text_field();
    roomSearchField_->set_placeholder("Search rooms");
    roomSearchField_->set_visible(false);
    auto* searchDebounce = new QTimer(this);
    searchDebounce->setSingleShot(true);
    roomSearchField_->set_on_changed([this, searchDebounce](const std::string& q) {
        roomSearchPendingText_ = q;
        searchDebounce->start(500);
    });
    connect(searchDebounce, &QTimer::timeout, [this] {
        if (roomListView_) roomListView_->set_search_text(roomSearchPendingText_);
    });
    roomSurface_->set_on_layout([this] {
        if (!roomListView_ || !roomSearchField_) return;
        bool visible = roomListView_->search_field_visible();
        roomSearchField_->set_visible(visible);
        if (visible) {
            roomSearchField_->set_rect(
                roomListView_->search_field_rect());
        }
    });

    // ---- User identity strip (footer) ----
    //
    // Two-line layout: bold display name on top, smaller dimmer Matrix ID
    // on the second line. Left-click opens the AccountPicker popover when
    // there are ≥2 accounts; right-click pops the Add Account / Log Out
    // context menu. The strip is intentionally still a native composite
    // (QLabel + QLabel) rather than a tk::Surface — the canvas-painted
    // strip migration is tracked as a follow-up cosmetic change.
    userStrip_ = new QWidget(sidePanel);
    userStrip_->setObjectName("userStrip");
    userStrip_->setStyleSheet(
        "#userStrip { background-color:#E8EAEE; border-top:1px solid #D0D3D8; }");
    userStrip_->setFixedHeight(56);
    userStrip_->setVisible(false);
    userStrip_->setContextMenuPolicy(Qt::CustomContextMenu);
    userStrip_->setCursor(Qt::PointingHandCursor);
    userStrip_->installEventFilter(this);   // mouse-press for left-click → picker
    {
        auto* uLayout = new QHBoxLayout(userStrip_);
        uLayout->setContentsMargins(8, 8, 8, 8);
        uLayout->setSpacing(10);

        userAvatarLabel_ = new QLabel(userStrip_);
        userAvatarLabel_->setFixedSize(32, 32);
        uLayout->addWidget(userAvatarLabel_);

        auto* textCol = new QWidget(userStrip_);
        auto* textBox = new QVBoxLayout(textCol);
        textBox->setContentsMargins(0, 0, 0, 0);
        textBox->setSpacing(2);

        userNameLabel_ = new QLabel(textCol);
        userNameLabel_->setStyleSheet("font-size:13px; font-weight:bold; color:#111111;");
        userNameLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
        textBox->addWidget(userNameLabel_);

        userIdLabel_ = new QLabel(textCol);
        userIdLabel_->setStyleSheet("font-size:11px; color:#888888;");
        userIdLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
        textBox->addWidget(userIdLabel_);

        uLayout->addWidget(textCol, 1);
    }
    sideLayout->addWidget(userStrip_);
    connect(userStrip_, &QWidget::customContextMenuRequested,
            this,       &MainWindow::onUserStripContextMenu);

    hLayout->addWidget(sidePanel);

    // 1px vertical separator between sidebar and chat area. A QFrame::VLine
    // is used instead of a stylesheet border on the sidebar, because the
    // QListView's own background paints over the parent's right-edge border.
    auto* sideSeparator = new QFrame(mainContent_);
    sideSeparator->setFrameShape(QFrame::VLine);
    sideSeparator->setFrameShadow(QFrame::Plain);
    sideSeparator->setLineWidth(1);
    sideSeparator->setStyleSheet("color: #D0D3D8;");
    sideSeparator->setFixedWidth(1);
    hLayout->addWidget(sideSeparator);

    // ---- Main chat area ----
    auto* chatPanel = new QWidget(this);
    auto* vLayout   = new QVBoxLayout(chatPanel);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
    hLayout->addWidget(chatPanel, 1);

    // Room header bar
    roomHeader_ = new QWidget(chatPanel);
    roomHeader_->setObjectName("roomHeader");
    roomHeader_->setStyleSheet(
        "#roomHeader { background-color:#FFFFFF; border-bottom:1px solid #D0D3D8; }");
    roomHeader_->setFixedHeight(60);
    roomHeader_->setVisible(false);

    auto* headerLayout = new QHBoxLayout(roomHeader_);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    headerLayout->setSpacing(12);

    roomHeaderAvatar_ = new QLabel(roomHeader_);
    roomHeaderAvatar_->setFixedSize(40, 40);
    headerLayout->addWidget(roomHeaderAvatar_);

    auto* nameBlock = new QWidget(roomHeader_);
    auto* nameVBox  = new QVBoxLayout(nameBlock);
    nameVBox->setContentsMargins(0, 0, 0, 0);
    nameVBox->setSpacing(2);

    roomHeaderName_ = new QLabel(nameBlock);
    roomHeaderName_->setStyleSheet("font-size:14px; font-weight:bold; color:#111111;");
    nameVBox->addWidget(roomHeaderName_);

    roomHeaderTopic_ = new QLabel(nameBlock);
    roomHeaderTopic_->setStyleSheet("font-size:11px; color:#65676B;");
    roomHeaderTopic_->setVisible(false);
    roomHeaderTopic_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    roomHeaderTopic_->installEventFilter(this);
    nameVBox->addWidget(roomHeaderTopic_);

    headerLayout->addWidget(nameBlock, 1);
    vLayout->addWidget(roomHeader_);

    // Recovery banner — shared widget hosted in a tk::qt6::Surface.
    recoverySurface_ = new tk::qt6::Surface(tk::Theme::light(), chatPanel);
    recoverySurface_->setFixedHeight(48);
    recoverySurface_->setVisible(false);
    {
        auto banner = std::make_unique<tesseract::views::RecoveryBanner>();
        recoveryShared_ = banner.get();
        recoveryShared_->on_verify =
            [this](const std::string& key) { (void)key; onRecoveryVerifyClicked(); };
        recoveryShared_->on_dismiss =
            [this] { onDismissRecoveryBanner(); };
        recoverySurface_->set_root(std::move(banner));

        recoveryKeyField_ = recoverySurface_->host().make_text_field();
        recoveryKeyField_->set_placeholder("Recovery key or passphrase");
        recoveryKeyField_->set_password(true);
        recoveryKeyField_->set_on_changed([this](const std::string& k) {
            if (recoveryShared_) recoveryShared_->set_current_key(k);
        });
        recoveryKeyField_->set_on_submit([this] { onRecoveryVerifyClicked(); });

        recoverySurface_->set_on_layout([this] {
            if (!recoveryShared_ || !recoveryKeyField_) return;
            recoveryKeyField_->set_visible(
                recoveryShared_->recovery_key_field_visible());
            recoveryKeyField_->set_rect(
                recoveryShared_->recovery_key_field_rect());
        });
    }
    connect(this, &MainWindow::recoverFinished,
            this, &MainWindow::onRecoverFinished, Qt::QueuedConnection);
    vLayout->addWidget(recoverySurface_);

    // Shared-toolkit message list. Each row is paint-only — no per-row
    // QWidget — and the surface owns scrolling, hit-testing, and
    // viewport virtualisation.
    msgSurface_ = new tk::qt6::Surface(tk::Theme::light(), chatPanel);
    auto msg_view_owner = std::make_unique<tesseract::views::MessageListView>();
    messageListView_ = msg_view_owner.get();
    messageListView_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            auto it = tk_avatars_.find(mxc);
            return it == tk_avatars_.end() ? nullptr : it->second.get();
        });
    messageListView_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            // Animated entries take priority — onMessageAnimTick_
            // keeps `current` valid; static cache is the second hop.
            auto ait = tk_anim_images_.find(mxc);
            if (ait != tk_anim_images_.end() && !ait->second.frames.empty()) {
                return ait->second.frames[ait->second.current].get();
            }
            auto it = tk_images_.find(mxc);
            return it == tk_images_.end() ? nullptr : it->second.get();
        });
    // Voice (MSC3245) playback wiring. The Qt backend builds a QMediaPlayer
    // when `make_audio_player()` is called; the bytes provider piggybacks
    // on the SDK media cache via fetch_source_bytes (synchronous; empty on
    // cache miss, in which case the view stays in its idle state).
    if (auto player = msgSurface_->host().make_audio_player()) {
        messageListView_->set_audio_player(std::move(player));
    }
    messageListView_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return client_->fetch_source_bytes(source_json);
        });
    {
        QPointer<tk::qt6::Surface> sfp = msgSurface_;
        messageListView_->set_repaint_requester([sfp]() {
            if (sfp) sfp->update();
        });
    }
    msgSurface_->set_root(std::move(msg_view_owner));
    vLayout->addWidget(msgSurface_, 1);

    // Right-click context menu on sticker rows. The shared
    // MessageListView records sticker rects per-paint (world coords);
    // we feed the QContextMenuEvent's local-widget position to
    // `sticker_hit_at` and, on a hit, offer "Add to Saved Stickers".
    // Suppress the menu when the sticker is already in the user pack.
    msgSurface_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(msgSurface_, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (!messageListView_) return;
        // Surface coordinates equal MessageListView-local coordinates
        // since the view is the surface's root widget.
        auto hit = messageListView_->sticker_hit_at(
            tk::Point{ static_cast<float>(pos.x()),
                       static_cast<float>(pos.y()) });
        if (!hit) return;
        if (client_->user_pack_has_sticker(hit->mxc_url)) return;

        // Capture relevant fields up front; the hit_at result is
        // recomputed each paint so we don't hold a reference past
        // the menu lifetime.
        const auto event_id = hit->event_id;
        const auto mxc_url  = hit->mxc_url;
        const auto body     = hit->body;

        QMenu menu(this);
        QAction* add = menu.addAction(tr("Add to Saved Stickers"));
        connect(add, &QAction::triggered, this, [this, mxc_url, body, event_id]{
            // Width/height: best-effort from messageListView's row data.
            // We don't track them here; the SDK preserves info_json
            // round-trip via the original event when the picker later
            // sends. For Add-to-pack, an empty info object is fine.
            client_->save_sticker_to_user_pack(body, body, mxc_url, "{}");
        });
        menu.exec(msgSurface_->mapToGlobal(pos));
    });

    // Image / sticker lightbox overlay — full-window surface that paints a
    // dark backdrop + the selected image. Shown on `on_image_clicked`,
    // hidden on `on_close` or Escape. Host is a bare QWidget that we
    // manually position over `mainContent_`; it is not in any layout.
    {
        auto viewer_owner = std::make_unique<tesseract::views::ImageViewerOverlay>();
        imgViewer_        = viewer_owner.get();
        imgViewerHost_    = new QWidget(mainContent_);
        imgViewerHost_->setGeometry(mainContent_->rect());
        imgViewerHost_->hide();
        imgViewerSurface_ = new tk::qt6::Surface(tk::Theme::light(), imgViewerHost_);
        auto* ovLayout = new QVBoxLayout(imgViewerHost_);
        ovLayout->setContentsMargins(0, 0, 0, 0);
        ovLayout->addWidget(imgViewerSurface_);
        imgViewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto ait = tk_anim_images_.find(url);
                if (ait != tk_anim_images_.end() && !ait->second.frames.empty())
                    return ait->second.frames[ait->second.current].get();
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        imgViewer_->on_close = [this] { imgViewerHost_->hide(); };
        imgViewerSurface_->set_root(std::move(viewer_owner));
    }

    messageListView_->on_image_clicked =
        [this](const tesseract::views::MessageListView::ImageHit& hit) {
            if (!imgViewer_ || !imgViewerHost_) return;
            imgViewer_->open(hit.media_url, hit.body, hit.natural_w, hit.natural_h);
            imgViewerHost_->setGeometry(mainContent_->rect());
            imgViewerHost_->raise();
            imgViewerHost_->show();
            imgViewerHost_->setFocus();
        };

    // Video lightbox overlay — full-window surface for m.video playback.
    {
        auto viewer_owner = std::make_unique<tesseract::views::VideoViewerOverlay>();
        vidViewer_        = viewer_owner.get();
        vidViewerHost_    = new QWidget(mainContent_);
        vidViewerHost_->setGeometry(mainContent_->rect());
        vidViewerHost_->hide();
        vidViewerSurface_ = new tk::qt6::Surface(tk::Theme::light(), vidViewerHost_);
        auto* ovLayout = new QVBoxLayout(vidViewerHost_);
        ovLayout->setContentsMargins(0, 0, 0, 0);
        ovLayout->addWidget(vidViewerSurface_);
        vidViewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        vidViewer_->set_video_player(msgSurface_->host().make_video_player());
        vidViewer_->set_repaint_requester([this] {
            if (vidViewerSurface_) vidViewerSurface_->relayout();
        });
        vidViewer_->on_close = [this] { vidViewerHost_->hide(); };
        vidViewerSurface_->set_root(std::move(viewer_owner));
    }

    messageListView_->on_video_clicked =
        [this](const tesseract::views::MessageListView::VideoHit& hit) {
            if (!vidViewer_ || !vidViewerHost_) return;
            vidViewer_->open(hit.source_json, hit.thumbnail_url, hit.mime_type,
                             hit.duration_ms, hit.natural_w, hit.natural_h);
            vidViewerHost_->setGeometry(mainContent_->rect());
            vidViewerHost_->raise();
            vidViewerHost_->show();
            vidViewerHost_->setFocus();
            // Async byte fetch on a worker thread.
            std::string src = hit.source_json;
            runOnPool_([this, src = std::move(src)]() {
                auto bytes = client_->fetch_source_bytes(src);
                QMetaObject::invokeMethod(this, [this, bytes = std::move(bytes)]() mutable {
                    if (vidViewer_)
                        vidViewer_->load_bytes(bytes.data(), bytes.size());
                }, Qt::QueuedConnection);
            });
        };

    // Compose bar — shared widget on a tk::qt6::Surface. The text input
    // is a NativeTextArea overlaid on the shared ComposeBar's
    // text_area_rect; the emoji + send buttons paint into the toolkit.
    composeSurface_ = new tk::qt6::Surface(tk::Theme::light(), chatPanel);
    composeSurface_->setFixedHeight(
        static_cast<int>(tesseract::views::ComposeBar::kMinHeight));
    auto compose_owner = std::make_unique<tesseract::views::ComposeBar>();
    composeShared_ = compose_owner.get();
    composeSurface_->set_root(std::move(compose_owner));

    composeTextArea_ = composeSurface_->host().make_text_area();
    composeTextArea_->set_placeholder(tr("Message\xe2\x80\xa6").toStdString());
    composeTextArea_->set_on_changed([this](const std::string& s) {
        if (composeShared_) composeShared_->set_current_text(s);
    });
    composeTextArea_->set_on_submit([this] { onSendClicked(); });
    composeTextArea_->set_on_height_changed([this](float h) {
        if (!composeShared_ || !composeSurface_) return;
        composeShared_->set_text_area_natural_height(h);
        composeSurface_->setFixedHeight(
            static_cast<int>(composeShared_->natural_height()));
        composeSurface_->relayout();
    });
    composeTextArea_->set_on_image_paste(
        [this](std::vector<std::uint8_t> bytes, std::string mime) {
            if (composeShared_)
                composeShared_->set_pending_image(std::move(bytes),
                                                    std::move(mime));
        });

    // Drag-and-drop: dropping any file on either the message list or
    // the composer parks it in the compose bar (image → preview band,
    // anything else → file chip). Same handler wired to both surfaces;
    // the sidebar gets none (drops there show "no drop" by virtue of
    // accepting nothing).
    auto onFileDrop = [this](std::vector<std::uint8_t> bytes,
                             std::string mime,
                             std::string filename) {
        if (!composeShared_) return;
        const auto limit = client_->media_upload_limit();
        if (limit > 0 && bytes.size() > limit) {
            statusBar()->showMessage(
                tr("File exceeds server limit (%1)")
                    .arg(QString::fromStdString(
                        tesseract::views::format_size(limit))),
                4000);
            return;
        }
        if (mime.rfind("image/", 0) == 0) {
            composeShared_->set_pending_image(std::move(bytes),
                                              std::move(mime),
                                              std::move(filename));
        } else {
            composeShared_->set_pending_file(std::move(bytes),
                                             std::move(mime),
                                             std::move(filename));
        }
    };
    composeSurface_->set_on_file_drop(onFileDrop);
    if (msgSurface_) msgSurface_->set_on_file_drop(onFileDrop);
    composeSurface_->set_on_layout([this] {
        if (composeShared_ && composeTextArea_)
            composeTextArea_->set_rect(composeShared_->text_area_rect());
    });

    composeShared_->on_send  = [this](const std::string& body) {
        if (currentRoomId_.empty()) return;
        std::string trimmed = QString::fromStdString(body).trimmed().toStdString();
        if (trimmed.empty()) return;
        auto res = client_->send_message(currentRoomId_, trimmed);
        if (res) {
            if (composeTextArea_) composeTextArea_->set_text("");
            if (composeShared_)   composeShared_->set_current_text({});
        } else {
            statusBar()->showMessage(QString::fromStdString(res.message), 4000);
        }
    };
    composeShared_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                             std::string mime,
                                             std::string filename,
                                             std::string caption,
                                             std::uint32_t /*src_w*/,
                                             std::uint32_t /*src_h*/,
                                             std::string reply_event_id) {
        if (currentRoomId_.empty()) return;
        const bool compress =
            tesseract::Settings::instance().image_quality
            == tesseract::Settings::ImageQuality::Compressed;
        auto enc = composeSurface_->host().encode_for_send(
            bytes.data(), bytes.size(), compress);
        if (enc.bytes.empty()) {
            statusBar()->showMessage(tr("Image decode failed"), 4000);
            return;
        }
        // After compression mime/extension may have changed to JPEG.
        std::string out_name = filename;
        if (enc.mime == "image/jpeg") {
            auto dot = out_name.find_last_of('.');
            if (dot != std::string::npos) out_name = out_name.substr(0, dot);
            out_name += ".jpg";
        }
        auto res = client_->send_image(currentRoomId_, enc.bytes, enc.mime,
                                        out_name, caption,
                                        enc.width, enc.height,
                                        reply_event_id);
        if (!res) {
            statusBar()->showMessage(
                tr("Send image failed: %1").arg(QString::fromStdString(res.message)),
                4000);
            return;
        }
        // Clear the caption now that the image went out.
        if (composeTextArea_) composeTextArea_->set_text("");
        if (composeShared_)   composeShared_->set_current_text({});
    };
    composeShared_->on_send_file = [this](std::vector<std::uint8_t> bytes,
                                            std::string mime,
                                            std::string filename,
                                            std::string caption,
                                            std::string reply_event_id) {
        if (currentRoomId_.empty()) return;
        auto res = client_->send_file(currentRoomId_, bytes, mime,
                                      filename, caption, reply_event_id);
        if (!res) {
            statusBar()->showMessage(
                tr("Send file failed: %1").arg(QString::fromStdString(res.message)),
                4000);
            return;
        }
        if (composeTextArea_) composeTextArea_->set_text("");
        if (composeShared_)   composeShared_->set_current_text({});
    };
    composeShared_->on_size_changed = [this] {
        if (!composeShared_ || !composeSurface_) return;
        composeSurface_->setFixedHeight(
            static_cast<int>(composeShared_->natural_height()));
        composeSurface_->relayout();
    };
    composeShared_->on_emoji = [this] {
        if (!emojiPicker_) return;
        if (emojiPicker_->isVisible()) emojiPicker_->hide();
        else                            emojiPicker_->popupAt(composeSurface_);
    };
    composeShared_->on_sticker = [this] {
        if (!stickerPicker_) return;
        if (stickerPicker_->isVisible()) stickerPicker_->hide();
        else                              stickerPicker_->popupAt(composeSurface_);
    };
    composeShared_->on_send_reply = [this](const std::string& reply_event_id,
                                            const std::string& body) {
        if (body.empty() || currentRoomId_.empty()) return;
        auto res = client_->send_reply(currentRoomId_, reply_event_id, body);
        if (!res) {
            statusBar()->showMessage(
                tr("Send reply failed: %1").arg(QString::fromStdString(res.message)), 4000);
            return;
        }
        if (composeTextArea_) composeTextArea_->set_text("");
        if (composeShared_)   composeShared_->set_current_text({});
    };

    vLayout->addWidget(composeSurface_);

    // Emoji picker: build the floating panel, wire selection → cursor
    // insert + account-data bump. Recents live in the SDK now (synced via
    // `io.element.recent_emoji`), so no local-disk load is needed.
    emojiPicker_ = new EmojiPicker(this);
    emojiPicker_->setClient(client_);
    emojiPicker_->onSelected = [this](const QString& glyph) {
        // Reaction mode: a message's "+" chip set pendingReactionEventId_
        // before opening the picker. Route the glyph through
        // send_reaction (toggle semantics handled Rust-side) and skip
        // the compose-bar insert.
        if (!pendingReactionEventId_.empty()) {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            if (!currentRoomId_.empty()) {
                client_->send_reaction(currentRoomId_, ev, glyph.toStdString());
            }
            client_->recent_emoji_bump(glyph.toStdString());
            emojiPicker_->hide();
            return;
        }
        if (!composeTextArea_) return;
        std::string cur = composeTextArea_->text();
        cur += glyph.toStdString();
        composeTextArea_->set_text(cur);
        if (composeShared_) composeShared_->set_current_text(cur);
        composeTextArea_->set_focused(true);
        client_->recent_emoji_bump(glyph.toStdString());
    };

    // Sticker picker: floating panel anchored at the compose-bar sticker
    // button. On selection, send `m.sticker` to the current room (matrix-
    // sdk encrypts transparently in E2EE rooms).
    stickerPicker_ = new StickerPicker(this);
    stickerPicker_->setClient(client_);
    stickerPicker_->onSelected =
        [this](const tesseract::ImagePackImage& img) {
            if (currentRoomId_.empty()) return;
            std::string body = img.body.empty() ? img.shortcode : img.body;
            client_->send_sticker(currentRoomId_, body, img.url, img.info_json);
            stickerPicker_->hide();
        };

    // Reaction-chip click: toggle (Rust handles add/remove).
    messageListView_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            if (currentRoomId_.empty()) return;
            client_->send_reaction(currentRoomId_, event_id, key);
        };

    // "+" pseudo-chip click: open the emoji picker in reaction mode.
    messageListView_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor) {
            if (!emojiPicker_ || currentRoomId_.empty()) return;
            pendingReactionEventId_ = event_id;
            // anchor is in MessageListView-local coords; the view is the
            // root of msgSurface_, so the rect maps directly to surface
            // widget coords.
            emojiPicker_->popupAtRect(msgSurface_, anchor);
        };

    // "↩" hover button → enter reply mode in the compose bar.
    messageListView_->on_reply_requested =
        [this](const std::string& event_id,
               const std::string& sender_name,
               const std::string& body_preview) {
            if (!composeShared_) return;
            composeShared_->set_reply_to(event_id, sender_name, body_preview);
            if (composeTextArea_) composeTextArea_->set_focused(true);
        };

    // "✏" hover button → enter edit mode in the compose bar.
    messageListView_->on_edit_requested =
        [this](const std::string& event_id, const std::string& current_body) {
            if (!composeShared_) return;
            composeShared_->set_editing(event_id);
            if (composeTextArea_) {
                composeTextArea_->set_text(current_body);
                composeShared_->set_current_text(current_body);
                composeTextArea_->set_focused(true);
            }
        };

    composeShared_->on_send_edit = [this](const std::string& event_id,
                                           const std::string& new_body) {
        if (new_body.empty() || currentRoomId_.empty()) return;
        auto res = client_->send_edit(currentRoomId_, event_id, new_body);
        if (!res) {
            statusBar()->showMessage(
                tr("Edit failed: %1").arg(QString::fromStdString(res.message)), 4000);
            return;
        }
        if (composeTextArea_) composeTextArea_->set_text("");
        if (composeShared_)   composeShared_->set_current_text({});
    };

    composeShared_->on_edit_cancelled = [this] {
        if (composeTextArea_) composeTextArea_->set_text("");
        if (composeShared_)   composeShared_->set_current_text({});
    };

    statusBar()->showMessage(tr("Not logged in"));
    // Room selection is delivered through RoomListView's on_room_selected
    // callback wired in the surface-construction block above.

    // Bridges live in `accounts_[].bridge` and are wired through
    // `wireBridge` when an account is attached. Slots filter on `sender()`
    // so callbacks from inactive accounts don't reach the UI surfaces.

    // Notifiers are created per-account in doLogin / onLoginSucceeded.

    // Animation frame-tick for inline media in the timeline (GIF /
    // animated WebP / APNG). 60 Hz; the timer self-stops in
    // `onMessageAnimTick_` when `tk_anim_images_` empties.
    tk_anim_timer_ = new QTimer(this);
    tk_anim_timer_->setInterval(16);
    connect(tk_anim_timer_, &QTimer::timeout,
            this, &MainWindow::onMessageAnimTick_);

    // Worker-thread media fetches bounce back through this queued
    // connection — see `requestRoomAvatar_` / `requestUserAvatar_` /
    // `requestMediaImage_`.
    connect(this, &MainWindow::mediaBytesLoaded_,
            this, &MainWindow::onMediaBytesLoaded_,
            Qt::QueuedConnection);

    // Back-pagination on scroll-to-top. The shared MessageListView fires
    // this once per crossing of the near-top threshold; the latch is
    // re-armed automatically after prepended rows are spliced in.
    messageListView_->on_near_top = [this]{
        if (currentRoomId_.empty()) return;
        requestMoreHistory(currentRoomId_);
    };

    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    // Drain background workers BEFORE tearing the clients down.  Workers
    // call `client_->paginate_back_with_status` which does a tokio block_on
    // over a network round-trip; we must not destroy the Runtime while that
    // is in flight (causes panic_in_cleanup through cxx's prevent_unwind).
    //
    // stop_sync() is called BEFORE waitForDone so the Rust-side select! in
    // paginate_back_with_status sees the cancellation signal and returns
    // immediately, unblocking any worker threads blocked inside it.
    // The ~ClientFfi destructor calls stop_sync() again as a no-op safety
    // net; stop_sync() is idempotent (handler.take() returns None on the
    // second call). Calling it here while the bridges are still alive also
    // ensures the final session-refresh callback can reach the handler.
    shuttingDown_.store(true, std::memory_order_release);
    mediaPool_.clear();
    for (auto& a : accounts_) {
        if (a && a->client) a->client->stop_sync();
    }
    if (pending_login_client_) pending_login_client_->stop_sync();
    mediaPool_.waitForDone(5000); // 5 s safety-net; matches GTK4 / Win32

    client_ = nullptr;
    bridge_ = nullptr;

    // LoginView is a child widget, normally destroyed during ~QMainWindow
    // — but that runs *after* the AccountSessions (and thus their
    // Clients) are destroyed, and ~LoginView calls cancel_oauth on its
    // bound client. Tear it down here while everything is still alive.
    delete loginView_;
    loginView_ = nullptr;
}

void MainWindow::runOnPool_(std::function<void()> fn) {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    auto* runner = QRunnable::create([this, fn = std::move(fn)]() mutable {
        if (shuttingDown_.load(std::memory_order_acquire)) return;
        fn();
    });
    mediaPool_.start(runner);
}

// ---------------------------------------------------------------------------

// eventFilter is defined further down (multi-account section) so it can
// dispatch user-strip left-clicks into the AccountPicker popover.

void MainWindow::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Escape) {
        if (vidViewer_ && vidViewer_->is_open()) { vidViewer_->close(); ev->accept(); return; }
        if (imgViewer_ && imgViewer_->is_open()) { imgViewer_->close(); ev->accept(); return; }
    }
    QMainWindow::keyPressEvent(ev);
}

void MainWindow::resizeEvent(QResizeEvent* ev) {
    QMainWindow::resizeEvent(ev);
    if (imgViewerHost_ && mainContent_)
        imgViewerHost_->setGeometry(mainContent_->rect());
    if (vidViewerHost_ && mainContent_)
        vidViewerHost_->setGeometry(mainContent_->rect());
}

// ---------------------------------------------------------------------------

void MainWindow::doLogin() {
    // One-shot migration from the legacy single-account layout. Runs once
    // per install before any Client is constructed; idempotent on every
    // subsequent launch.
    tesseract::SessionStore::migrate_legacy_layout();

    // Restore every account on disk, in index order, so notifications
    // fire for any of them while the user works in the foreground one.
    auto idx = tesseract::SessionStore::load_index();

    if (idx.user_ids.empty()) {
        // Fresh install: no accounts → initial LoginView, no Cancel.
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_client_ = std::make_unique<tesseract::Client>();
        pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
            "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
        std::error_code ec;
        std::filesystem::create_directories(pending_login_temp_dir_, ec);
        pending_login_client_->set_data_dir(
            (pending_login_temp_dir_ / "matrix-store").string());
        loginView_->set_client(pending_login_client_.get());
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Not logged in"));
        return;
    }

    statusBar()->showMessage(tr("Restoring sessions\xe2\x80\xa6"));

    int target_active = -1;
    for (std::size_t i = 0; i < idx.user_ids.size(); ++i) {
        const auto& uid = idx.user_ids[i];
        auto json = tesseract::SessionStore::load_account(uid);
        if (!json) continue;

        auto session = std::make_unique<tesseract::AccountSession>();
        session->user_id = uid;
        session->client  = std::make_unique<tesseract::Client>();
        session->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());

        auto res = session->client->restore_session(*json);
        if (!res) {
            // This account's session is bad — wipe it from disk so the
            // next launch doesn't keep retrying.
            tesseract::SessionStore::clear_account(uid);
            statusBar()->showMessage(
                tr("Account %1 expired: %2")
                    .arg(QString::fromStdString(uid),
                         QString::fromStdString(res.message)),
                6000);
            continue;
        }
        session->display_name = session->client->get_display_name();
        session->avatar_url   = session->client->get_avatar_url();
        session->last_room    = tesseract::Prefs::parse(
            session->client->load_prefs_json()).last_room;

        auto bridge = std::make_unique<EventBridge>(this);
        bridge->set_user_id(uid);
        wireBridge(bridge.get());
        session->client->start_sync(bridge.get());
        session->sync_started = true;
        session->bridge = std::move(bridge);

        // Per-account notifier: click switches to this account then navigates.
        session->notifier = std::make_unique<LinuxNotifierQt>(
            [this, uid](std::string room_id) {
                for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                    if (accounts_[i]->user_id == uid) { switchActiveAccount(i); break; }
                }
                navigate_to_room(std::move(room_id));
            });

        if (uid == idx.active_user_id) target_active = static_cast<int>(accounts_.size());
        accounts_.push_back(std::move(session));
    }

    if (accounts_.empty()) {
        // Every stored account failed to restore — drop to initial login.
        tesseract::SessionStore::save_index({});
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_client_ = std::make_unique<tesseract::Client>();
        pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
            "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
        std::error_code ec;
        std::filesystem::create_directories(pending_login_temp_dir_, ec);
        pending_login_client_->set_data_dir(
            (pending_login_temp_dir_ / "matrix-store").string());
        loginView_->set_client(pending_login_client_.get());
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Not logged in"));
        return;
    }

    if (target_active < 0) target_active = 0;
    switchActiveAccount(target_active);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainContent_);
    maybeShowRecoveryBanner();

    if (!tray_) {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]{ show(); raise(); activateWindow(); },
            [this]{ if (isVisible()) hide(); else { show(); raise(); activateWindow(); } },
            []{ qApp->quit(); },
            this);
        if (tray_->is_available())
            qApp->setQuitOnLastWindowClosed(false);
    }
}

void MainWindow::onLoginSucceeded() {
    if (!pending_login_client_) return;   // defensive — should not happen

    // The OAuth round-trip completed on `pending_login_client_`, which the
    // LoginView drove against a temporary data dir
    // (`pending_login_temp_dir_/matrix-store`). We don't yet know the
    // user_id; ask the client now.
    std::string user_id = pending_login_client_->get_user_id();
    if (user_id.empty()) {
        statusBar()->showMessage(tr("Sign-in failed: no user id"), 6000);
        return;
    }

    // If an account with the same user_id is already signed in (the user
    // added an account they're already logged into), refuse rather than
    // colliding on disk.
    for (auto& a : accounts_) {
        if (a->user_id == user_id) {
            statusBar()->showMessage(
                tr("Already signed in as %1")
                    .arg(QString::fromStdString(user_id)),
                4000);
            pending_login_client_.reset();
            loginView_->set_client(nullptr);
            std::error_code ec;
            std::filesystem::remove_all(pending_login_temp_dir_, ec);
            // Restore previous active account's UI.
            if (add_account_return_idx_ >= 0
                && add_account_return_idx_ < static_cast<int>(accounts_.size())) {
                switchActiveAccount(add_account_return_idx_);
                contentStack_->setCurrentWidget(mainContent_);
            }
            pending_login_is_add_account_ = false;
            add_account_return_idx_ = -1;
            return;
        }
    }

    // Snapshot the session blob before we drop the in-flight Client —
    // re-opening it below needs to restore from this JSON.
    auto session_json = pending_login_client_->export_session();
    if (session_json.empty()) {
        statusBar()->showMessage(tr("Sign-in failed: empty session"), 6000);
        return;
    }

    // Drop the in-flight Client so its SQLite handles are released
    // before we rename the directory underneath it.
    pending_login_client_.reset();
    loginView_->set_client(nullptr);

    // Move the temp account directory into its final per-user-id home.
    std::filesystem::path final_dir = tesseract::SessionStore::account_dir(user_id);
    {
        std::error_code ec;
        std::filesystem::create_directories(final_dir.parent_path(), ec);
        std::filesystem::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec) {
            // Rename failed — try recursive copy + remove, falling back
            // to leaving the data in the temp dir if even that fails.
            std::error_code ec2;
            std::filesystem::copy(pending_login_temp_dir_, final_dir,
                std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::overwrite_existing, ec2);
            if (ec2) {
                statusBar()->showMessage(
                    tr("Sign-in failed: couldn't persist matrix store: %1")
                        .arg(QString::fromStdString(ec2.message())),
                    6000);
                return;
            }
            std::filesystem::remove_all(pending_login_temp_dir_, ec2);
        }
    }
    pending_login_temp_dir_.clear();

    // Persist the session blob into the final per-account dir.
    if (!tesseract::SessionStore::save_account(user_id, session_json)) {
        statusBar()->showMessage(tr("Sign-in failed: couldn't persist session"), 6000);
        return;
    }

    // Open a fresh Client against the final store path and restore from
    // the just-exported session JSON (the matrix-sdk reuses the moved
    // SQLite store transparently — no resync).
    auto session = std::make_unique<tesseract::AccountSession>();
    session->user_id = user_id;
    session->client  = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    auto res = session->client->restore_session(session_json);
    if (!res) {
        statusBar()->showMessage(
            tr("Sign-in failed at restore: %1")
                .arg(QString::fromStdString(res.message)),
            6000);
        tesseract::SessionStore::clear_account(user_id);
        return;
    }
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    session->last_room    = tesseract::Prefs::parse(
        session->client->load_prefs_json()).last_room;

    auto bridge = std::make_unique<EventBridge>(this);
    bridge->set_user_id(user_id);
    wireBridge(bridge.get());
    session->client->start_sync(bridge.get());
    session->sync_started = true;
    session->bridge = std::move(bridge);

    // Per-account notifier: click switches to this account then navigates.
    session->notifier = std::make_unique<LinuxNotifierQt>(
        [this, uid = user_id](std::string room_id) {
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
                if (accounts_[i]->user_id == uid) { switchActiveAccount(i); break; }
            }
            navigate_to_room(std::move(room_id));
        });

    int new_idx = static_cast<int>(accounts_.size());
    accounts_.push_back(std::move(session));

    // Update the on-disk index. Active = the account we just added.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = user_id;
    for (auto& a : accounts_) idx.user_ids.push_back(a->user_id);
    tesseract::SessionStore::save_index(idx);

    switchActiveAccount(new_idx);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainContent_);
    maybeShowRecoveryBanner();

    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;

    if (!tray_) {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]{ show(); raise(); activateWindow(); },
            [this]{ if (isVisible()) hide(); else { show(); raise(); activateWindow(); } },
            []{ qApp->quit(); },
            this);
        if (tray_->is_available())
            qApp->setQuitOnLastWindowClosed(false);
    }
}

void MainWindow::onLoginCancelled() {
    // The user clicked Cancel during AddAccount. Return to the previous
    // foreground account; the in-flight client is discarded.
    pending_login_client_.reset();
    loginView_->set_client(nullptr);
    if (!pending_login_is_add_account_) return;   // no back-state in Initial mode

    int back = add_account_return_idx_;
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
    if (back >= 0 && back < static_cast<int>(accounts_.size())) {
        switchActiveAccount(back);
        contentStack_->setCurrentWidget(mainContent_);
    }
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    if (tray_ && tray_->is_available()) {
        ev->ignore();
        hide();
        return;
    }
    QMainWindow::closeEvent(ev);
}

void MainWindow::navigate_to_room(const std::string& room_id) {
    if (room_id.empty()) return;
    if (roomListView_) roomListView_->set_selected_room(room_id);
    onRoomSelected(room_id);
    raise();
    activateWindow();
}

void MainWindow::onNotificationTriggered(
        QString roomId, QString roomName, QString sender,
        QString body, bool is_mention)
{
    const std::string rid = roomId.toStdString();
    // Find the account that owns this notification via the emitting bridge.
    auto* b = qobject_cast<EventBridge*>(QObject::sender());
    const std::string uid = b ? b->user_id() : std::string{};
    for (auto& sess : accounts_) {
        if (sess->user_id != uid) continue;
        // Suppress only when this account is active and its room is open.
        if (isActiveWindow()
                && active_account_index_ >= 0
                && accounts_[active_account_index_]->user_id == uid
                && currentRoomId_ == rid)
            return;
        if (sess->notifier)
            sess->notifier->notify({ rid, roomName.toStdString(),
                                     sender.toStdString(), body.toStdString(),
                                     is_mention });
        return;
    }
}

void MainWindow::onSendClicked() {
    if (composeShared_) composeShared_->trigger_send();
}

void MainWindow::onRoomSelected(const std::string& room_id) {
    if (room_id.empty()) return;

    // Drill into a space if the clicked row is one.
    for (const auto& r : rooms_) {
        if (r.id == room_id && r.is_space) {
            spaceStack_.push_back(room_id);
            refreshRoomList();
            return;
        }
    }

    if (!currentRoomId_.empty() && currentRoomId_ != room_id)
        client_->unsubscribe_room(currentRoomId_);

    currentRoomId_ = room_id;
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = room_id;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (composeShared_) {
        composeShared_->clear_reply();
        composeShared_->clear_editing();
    }

    for (const auto& r : rooms_)
        if (r.id == currentRoomId_) { updateRoomHeader(r); break; }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the UI stays responsive during the
    // first-load network round-trip.
    std::string sub_room = currentRoomId_;
    runOnPool_([this, sub_room]{
        auto res = client_->subscribe_room(sub_room);
        bool ok  = res.ok;
        std::string msg = res.message;
        bool reached = false;
        if (ok) {
            auto pr = client_->paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            client_->start_background_backfill();
        }
        QMetaObject::invokeMethod(
            this,
            [this, sub_room, ok, msg = std::move(msg), reached]() mutable {
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Subscribe failed: %1").arg(
                            QString::fromStdString(msg)), 4000);
                    return;
                }
                if (currentRoomId_ == sub_room) {
                    auto& state = pagination_[sub_room];
                    state.in_flight     = false;
                    state.reached_start = reached;
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::onTimelineReset(
    QString roomId, std::vector<tesseract::Event*> snapshot)
{
    // Filter: only the active account drives the message list. Background
    // accounts deliver their timeline events too (they're all syncing), but
    // the message list always shows the foreground room.
    const bool from_active = (sender() == bridge_);
    const bool current = from_active && roomId.toStdString() == currentRoomId_;
    if (current) {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto* ev : snapshot) {
            if (!ev) continue;
            ensureRowMedia(*ev);
            ensureReplyDetails(ev->in_reply_to_id);
            rows.push_back(toRowData(*ev));
        }
        messageListView_->set_messages(std::move(rows));
        msgSurface_->relayout();
    }
    for (auto* ev : snapshot) delete ev;
}

void MainWindow::onMessageInserted(
    QString roomId, std::size_t index, tesseract::Event* ev)
{
    const bool from_active = (sender() == bridge_);
    if (ev && from_active && roomId.toStdString() == currentRoomId_
        && ev->type != tesseract::EventType::Unhandled)
    {
        ensureRowMedia(*ev);
        ensureReplyDetails(ev->in_reply_to_id);
        messageListView_->insert_message(index, toRowData(*ev));
        msgSurface_->relayout();
    }
    delete ev;
}

void MainWindow::onMessageUpdated(
    QString roomId, std::size_t index, tesseract::Event* ev)
{
    const bool from_active = (sender() == bridge_);
    if (ev && from_active && roomId.toStdString() == currentRoomId_
        && ev->type != tesseract::EventType::Unhandled)
    {
        ensureRowMedia(*ev);
        ensureReplyDetails(ev->in_reply_to_id);
        messageListView_->update_message(index, toRowData(*ev));
        msgSurface_->relayout();
    }
    delete ev;
}

void MainWindow::onMessageRemoved(QString roomId, std::size_t index) {
    if (sender() != bridge_) return;
    if (roomId.toStdString() != currentRoomId_) return;
    messageListView_->remove_message(index);
    msgSurface_->relayout();
}

void MainWindow::requestMoreHistory(const std::string& room_id) {
    if (room_id.empty()) return;
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    // Run the blocking SDK call off the UI thread; bounce the result back
    // via a queued connection. `client_` is thread-safe (Rust runtime
    // serialises concurrent calls).
    runOnPool_([this, room_id]{
        auto res = client_->paginate_back_with_status(room_id, kPaginationBatch);
        bool reached = res.ok && res.reached_start;
        QMetaObject::invokeMethod(
            this,
            "onPaginateFinished",
            Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(room_id)),
            Q_ARG(bool, reached));
    });
}

void MainWindow::onPaginateFinished(QString roomId, bool reached_start) {
    auto it = pagination_.find(roomId.toStdString());
    if (it == pagination_.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached_start;
    // Re-arm the near-top latch so the user's next scroll-up can trigger
    // another page. If the prepended rows already triggered a relayout,
    // `arrange()` will have reset the latch too — calling here is cheap
    // and idempotent.
    if (roomId.toStdString() == currentRoomId_ && messageListView_)
        messageListView_->reset_near_top_latch();
}

void MainWindow::onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms) {
    // Identify the source account from the sender bridge so we can cache
    // the snapshot under the right user_id even when the callback comes
    // from a background account. Looking up by pointer keeps this O(N)
    // worst case in account count, which is tiny.
    auto* sender_bridge = qobject_cast<EventBridge*>(sender());
    std::string source_uid;
    if (sender_bridge) source_uid = sender_bridge->user_id();

    if (!source_uid.empty())
        per_account_rooms_[source_uid] = rooms;

    if (sender_bridge != bridge_) return;   // background account → cache only

    rooms_ = std::move(rooms);
    refreshRoomList();
    if (!currentRoomId_.empty()) {
        for (const auto& r : rooms_)
            if (r.id == currentRoomId_) { updateRoomHeader(r); break; }
    } else if (!pendingRestoreRoom_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == pendingRestoreRoom_ && !r.is_space) {
                std::string target = std::move(pendingRestoreRoom_);
                pendingRestoreRoom_.clear();
                onRoomSelected(target);
                break;
            }
        }
    }
}

void MainWindow::onSyncError(
    QString context, QString description, bool soft_logout)
{
    // Identify which account fired the error. With multi-account we have
    // N concurrent syncs; a soft-logout on one shouldn't yank a different
    // account's session.
    auto* sender_bridge = qobject_cast<EventBridge*>(sender());
    tesseract::AccountSession* affected = nullptr;
    if (sender_bridge) {
        for (auto& a : accounts_) {
            if (static_cast<EventBridge*>(a->bridge.get()) == sender_bridge) {
                affected = a.get(); break;
            }
        }
    }

    if (context == "sync_reconnect") {
        statusBar()->showMessage(tr("Sync error: reconnecting\xe2\x80\xa6"));
        if (affected && affected->client) affected->client->stop_sync();
        doLogin();
    } else if (context == "sync_auth_error") {
        if (soft_logout && affected) {
            if (auto saved = tesseract::SessionStore::load_account(affected->user_id)) {
                statusBar()->showMessage(tr("Reconnecting session\xe2\x80\xa6"));
                if (affected->client->restore_session(*saved)) {
                    affected->display_name = affected->client->get_display_name();
                    affected->avatar_url   = affected->client->get_avatar_url();
                    if (affected == accounts_[std::max(0, active_account_index_)].get())
                        switchActiveAccount(active_account_index_);
                    affected->client->start_sync(static_cast<EventBridge*>(affected->bridge.get()));
                    statusBar()->showMessage(tr("Reconnected"));
                    maybeShowRecoveryBanner();
                    return;
                }
            }
        }
        if (affected) {
            tesseract::SessionStore::clear_account(affected->user_id);
            affected->client->stop_sync();
        }
        statusBar()->showMessage(tr("Session expired; please log in again."));
        doLogin();
    } else {
        statusBar()->showMessage(tr("Sync error: %1").arg(description), 8000);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::updateRoomHeader(const tesseract::RoomInfo& info) {
    roomHeaderName_->setText(QString::fromStdString(info.name));

    if (!info.topic.empty()) {
        currentTopicText_ = QString::fromStdString(info.topic);
        roomHeaderTopic_->setToolTip(currentTopicText_);
        updateTopicElision();
        roomHeaderTopic_->setVisible(true);
        roomHeader_->setFixedHeight(68);
    } else {
        currentTopicText_.clear();
        roomHeaderTopic_->setToolTip({});
        roomHeaderTopic_->setVisible(false);
        roomHeader_->setFixedHeight(60);
    }

    QPixmap pm;
    if (!info.avatar_url.empty()) {
        QString qurl = QString::fromStdString(info.avatar_url);
        if (!avatarCache_.contains(qurl)) {
            auto bytes = client_->fetch_avatar_bytes(info.id);
            if (!bytes.empty()) {
                QPixmap raw;
                raw.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                 static_cast<uint>(bytes.size()));
                if (!raw.isNull())
                    avatarCache_[qurl] = raw.scaled(
                        kRoomAvatarSize, kRoomAvatarSize,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        if (avatarCache_.contains(qurl))
            pm = avatarCache_[qurl];
    }
    if (pm.isNull())
        pm = makeInitialsPixmap(QString::fromStdString(info.name), 40);
    else
        pm = makeCirclePixmap(pm, 40);

    roomHeaderAvatar_->setPixmap(pm);
    roomHeader_->setVisible(true);
}

void MainWindow::updateTopicElision() {
    if (currentTopicText_.isEmpty()) return;
    int w = roomHeaderTopic_->width();
    if (w <= 0) return;
    roomHeaderTopic_->setText(
        QFontMetrics(roomHeaderTopic_->font())
            .elidedText(currentTopicText_, Qt::ElideRight, w));
}

void MainWindow::clearMessages() {
    messageListView_->set_messages({});
    msgSurface_->relayout();
}

// These three helpers used to call the synchronous Rust FFI on the UI
// thread. `fetch_avatar_bytes` / `fetch_media_bytes` do a
// `tokio::block_on` inside; on first sync of an account with many rooms
// `showRooms` froze the event loop for minutes (one network round-trip
// per room avatar, serialised on the UI thread). Decode + cache now
// happens via `mediaBytesLoaded_` after a `QThreadPool` worker landed
// the bytes; the call sites return immediately and the views paint
// initials placeholders until the bytes land.
void MainWindow::ensureRoomAvatar(const tesseract::RoomInfo& r) {
    requestRoomAvatar_(r.id, r.avatar_url);
}

void MainWindow::ensureUserAvatar(const std::string& mxc) {
    requestUserAvatar_(mxc);
}

void MainWindow::ensureMediaImage(const std::string& url, int max_w, int max_h) {
    requestMediaImage_(url, max_w, max_h);
}

void MainWindow::ensureReplyDetails(const std::string& event_id) {
    if (event_id.empty() || currentRoomId_.empty()) return;
    if (!reply_details_requested_.insert(event_id).second) return;
    client_->fetch_reply_details(currentRoomId_, event_id);
}

void MainWindow::requestRoomAvatar_(const std::string& room_id,
                                      const std::string& mxc) {
    if (room_id.empty() || mxc.empty()) return;
    if (tk_avatars_.count(mxc)) return;
    if (!mediaFetchesInFlight_.insert(mxc).second) return;

    QString qkey = QString::fromStdString(mxc);
    runOnPool_([this, room_id, qkey]() {
        auto bytes = client_->fetch_avatar_bytes(room_id);
        QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<int>(bytes.size()));
        emit mediaBytesLoaded_(qkey,
                                static_cast<int>(MediaKind::RoomAvatar),
                                qb);
    });
}

void MainWindow::requestUserAvatar_(const std::string& mxc) {
    if (mxc.empty()) return;
    if (tk_avatars_.count(mxc)) return;
    if (!mediaFetchesInFlight_.insert(mxc).second) return;

    QString qkey = QString::fromStdString(mxc);
    runOnPool_([this, key = mxc, qkey]() {
        auto bytes = client_->fetch_media_bytes(key);
        QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<int>(bytes.size()));
        emit mediaBytesLoaded_(qkey,
                                static_cast<int>(MediaKind::UserAvatar),
                                qb);
    });
}

void MainWindow::requestMediaImage_(const std::string& url,
                                      int max_w, int max_h) {
    if (url.empty()) return;
    if (tk_images_.count(url) || tk_anim_images_.count(url)) return;
    if (!mediaFetchesInFlight_.insert(url).second) return;
    mediaImageSizes_[url] = { max_w, max_h };

    QString qkey = QString::fromStdString(url);
    runOnPool_([this, key = url, qkey]() {
        // `key` may be plain mxc (plain images/stickers) or a JSON
        // MediaSource (encrypted images/stickers + reaction sources).
        // `fetch_source_bytes` handles both shapes; `fetch_media_bytes`
        // only handles plain mxc and would return empty for encrypted.
        auto bytes = client_->fetch_source_bytes(key);
        QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<int>(bytes.size()));
        emit mediaBytesLoaded_(qkey,
                                static_cast<int>(MediaKind::MediaImage),
                                qb);
    });
}

void MainWindow::onMediaBytesLoaded_(QString cache_key, int kind,
                                       QByteArray bytes) {
    std::string key = cache_key.toStdString();
    mediaFetchesInFlight_.erase(key);
    if (bytes.isEmpty()) {
        mediaImageSizes_.erase(key);
        return;
    }

    const MediaKind k = static_cast<MediaKind>(kind);
    if (k == MediaKind::RoomAvatar || k == MediaKind::UserAvatar) {
        if (tk_avatars_.count(key)) return;
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.constData()),
                              bytes.size()))
            return;
        const int size = (k == MediaKind::RoomAvatar)
                          ? kRoomAvatarSize : kMsgAvatarSize;
        QImage scaled = img.scaled(size, size,
                                    Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        tk_avatars_.emplace(key, tk::qt6::make_image(std::move(scaled)));
        if (k == MediaKind::RoomAvatar) {
            if (roomSurface_) roomSurface_->update();
        } else {
            if (msgSurface_) msgSurface_->update();
        }
        return;
    }

    // MediaImage — animated probe first, then static fallback.
    if (tk_images_.count(key) || tk_anim_images_.count(key)) {
        mediaImageSizes_.erase(key);
        return;
    }
    int max_w = kMaxImageWidth, max_h = kMaxImageHeight;
    if (auto sit = mediaImageSizes_.find(key); sit != mediaImageSizes_.end()) {
        max_w = sit->second.first;
        max_h = sit->second.second;
        mediaImageSizes_.erase(sit);
    }

    QBuffer buf(&bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    if (reader.supportsAnimation() && reader.imageCount() > 1) {
        AnimatedImage entry;
        entry.frames.reserve(reader.imageCount());
        entry.delays_ms.reserve(reader.imageCount());
        QImage frame;
        while (reader.read(&frame)) {
            int delay = reader.nextImageDelay();
            if (delay <= 0)   delay = 100;
            if (delay < 20)   delay = 20;
            QImage scaled = frame.scaled(max_w, max_h,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            entry.frames.push_back(tk::qt6::make_image(std::move(scaled)));
            entry.delays_ms.push_back(delay);
        }
        if (!entry.frames.empty()) {
            entry.current         = 0;
            entry.next_advance_ms = QDateTime::currentMSecsSinceEpoch()
                                  + entry.delays_ms[0];
            tk_anim_images_.emplace(key, std::move(entry));
            if (tk_anim_timer_ && !tk_anim_timer_->isActive())
                tk_anim_timer_->start();
            if (msgSurface_) msgSurface_->update();
            return;
        }
        buf.seek(0);
    }

    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.constData()),
                          bytes.size()))
        return;
    QImage scaled = img.scaled(max_w, max_h,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    tk_images_.emplace(key, tk::qt6::make_image(std::move(scaled)));
    if (msgSurface_) msgSurface_->update();
}

void MainWindow::onMessageAnimTick_() {
    if (tk_anim_images_.empty()) {
        if (tk_anim_timer_) tk_anim_timer_->stop();
        return;
    }
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    bool any_changed = false;
    for (auto& [_, entry] : tk_anim_images_) {
        if (entry.frames.size() <= 1) continue;
        std::size_t steps = 0;
        while (now >= entry.next_advance_ms
                && steps < entry.frames.size())
        {
            entry.current = (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms += entry.delays_ms[entry.current];
            ++steps;
        }
        if (steps > 0) any_changed = true;
    }
    if (any_changed && msgSurface_) msgSurface_->update();
}

void MainWindow::showRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(r);

    // Eagerly fetch avatars for the new room set so the first paint has
    // them ready. Bytes-already-cached is a no-op via tk_avatars_.count.
    for (const auto& r : sorted) ensureRoomAvatar(r);

    roomListView_->set_rooms(std::move(sorted));
    if (!currentRoomId_.empty())
        roomListView_->set_selected_room(currentRoomId_);
    roomSurface_->relayout();
}

void MainWindow::refreshRoomList() {
    if (spaceStack_.empty()) {
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
            if ( r.is_space) filtered.push_back(r);
        showRooms(filtered);
        roomNavBar_->setVisible(false);
    } else {
        const std::string& space_id = spaceStack_.back();
        auto child_ids = client_->space_children(space_id);
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
            if (std::find(child_ids.begin(), child_ids.end(), r.id) != child_ids.end())
                filtered.push_back(r);
        showRooms(filtered);
        for (const auto& r : rooms_)
            if (r.id == space_id) {
                spaceNameLabel_->setText(QString::fromStdString(r.name));
                break;
            }
        roomNavBar_->setVisible(true);
    }
}

void MainWindow::onSpaceBack() {
    if (!spaceStack_.empty()) spaceStack_.pop_back();
    refreshRoomList();
}

// ---------------------------------------------------------------------------

tesseract::views::MessageRowData MainWindow::toRowData(const tesseract::Event& ev) {
    using Kind = tesseract::views::MessageRowData::Kind;
    tesseract::views::MessageRowData row;
    row.event_id          = ev.event_id;
    row.sender            = ev.sender;
    row.sender_name       = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body              = ev.body;
    row.timestamp_ms      = ev.timestamp;
    row.is_own            = (ev.sender == myUserId_);
    row.reactions         = ev.reactions;
    row.read_receipts     = ev.read_receipts;

    row.in_reply_to_id          = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    row.in_reply_to_body        = ev.in_reply_to_body;
    row.is_edited               = ev.is_edited;

    switch (ev.type) {
        case tesseract::EventType::Text:
            row.kind = Kind::Text;
            break;
        case tesseract::EventType::Image: {
            row.kind = Kind::Image;
            const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
            row.media_url            = img.image_url;
            row.media_w              = static_cast<int>(img.width);
            row.media_h              = static_cast<int>(img.height);
            row.has_filename_caption = !img.filename.empty();
            break;
        }
        case tesseract::EventType::Sticker: {
            row.kind = Kind::Sticker;
            const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
            row.media_url = s.image_url;
            row.media_w   = static_cast<int>(s.width);
            row.media_h   = static_cast<int>(s.height);
            break;
        }
        case tesseract::EventType::File: {
            row.kind = Kind::File;
            const auto& f = static_cast<const tesseract::FileEvent&>(ev);
            row.file_name = f.file_name;
            row.file_size = f.file_size;
            row.media_url = f.file_url;
            break;
        }
        case tesseract::EventType::Voice: {
            row.kind = Kind::Voice;
            const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
            row.audio_source = v.audio_source;
            row.audio_mime   = v.mime_type;
            row.duration_ms  = v.duration_ms;
            row.waveform     = v.waveform;
            break;
        }
        case tesseract::EventType::Video: {
            row.kind = Kind::Video;
            const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
            row.media_url         = vid.video_url;
            // Use server thumbnail when available; otherwise fall back to the
            // client-generated key so the image provider will find the frame
            // once ensureRowMedia() finishes generating it.
            row.video_thumb_url   = vid.thumbnail_url.empty()
                                    ? ("thumb::" + ev.event_id)
                                    : vid.thumbnail_url;
            row.media_w           = static_cast<int>(vid.width);
            row.media_h           = static_cast<int>(vid.height);
            row.duration_ms       = vid.duration_ms;
            row.has_filename_caption = !vid.filename.empty();
            break;
        }
        case tesseract::EventType::Redacted:
            row.kind = Kind::Redacted;
            break;
        case tesseract::EventType::Unhandled:
            row.kind = Kind::Unhandled;
            break;
    }
    return row;
}

void MainWindow::ensureRowMedia(const tesseract::Event& ev) {
    // Fetch + decode any media the row references. The shared
    // MessageListView reads from tk_avatars_ / tk_images_ via provider
    // lambdas wired in the constructor.
    ensureUserAvatar(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts) {
        ensureUserAvatar(rr.avatar_url);
    }
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        ensureMediaImage(img.image_url, kMaxImageWidth, kMaxImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        ensureMediaImage(s.image_url, kMaxStickerSize, kMaxStickerSize);
    } else if (ev.type == tesseract::EventType::Voice) {
        const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            voice_prefetched_.insert(v.audio_source).second) {
            // Pull the clip into the SDK media cache on a worker so the
            // first play-button tap is instant. We discard the bytes —
            // the next synchronous `fetch_source_bytes` (driven by the
            // view) reads them straight back out of the cache.
            std::string src = v.audio_source;
            runOnPool_([this, src = std::move(src)]() {
                (void)client_->fetch_source_bytes(src);
            });
        }
    } else if (ev.type == tesseract::EventType::Video) {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        // Fetch server thumbnail when available.
        if (!vid.thumbnail_url.empty())
            ensureMediaImage(vid.thumbnail_url, kMaxImageWidth, kMaxImageHeight);
        // Client-side first-frame generation when no server thumbnail.
        if (vid.thumbnail_url.empty() && !vid.video_url.empty() &&
            video_thumb_in_flight_.insert(ev.event_id).second) {
            const std::string eid = ev.event_id;
            std::string src = vid.video_url;
            runOnPool_(
                [this, eid, src = std::move(src)]() {
                    auto bytes = client_->fetch_source_bytes(src);
                    if (bytes.empty()) return;
                    // Decode the first frame on the UI thread — Qt multimedia
                    // objects (QMediaPlayer, QVideoSink) must live there.
                    QMetaObject::invokeMethod(this,
                        [this, eid, bytes = std::move(bytes)]() mutable {
                            const std::string key = "thumb::" + eid;
                            if (tk_images_.count(key)) return;
                            auto* player = new QMediaPlayer(this);
                            auto* sink   = new QVideoSink(player);
                            player->setVideoSink(sink);
                            auto* buf = new QBuffer(player);
                            QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<qsizetype>(bytes.size()));
                            buf->setData(ba);
                            buf->open(QIODevice::ReadOnly);
                            player->setSourceDevice(buf);
                            QObject::connect(
                                sink, &QVideoSink::videoFrameChanged,
                                sink, [this, key, player](const QVideoFrame& frame) {
                                    if (!frame.isValid()) return;
                                    player->stop();
                                    player->deleteLater();
                                    if (tk_images_.count(key)) return;
                                    QImage img = frame.toImage();
                                    if (img.isNull()) return;
                                    QByteArray enc;
                                    QBuffer encbuf(&enc);
                                    encbuf.open(QIODevice::WriteOnly);
                                    img.save(&encbuf, "JPEG", 85);
                                    if (!enc.isEmpty())
                                        emit mediaBytesLoaded_(
                                            QString::fromStdString(key),
                                            static_cast<int>(MediaKind::MediaImage),
                                            enc);
                                });
                            player->play();
                        }, Qt::QueuedConnection);
                });
        }
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty())
            ensureMediaImage(r.source_json, 20, 20);
    }
}


// ---------------------------------------------------------------------------
// Recovery banner + dialog (Step 6)
// ---------------------------------------------------------------------------

void MainWindow::maybeShowRecoveryBanner() {
    if (recoveryBannerDismissed_) return;
    if (!client_->needs_recovery()) return;
    if (!recoverySurface_->isVisible()) {
        if (recoveryShared_) {
            recoveryShared_->set_state(
                tesseract::views::RecoveryBanner::State::Form);
            recoveryShared_->set_current_key("");
        }
        if (recoveryKeyField_) {
            recoveryKeyField_->set_text("");
            recoveryKeyField_->set_enabled(true);
        }
        recoverySurface_->setVisible(true);
        recoverySurface_->relayout();
    }
}

void MainWindow::onRecoveryVerifyClicked() {
    std::string key;
    if (recoveryKeyField_) key = recoveryKeyField_->text();
    // Trim whitespace.
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of (" \t\r\n");
    if (a == std::string::npos) {
        if (recoveryShared_) {
            recoveryShared_->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            recoveryShared_->set_failure_message(
                "Please enter a recovery key or passphrase.");
        }
        recoverySurface_->relayout();
        return;
    }
    key = key.substr(a, b - a + 1);

    if (recoveryShared_)
        recoveryShared_->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    if (recoveryKeyField_) recoveryKeyField_->set_enabled(false);
    recoverySurface_->relayout();

    runOnPool_([this, k = key]() {
        auto res = client_->recover(k);
        emit recoverFinished(res.ok, QString::fromStdString(res.message));
    });
}

void MainWindow::onRecoverFinished(bool ok, QString error) {
    if (ok) {
        if (recoveryShared_) {
            recoveryShared_->set_state(
                tesseract::views::RecoveryBanner::State::Importing);
        }
        recoverySurface_->relayout();
        return;
    }
    if (recoveryShared_) {
        recoveryShared_->set_state(
            tesseract::views::RecoveryBanner::State::Failed);
        recoveryShared_->set_failure_message(error.toStdString());
    }
    if (recoveryKeyField_) {
        recoveryKeyField_->set_enabled(true);
        recoveryKeyField_->set_focused(true);
    }
    recoverySurface_->relayout();
}

void MainWindow::onDismissRecoveryBanner() {
    recoveryBannerDismissed_ = true;
    if (recoverySurface_) recoverySurface_->setVisible(false);
}

void MainWindow::onBackupProgress(tesseract::BackupProgress progress) {
    if (sender() != bridge_) return;   // background-account backup state stays out of the active UI
    maybeShowRecoveryBanner();

    if (recoverySurface_ && recoverySurface_->isVisible()
        && recoveryShared_
        && recoveryShared_->state() ==
            tesseract::views::RecoveryBanner::State::Importing
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        recoveryShared_->set_import_progress(progress.imported_keys);
        recoverySurface_->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !client_->needs_recovery())
    {
        if (recoverySurface_) recoverySurface_->setVisible(false);
    }

    lastBackupState_  = progress.state;
    lastImportedKeys_ = progress.imported_keys;
    refreshSyncStatus();
}

void MainWindow::onRoomListStateChanged(std::uint8_t state) {
    if (sender() != bridge_) return;   // background-account sync state stays out of the status bar
    lastRoomListState_ = static_cast<tesseract::RoomListState>(state);
    refreshSyncStatus();
}

void MainWindow::refreshSyncStatus() {
    // Compose progress text for the status bar. Priority order:
    //   1. Init / SettingUp        → "Syncing rooms…" (debounced 300 ms)
    //   2. Recovering              → "Reconnecting…"
    //   3. backup Downloading      → "Downloading encryption keys (N)…"
    //   4. else                    → clear (restores prior "Connected" copy)
    //
    // Error / Terminated leave whatever the sync_error path already wrote.
    using RLS = tesseract::RoomListState;
    using BS  = tesseract::BackupState;

    const bool room_busy = (lastRoomListState_ == RLS::Init
                         || lastRoomListState_ == RLS::SettingUp);
    const bool reconnecting = (lastRoomListState_ == RLS::Recovering);
    const bool keys_busy = (lastBackupState_ == BS::Downloading);

    if (room_busy) {
        // Debounce: only show "Syncing rooms…" after 300 ms of being
        // non-Running, so already-cached sessions that flash through
        // Init→Running in a single tick don't churn the bar.
        if (!syncStatusDebounce_) {
            syncStatusDebounce_ = new QTimer(this);
            syncStatusDebounce_->setSingleShot(true);
            connect(syncStatusDebounce_, &QTimer::timeout, this, [this] {
                using RLS2 = tesseract::RoomListState;
                if (lastRoomListState_ == RLS2::Init
                 || lastRoomListState_ == RLS2::SettingUp) {
                    syncProgressShown_ = true;
                    statusBar()->showMessage(tr("Syncing rooms…"));
                }
            });
        }
        if (!syncStatusDebounce_->isActive() && !syncProgressShown_)
            syncStatusDebounce_->start(300);
        else if (syncProgressShown_)
            statusBar()->showMessage(tr("Syncing rooms…"));
        return;
    }

    if (syncStatusDebounce_) syncStatusDebounce_->stop();

    if (reconnecting) {
        syncProgressShown_ = true;
        statusBar()->showMessage(tr("Reconnecting…"));
        return;
    }
    if (keys_busy) {
        syncProgressShown_ = true;
        statusBar()->showMessage(
            tr("Downloading encryption keys (%1)…")
                .arg(static_cast<qulonglong>(lastImportedKeys_)));
        return;
    }
    if (syncProgressShown_) {
        // We covered up a prior login message; settle to the steady-state copy.
        syncProgressShown_ = false;
        statusBar()->showMessage(tr("Connected"));
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populateUserStrip() {
    QString shown = myDisplayName_.empty()
        ? QString::fromStdString(myUserId_)
        : QString::fromStdString(myDisplayName_);
    userNameLabel_->setText(shown);
    if (userIdLabel_) userIdLabel_->setText(QString::fromStdString(myUserId_));

    QPixmap avatar;
    if (!myAvatarUrl_.empty() && client_) {
        auto bytes = client_->fetch_media_bytes(myAvatarUrl_);
        if (!bytes.empty()) {
            QPixmap raw;
            if (raw.loadFromData(bytes.data(), static_cast<int>(bytes.size()))) {
                avatar = makeCirclePixmap(
                    raw.scaled(32, 32, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation),
                    32);
            }
        }
    }
    if (avatar.isNull())
        avatar = makeInitialsPixmap(shown, 32);
    userAvatarLabel_->setPixmap(avatar);

    userStrip_->setVisible(true);
}

void MainWindow::onUserStripContextMenu(const QPoint& pos) {
    QMenu menu(this);
    QAction* addAct = menu.addAction(tr("Add Account…"));
    QString logout_label = tr("Log Out %1").arg(
        myDisplayName_.empty()
            ? QString::fromStdString(myUserId_)
            : QString::fromStdString(myDisplayName_));
    QAction* logoutAct = menu.addAction(logout_label);
    QAction* picked = menu.exec(userStrip_->mapToGlobal(pos));
    if      (picked == addAct)    beginAddAccount();
    else if (picked == logoutAct) logoutActiveAccount();
}

void MainWindow::onUserStripLeftClick(const QPoint& pos) {
    // Left-click is a no-op when there's only one signed-in account —
    // the picker would just show the active row and do nothing.
    if (accounts_.size() < 2) return;
    openAccountPicker(userStrip_->mapToGlobal(pos));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == userStrip_ && event->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            onUserStripLeftClick(me->pos());
            return true;
        }
    }
    if (obj == roomHeaderTopic_ && event->type() == QEvent::Resize) {
        updateTopicElision();
    }
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// Multi-account orchestration
// ---------------------------------------------------------------------------

void MainWindow::wireBridge(EventBridge* b) {
    // Each AccountSession's bridge is connected to the same MainWindow
    // slots. Slots check `sender()` against `bridge_` (the active bridge)
    // to filter out callbacks coming from inactive accounts — except
    // `onRoomsUpdated`, which caches every account's snapshot in
    // `per_account_rooms_` so a fast switch is immediate, and
    // `onNotificationTriggered`, which fires regardless of foreground.
    connect(b, &EventBridge::timelineReset,
            this, &MainWindow::onTimelineReset);
    connect(b, &EventBridge::messageInserted,
            this, &MainWindow::onMessageInserted);
    connect(b, &EventBridge::messageUpdated,
            this, &MainWindow::onMessageUpdated);
    connect(b, &EventBridge::messageRemoved,
            this, &MainWindow::onMessageRemoved);
    connect(b, &EventBridge::roomsUpdated,
            this, &MainWindow::onRoomsUpdated);
    connect(b, &EventBridge::syncError,
            this, &MainWindow::onSyncError);
    connect(b, &EventBridge::backupProgress,
            this, &MainWindow::onBackupProgress);
    connect(b, &EventBridge::roomListStateChanged,
            this, &MainWindow::onRoomListStateChanged);
    connect(b, &EventBridge::imagePacksUpdated,
            this, [this, b]{
                if (b == bridge_ && stickerPicker_) stickerPicker_->refreshPacks();
            },
            Qt::QueuedConnection);
    connect(b, &EventBridge::accountPrefsUpdated,
            this, [this, b](const QString& json) {
                if (b != bridge_) return;   // ignore inactive accounts
                auto prefs = tesseract::Prefs::parse(json.toStdString());
                if (!prefs.last_room.empty() && pendingRestoreRoom_.empty() && currentRoomId_.empty())
                    pendingRestoreRoom_ = prefs.last_room;
            },
            Qt::QueuedConnection);
    connect(b, &EventBridge::notificationTriggered,
            this, &MainWindow::onNotificationTriggered,
            Qt::QueuedConnection);
}

void MainWindow::switchActiveAccount(int new_idx) {
    if (new_idx < 0 || new_idx >= static_cast<int>(accounts_.size())) return;
    if (new_idx == active_account_index_ && client_) return;

    // Unsubscribe the previous account's open room so its timeline stops
    // streaming updates to the message list when we swap surfaces.
    if (client_ && !currentRoomId_.empty()) {
        client_->unsubscribe_room(currentRoomId_);
    }
    currentRoomId_.clear();
    clearMessages();

    active_account_index_ = new_idx;
    auto& s = *accounts_[new_idx];
    client_ = s.client.get();
    bridge_ = static_cast<EventBridge*>(s.bridge.get());

    myUserId_      = s.user_id;
    myDisplayName_ = s.display_name;
    myAvatarUrl_   = s.avatar_url;
    pendingRestoreRoom_ = s.last_room;

    populateUserStrip();
    if (emojiPicker_)   emojiPicker_->setClient(client_);
    if (stickerPicker_) stickerPicker_->setClient(client_);

    // Use this account's last-known rooms snapshot if we have one
    // cached; otherwise wait for the next on_rooms_updated callback to
    // populate the list.
    auto it = per_account_rooms_.find(s.user_id);
    if (it != per_account_rooms_.end()) {
        rooms_ = it->second;
        refreshRoomList();
    } else {
        rooms_.clear();
        refreshRoomList();
    }

    // Persist the active selection.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = s.user_id;
    for (auto& a : accounts_) idx.user_ids.push_back(a->user_id);
    tesseract::SessionStore::save_index(idx);

    rebuildAccountPicker();
    maybeShowRecoveryBanner();
}

void MainWindow::beginAddAccount() {
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;

    // Create a fresh client for the OAuth round-trip. The user_id won't
    // be known until await_oauth completes, so we point the SDK at a
    // per-attempt "pending-<ts>" directory; onLoginSucceeded renames
    // it to accounts/<sanitized-uid>/ once the round-trip completes.
    pending_login_client_ = std::make_unique<tesseract::Client>();
    pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
        "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
    std::error_code ec;
    std::filesystem::create_directories(pending_login_temp_dir_, ec);
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());

    loginView_->set_client(pending_login_client_.get());
    loginView_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    loginView_->reset();
    contentStack_->setCurrentWidget(loginView_);
    statusBar()->showMessage(tr("Add Account"));
}

void MainWindow::logoutActiveAccount() {
    if (active_account_index_ < 0) return;
    auto& a = *accounts_[active_account_index_];
    const std::string uid = a.user_id;

    a.client->logout();
    a.client->stop_sync();
    tesseract::SessionStore::clear_account(uid);
    per_account_rooms_.erase(uid);

    // Remove this account from the live vector.
    accounts_.erase(accounts_.begin() + active_account_index_);
    active_account_index_ = -1;
    client_ = nullptr;
    bridge_ = nullptr;

    // Reset visible state regardless of where we go next.
    currentRoomId_.clear();
    myUserId_.clear();
    myDisplayName_.clear();
    myAvatarUrl_.clear();
    rooms_.clear();
    refreshRoomList();
    clearMessages();
    if (recoverySurface_) recoverySurface_->setVisible(false);
    recoveryBannerDismissed_ = false;
    roomHeader_->setVisible(false);

    if (accounts_.empty()) {
        // No accounts left → back to initial login.
        userStrip_->setVisible(false);
        tesseract::SessionStore::save_index({});
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Signed out"), 3000);
        rebuildAccountPicker();
        return;
    }

    // Otherwise switch to the next account (or the first one if we
    // removed the last in the vector). Update the on-disk active pointer.
    switchActiveAccount(0);
    statusBar()->showMessage(tr("Signed out of %1")
                                 .arg(QString::fromStdString(uid)),
                             3000);
}

void MainWindow::rebuildAccountPicker() {
    if (!accountPicker_) return;
    std::vector<tesseract::views::AccountEntry> entries;
    entries.reserve(accounts_.size());
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        const auto& a = *accounts_[i];
        entries.push_back({
            a.user_id, a.display_name, a.avatar_url,
            static_cast<int>(i) == active_account_index_,
        });
    }
    accountPicker_->set_entries(std::move(entries));
}

void MainWindow::openAccountPicker(const QPoint& global_anchor) {
    if (!accountPickerPopover_) {
        accountPickerPopover_ = new QFrame(this);
        accountPickerPopover_->setWindowFlags(Qt::Popup
                                              | Qt::FramelessWindowHint);
        accountPickerPopover_->setFrameShape(QFrame::Box);
        accountPickerPopover_->setStyleSheet(
            "QFrame { background-color: #FFFFFF; border:1px solid #C9CDD2; }");
        auto* lay = new QVBoxLayout(accountPickerPopover_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        accountPickerSurface_ = new tk::qt6::Surface(tk::Theme::light(),
                                                     accountPickerPopover_);
        auto picker_owner = std::make_unique<tesseract::views::AccountPicker>();
        accountPicker_ = picker_owner.get();
        accountPicker_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it != tk_avatars_.end() ? it->second.get() : nullptr;
            });
        accountPicker_->on_select = [this](const std::string& uid) {
            onAccountSelected(uid);
        };
        accountPickerSurface_->set_root(std::move(picker_owner));
        lay->addWidget(accountPickerSurface_);
    }
    rebuildAccountPicker();

    constexpr int kPickerWidth = 260;
    constexpr int kRowHeight   = 56;
    const int rows = static_cast<int>(accounts_.size());
    const int height = std::max(kRowHeight, rows * kRowHeight) + 2;
    accountPickerPopover_->resize(kPickerWidth, height);

    // Anchor above the strip (popover hangs down from the user-strip top
    // edge on most desktops; if it would clip the screen bottom Qt::Popup
    // reflows automatically).
    QPoint anchor = global_anchor;
    anchor.setY(anchor.y() - height);
    accountPickerPopover_->move(anchor);
    accountPickerPopover_->show();
}

void MainWindow::onAccountSelected(const std::string& user_id) {
    if (accountPickerPopover_) accountPickerPopover_->hide();
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        if (accounts_[i]->user_id == user_id) {
            switchActiveAccount(static_cast<int>(i));
            contentStack_->setCurrentWidget(mainContent_);
            return;
        }
    }
}

void MainWindow::doLogout() {
    // Legacy shim that the tray / external code may still call. Routes to
    // the active-account logout path.
    logoutActiveAccount();
}

} // namespace qt6
