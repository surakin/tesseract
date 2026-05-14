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

Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)
Q_DECLARE_METATYPE(tesseract::BackupProgress)

namespace qt6 {

// EventBridge is now a thin QObject wrapper around EventHandlerBase.
// All IEventHandler method bodies live in EventHandlerBase (shared/app/).
// No EventBridge:: method definitions needed here.

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
    {
        auto* scrollDebounce = new QTimer(this);
        scrollDebounce->setSingleShot(true);
        roomListView_->on_scroll = [this, scrollDebounce] {
            scrollDebounce->start(300);
        };
        connect(scrollDebounce, &QTimer::timeout, this, [this] {
            if (!roomListView_ || !client_) return;
            auto ids = roomListView_->visible_room_ids();
            client_->stop_background_backfill();
            client_->start_background_backfill(ids);
        });
    }
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
        refreshRoomList();
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
    roomListView_->on_search_clear = [this] {
        roomSearchField_->set_text("");
        roomSearchPendingText_.clear();
        if (roomListView_) roomListView_->set_search_text("");
        refreshRoomList();
    };

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

    // Verification banner — inline strip shown when the device is unverified.
    verifSurface_ = new tk::qt6::Surface(tk::Theme::light(), chatPanel);
    verifSurface_->setFixedHeight(48);
    verifSurface_->setVisible(false);
    {
        auto banner = std::make_unique<tesseract::views::VerificationBanner>();
        verifShared_ = banner.get();
        verifShared_->on_verify = [this] {
            if (client_) client_->request_self_verification();
        };
        verifShared_->on_accept = [this] {
            if (client_ && !active_verification_flow_id_.empty()) {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verifShared_->on_match = [this] {
            if (client_ && !active_verification_flow_id_.empty()) {
                if (verifShared_)
                    verifShared_->set_state(
                        tesseract::views::VerificationBanner::State::Confirming);
                verifSurface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        verifShared_->on_mismatch = [this] {
            if (client_ && !active_verification_flow_id_.empty())
                client_->cancel_verification(active_verification_flow_id_);
        };
        verifShared_->on_cancel = [this] {
            if (client_ && !active_verification_flow_id_.empty())
                client_->cancel_verification(active_verification_flow_id_);
        };
        verifShared_->on_dismiss = [this] {
            verification_banner_dismissed_ = true;
            if (verifSurface_) verifSurface_->setVisible(false);
        };
        verifShared_->on_done = [this] {
            if (verifSurface_) verifSurface_->setVisible(false);
        };
        verifSurface_->set_root(std::move(banner));
    }
    vLayout->addWidget(verifSurface_);

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
            // Animated entries take priority; static cache is the second hop.
            if (const auto* f = anim_cache_.current_frame(mxc)) return f;
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
                // Prefer the full-res decode; fall back to the inline thumbnail
                // while the full-res fetch is still in flight.
                if (auto it = viewerFullresCache_.find(url);
                    it != viewerFullresCache_.end()) return it->second.get();
                if (const auto* f = anim_cache_.current_frame(url)) return f;
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

            // Fetch and decode at native resolution. The inline path stores a
            // 320×200-capped copy in tk_images_; the viewer needs the original.
            // Skip animated images — they're already playing from anim_cache_.
            const std::string& url = hit.media_url;
            if (!url.empty()
                && !viewerFullresCache_.count(url)
                && !anim_cache_.has(url)
                && viewerFullresInFlight_.insert(url).second)
            {
                runOnPool_([this, url]() {
                    auto bytes = client_->fetch_source_bytes(url);
                    QMetaObject::invokeMethod(this,
                        [this, url, bytes = std::move(bytes)]() mutable {
                            viewerFullresInFlight_.erase(url);
                            if (bytes.empty() || viewerFullresCache_.count(url)) return;
                            QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<int>(bytes.size()));
                            QBuffer buf(&qb);
                            buf.open(QIODevice::ReadOnly);
                            QImageReader reader(&buf);
                            reader.setAutoTransform(true);
                            QImage img;
                            if (!reader.read(&img)) return;
                            viewerFullresCache_.emplace(url,
                                tk::qt6::make_image(std::move(img)));
                            if (imgViewerSurface_) imgViewerSurface_->update();
                        });
                });
            }
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
                             hit.duration_ms, hit.natural_w, hit.natural_h,
                             hit.autoplay, hit.loop, hit.no_audio, hit.hide_controls);
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

    messageListView_->set_video_player_factory(
        [this]() { return msgSurface_->host().make_video_player(); });
    messageListView_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready) {
            runOnPool_([this, src, on_ready = std::move(on_ready)]() mutable {
                auto bytes = client_->fetch_source_bytes(src);
                QMetaObject::invokeMethod(this,
                    [on_ready = std::move(on_ready), bytes = std::move(bytes)]() mutable {
                        on_ready(std::move(bytes));
                    }, Qt::QueuedConnection);
            });
        });

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
        if (current_room_id_.empty()) return;
        std::string trimmed = QString::fromStdString(body).trimmed().toStdString();
        if (trimmed.empty()) return;
        auto res = client_->send_message(current_room_id_, trimmed);
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
        if (current_room_id_.empty()) return;
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
        auto res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
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
        if (current_room_id_.empty()) return;
        auto res = client_->send_file(current_room_id_, bytes, mime,
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
        if (body.empty() || current_room_id_.empty()) return;
        auto res = client_->send_reply(current_room_id_, reply_event_id, body);
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
            if (!current_room_id_.empty()) {
                client_->send_reaction(current_room_id_, ev, glyph.toStdString());
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

    emojiPicker_->onEmoticonSelected = [this](const tesseract::ImagePackImage& img) {
        if (!composeTextArea_) return;
        std::string cur = composeTextArea_->text();
        cur += ":" + img.shortcode + ":";
        composeTextArea_->set_text(cur);
        if (composeShared_) composeShared_->set_current_text(cur);
        composeTextArea_->set_focused(true);
    };

    // Sticker picker: floating panel anchored at the compose-bar sticker
    // button. On selection, send `m.sticker` to the current room (matrix-
    // sdk encrypts transparently in E2EE rooms).
    stickerPicker_ = new StickerPicker(this);
    stickerPicker_->setClient(client_);
    stickerPicker_->onSelected =
        [this](const tesseract::ImagePackImage& img) {
            if (current_room_id_.empty()) return;
            std::string body = img.body.empty() ? img.shortcode : img.body;
            client_->send_sticker(current_room_id_, body, img.url, img.info_json);
            stickerPicker_->hide();
        };

    // Reaction-chip click: toggle (Rust handles add/remove).
    messageListView_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            if (current_room_id_.empty()) return;
            client_->send_reaction(current_room_id_, event_id, key);
        };

    // "+" pseudo-chip click: open the emoji picker in reaction mode.
    messageListView_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor) {
            if (!emojiPicker_ || current_room_id_.empty()) return;
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
        if (new_body.empty() || current_room_id_.empty()) return;
        auto res = client_->send_edit(current_room_id_, event_id, new_body);
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

    // Bridges live in `accounts_[].bridge`; EventHandlerBase routes all
    // callbacks through post_to_ui_ → handle_*_ui_() on MainWindow.

    // Notifiers are created per-account in doLogin / onLoginSucceeded.

    // Animation frame-tick for inline media in the timeline (GIF /
    // animated WebP / APNG). 60 Hz; the timer self-stops in
    // `onMessageAnimTick_` when `anim_cache_` empties.
    tk_anim_timer_ = new QTimer(this);
    tk_anim_timer_->setInterval(16);
    connect(tk_anim_timer_, &QTimer::timeout,
            this, &MainWindow::onMessageAnimTick_);

    // Back-pagination on scroll-to-top. The shared MessageListView fires
    // this once per crossing of the near-top threshold; the latch is
    // re-armed automatically after prepended rows are spliced in.
    messageListView_->on_near_top = [this]{
        if (current_room_id_.empty()) return;
        requestMoreHistory(current_room_id_);
    };

    messageListView_->on_receipt_needed = [this](const std::string& eid) {
        maybe_send_read_receipt_(current_room_id_, eid);
    };

    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    // Drain ALL background workers before tearing the clients down.
    //
    // Two independent worker systems must both be drained:
    //
    //  1. mediaPool_ (QThreadPool) — used by runOnPool_() for pagination.
    //     Guarded by shuttingDown_; drained by mediaPool_.waitForDone(-1).
    //
    //  2. ShellBase detached std::threads — used by run_async_() for avatar /
    //     media fetches (ensure_room_avatar_, ensure_user_avatar_,
    //     ensure_media_image_).  Guarded by shutting_down_ (ShellBase);
    //     tracked by workers_in_flight_ / workers_cv_.
    //
    // Both flags must be flipped first so no new work is enqueued after the
    // clear/drain calls.  stop_sync() is called before the waits so the
    // Rust-side cancellation channel fires and any worker blocked inside a
    // tokio block_on() returns promptly instead of waiting on a network hop.
    // ~ClientFfi calls stop_sync() a second time as a no-op safety net
    // (handler.take() returns None on repeated calls).
    shuttingDown_.store(true, std::memory_order_release);
    shutting_down_.store(true, std::memory_order_release);
    mediaPool_.clear();
    for (auto& a : accounts_) {
        if (a && a->client) a->client->stop_sync();
    }
    if (pending_login_client_) pending_login_client_->stop_sync();
    // Drain ShellBase detached threads (avatar / media fetches).
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                              [this]{ return workers_in_flight_ == 0; });
    }
    // Drain Qt pool workers (pagination).  Unbounded: shutting_down_ is true
    // so no new runnables can be queued, and clear() removed pending ones.
    mediaPool_.waitForDone(-1);

    client_        = nullptr;
    event_handler_ = nullptr;

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

        auto bridge = std::make_unique<EventBridge>(this, this);
        bridge->set_user_id(uid);
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

    auto bridge = std::make_unique<EventBridge>(this, this);
    bridge->set_user_id(user_id);
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
    show();
    raise();
    activateWindow();
}


void MainWindow::onSendClicked() {
    if (composeShared_) composeShared_->trigger_send();
}

void MainWindow::onRoomSelected(const std::string& room_id) {
    if (room_id.empty()) return;

    // Drill into a space if the clicked row is one.
    for (const auto& r : rooms_) {
        if (r.id == room_id && r.is_space) {
            space_stack_.push_back(room_id);
            refreshRoomList();
            return;
        }
    }

    if (!current_room_id_.empty() && current_room_id_ != room_id)
        client_->unsubscribe_room(current_room_id_);

    current_room_id_ = room_id;
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
    if (composeTextArea_) composeTextArea_->set_text("");
    if (composeShared_)   composeShared_->set_current_text({});

    for (const auto& r : rooms_)
        if (r.id == current_room_id_) { updateRoomHeader(r); break; }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the UI stays responsive during the
    // first-load network round-trip.
    auto visible_ids = roomListView_ ? roomListView_->visible_room_ids()
                                     : std::vector<std::string>{};
    std::string sub_room = current_room_id_;
    runOnPool_([this, sub_room, visible_ids = std::move(visible_ids)]{
        auto res = client_->subscribe_room(sub_room);
        bool ok  = res.ok;
        std::string msg = res.message;
        bool reached = false;
        if (ok) {
            auto pr = client_->paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            client_->start_background_backfill(visible_ids);
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
                if (current_room_id_ == sub_room) {
                    auto& state = pagination_[sub_room];
                    state.in_flight     = false;
                    state.reached_start = reached;
                }
            },
            Qt::QueuedConnection);
    });
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
    const std::string rid = roomId.toStdString();
    bool is_current = (rid == current_room_id_);
    push_paginate_result_(rid, reached_start);
    if (is_current && messageListView_)
        messageListView_->reset_near_top_latch();
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

// ---------------------------------------------------------------------------
// ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::post_to_ui_(std::function<void()> fn) {
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

void MainWindow::on_rooms_updated_() {
    refreshRoomList();
    if (!current_room_id_.empty()) {
        for (const auto& r : rooms_)
            if (r.id == current_room_id_) { updateRoomHeader(r); break; }
    } else if (!pending_restore_room_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == pending_restore_room_ && !r.is_space) {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                onRoomSelected(target);
                break;
            }
        }
    }
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                        MediaKind kind,
                                        std::vector<uint8_t> bytes) {
    // Called on the UI thread (already posted via post_to_ui_ in ShellBase).
    if (bytes.empty()) {
        mediaImageSizes_.erase(cache_key);
        return;
    }

    if (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar) {
        if (tk_avatars_.count(cache_key)) return;
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                              static_cast<int>(bytes.size())))
            return;
        const int size = (kind == MediaKind::RoomAvatar)
                          ? kRoomAvatarSize : kMsgAvatarSize;
        QImage scaled = img.scaled(size, size,
                                    Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        tk_avatars_.emplace(cache_key, tk::qt6::make_image(std::move(scaled)));
        if (kind == MediaKind::RoomAvatar) {
            if (roomSurface_) roomSurface_->update();
        } else {
            if (msgSurface_) msgSurface_->update();
        }
        return;
    }

    // MediaImage — animated probe first, then static fallback.
    if (tk_images_.count(cache_key) || anim_cache_.has(cache_key)) {
        mediaImageSizes_.erase(cache_key);
        return;
    }
    int max_w = kMaxImageWidth, max_h = kMaxImageHeight;
    if (auto sit = mediaImageSizes_.find(cache_key); sit != mediaImageSizes_.end()) {
        max_w = sit->second.first;
        max_h = sit->second.second;
        mediaImageSizes_.erase(sit);
    }

    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    if (reader.supportsAnimation() && reader.imageCount() > 1) {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                        delays;
        frames.reserve(reader.imageCount());
        delays.reserve(reader.imageCount());
        QImage frame;
        while (reader.read(&frame)) {
            int delay = reader.nextImageDelay();
            if (delay <= 0) delay = 100;
            if (delay < 20) delay = 20;
            QImage scaled = frame.scaled(max_w, max_h,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            frames.push_back(tk::qt6::make_image(std::move(scaled)));
            delays.push_back(delay);
        }
        if (!frames.empty()) {
            anim_cache_.store(cache_key, std::move(frames), std::move(delays),
                              QDateTime::currentMSecsSinceEpoch());
            if (tk_anim_timer_ && !tk_anim_timer_->isActive())
                tk_anim_timer_->start();
            if (msgSurface_) msgSurface_->update();
            return;
        }
        buf.seek(0);
    }

    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(qb.constData()),
                          qb.size()))
        return;
    QImage scaled = img.scaled(max_w, max_h,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    tk_images_.emplace(cache_key, tk::qt6::make_image(std::move(scaled)));
    if (msgSurface_) msgSurface_->update();
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                            const std::string& video_url) {
    const std::string src = video_url;
    runOnPool_([this, eid = event_id, src]() {
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
                        if (!enc.isEmpty()) {
                            std::vector<uint8_t> v(
                                reinterpret_cast<const uint8_t*>(enc.constData()),
                                reinterpret_cast<const uint8_t*>(enc.constData()) + enc.size());
                            on_media_bytes_ready_(key, MediaKind::MediaImage,
                                                  std::move(v));
                        }
                    });
                player->play();
            }, Qt::QueuedConnection);
    });
}

void MainWindow::onMessageAnimTick_() {
    if (anim_cache_.empty()) {
        if (tk_anim_timer_) tk_anim_timer_->stop();
        return;
    }
    if (anim_cache_.advance(QDateTime::currentMSecsSinceEpoch()) && msgSurface_)
        msgSurface_->update();
}

void MainWindow::showRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(r);

    // Eagerly fetch avatars for the new room set so the first paint has
    // them ready. Bytes-already-cached is a no-op via tk_avatars_.count.
    for (const auto& r : sorted) ensure_room_avatar_(r);

    roomListView_->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
        roomListView_->set_selected_room(current_room_id_);
    roomSurface_->relayout();
}

void MainWindow::refreshRoomList() {
    if (space_stack_.empty()) {
        if (!roomSearchPendingText_.empty()) {
            showRooms(rooms_);
            roomNavBar_->setVisible(false);
            return;
        }
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
        const std::string& space_id = space_stack_.back();
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
    if (!space_stack_.empty()) space_stack_.pop_back();
    refreshRoomList();
}

// ---------------------------------------------------------------------------

void MainWindow::ensureRowMedia(const tesseract::Event& ev) {
    // Store decode size hints before delegating to the ShellBase helper.
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        if (!img.image_url.empty())
            mediaImageSizes_[img.image_url] = { kMaxImageWidth, kMaxImageHeight };
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        if (!s.image_url.empty())
            mediaImageSizes_[s.image_url] = { kMaxStickerSize, kMaxStickerSize };
    } else if (ev.type == tesseract::EventType::Video) {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
            mediaImageSizes_[vid.thumbnail_url] = { kMaxImageWidth, kMaxImageHeight };
    }
    for (const auto& r : ev.reactions)
        if (!r.source_json.empty())
            mediaImageSizes_[r.source_json] = { 20, 20 };
    ensure_row_media_(ev);
}


// ---------------------------------------------------------------------------
// Recovery banner + dialog (Step 6)
// ---------------------------------------------------------------------------

void MainWindow::maybeShowRecoveryBanner() {
    if (recovery_banner_dismissed_) return;
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
    recovery_banner_dismissed_ = true;
    if (recoverySurface_) recoverySurface_->setVisible(false);
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

    const bool room_busy = (last_room_list_state_ == RLS::Init
                         || last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy) {
        // Debounce: only show "Syncing rooms…" after 300 ms of being
        // non-Running, so already-cached sessions that flash through
        // Init→Running in a single tick don't churn the bar.
        if (!syncStatusDebounce_) {
            syncStatusDebounce_ = new QTimer(this);
            syncStatusDebounce_->setSingleShot(true);
            connect(syncStatusDebounce_, &QTimer::timeout, this, [this] {
                using RLS2 = tesseract::RoomListState;
                if (last_room_list_state_ == RLS2::Init
                 || last_room_list_state_ == RLS2::SettingUp) {
                    sync_progress_shown_ = true;
                    statusBar()->showMessage(tr("Syncing rooms…"));
                }
            });
        }
        if (!syncStatusDebounce_->isActive() && !sync_progress_shown_)
            syncStatusDebounce_->start(300);
        else if (sync_progress_shown_)
            statusBar()->showMessage(tr("Syncing rooms…"));
        return;
    }

    if (syncStatusDebounce_) syncStatusDebounce_->stop();

    if (reconnecting) {
        sync_progress_shown_ = true;
        statusBar()->showMessage(tr("Reconnecting…"));
        return;
    }
    if (keys_busy) {
        sync_progress_shown_ = true;
        statusBar()->showMessage(
            tr("Downloading encryption keys (%1)…")
                .arg(static_cast<qulonglong>(last_imported_keys_)));
        return;
    }
    if (sync_progress_shown_) {
        // We covered up a prior login message; settle to the steady-state copy.
        sync_progress_shown_ = false;
        statusBar()->showMessage(tr("Connected"));
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populateUserStrip() {
    QString shown = my_display_name_.empty()
        ? QString::fromStdString(my_user_id_)
        : QString::fromStdString(my_display_name_);
    userNameLabel_->setText(shown);
    if (userIdLabel_) userIdLabel_->setText(QString::fromStdString(my_user_id_));

    QPixmap avatar;
    if (!my_avatar_url_.empty() && client_) {
        auto bytes = client_->fetch_media_bytes(my_avatar_url_);
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
        my_display_name_.empty()
            ? QString::fromStdString(my_user_id_)
            : QString::fromStdString(my_display_name_));
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
// EventHandlerBase UI-thread hook implementations (Qt6)
// ---------------------------------------------------------------------------

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    if (room_id != current_room_id_) return;
    std::vector<tesseract::views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto& ev : snapshot) {
        if (!ev) continue;
        ensureRowMedia(*ev);
        ensure_reply_details_(ev->in_reply_to_id);
        rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
    }
    messageListView_->set_messages(std::move(rows));
    msgSurface_->relayout();
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || room_id != current_room_id_
            || ev->type == tesseract::EventType::Unhandled)
        return;
    ensureRowMedia(*ev);
    ensure_reply_details_(ev->in_reply_to_id);
    messageListView_->insert_message(index,
        tesseract::views::make_row_data(*ev, my_user_id_));
    msgSurface_->relayout();
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || room_id != current_room_id_
            || ev->type == tesseract::EventType::Unhandled)
        return;
    ensureRowMedia(*ev);
    ensure_reply_details_(ev->in_reply_to_id);
    messageListView_->update_message(index,
        tesseract::views::make_row_data(*ev, my_user_id_));
    msgSurface_->relayout();
}

void MainWindow::handle_message_removed_ui_(
    std::string room_id, std::size_t index)
{
    if (room_id != current_room_id_) return;
    messageListView_->remove_message(index);
    msgSurface_->relayout();
}

void MainWindow::handle_sync_error_ui_(
    std::string context, std::string user_id,
    std::string description, bool soft_logout)
{
    tesseract::AccountSession* affected = nullptr;
    for (auto& a : accounts_)
        if (a->user_id == user_id) { affected = a.get(); break; }

    if (context == "sync_reconnect") {
        statusBar()->showMessage(tr("Sync error: reconnecting\xe2\x80\xa6"));
        if (affected && affected->client) {
            affected->client->stop_sync();
            affected->sync_started = false;
            QTimer::singleShot(5000, this, [this, uid = affected->user_id]() {
                for (auto& a : accounts_) {
                    if (a->user_id == uid && !a->sync_started && a->client) {
                        a->sync_started = true;
                        a->client->start_sync(a->bridge.get());
                    }
                }
            });
        }
    } else if (context == "sync_auth_error") {
        if (soft_logout && affected) {
            if (auto saved = tesseract::SessionStore::load_account(affected->user_id)) {
                statusBar()->showMessage(tr("Reconnecting session\xe2\x80\xa6"));
                if (affected->client->restore_session(*saved)) {
                    affected->display_name = affected->client->get_display_name();
                    affected->avatar_url   = affected->client->get_avatar_url();
                    if (affected == accounts_[std::max(0, active_account_index_)].get())
                        switchActiveAccount(active_account_index_);
                    affected->client->start_sync(affected->bridge.get());
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
        statusBar()->showMessage(
            tr("Sync error: %1").arg(QString::fromStdString(description)), 8000);
    }
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    // Only the active account's backup state drives the recovery banner and
    // status bar — but we can't filter by user_id here since BackupProgress
    // doesn't carry one. We rely on EventHandlerBase's post_to_ui_ being
    // per-instance: the active client_ pointer check gates the banner update.
    if (!client_) return;

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

    last_backup_state_  = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refreshSyncStatus();
}

void MainWindow::handle_image_packs_updated_ui_()
{
    if (stickerPicker_)  stickerPicker_->refreshPacks();
    if (emojiPicker_)    emojiPicker_->refreshEmoticonPacks();
}

void MainWindow::handle_account_prefs_updated_ui_(
    std::string user_id, std::string json)
{
    // Only the active account's prefs set the pending restore room.
    if (active_account_index_ < 0
            || accounts_[active_account_index_]->user_id != user_id)
        return;
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty()
            && pending_restore_room_.empty()
            && current_room_id_.empty())
        pending_restore_room_ = prefs.last_room;
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id,
    std::string room_name, std::string sender,
    std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes)
{
    for (auto& sess : accounts_) {
        if (sess->user_id != user_id) continue;
        if (isActiveWindow()
                && active_account_index_ >= 0
                && accounts_[active_account_index_]->user_id == user_id
                && current_room_id_ == room_id)
            return;
        if (sess->notifier) {
            tesseract::Notification n;
            n.room_id      = room_id;
            n.room_name    = room_name;
            n.sender       = sender;
            n.body         = body;
            n.is_mention   = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            sess->notifier->notify(n);
        }
        return;
    }
}

void MainWindow::on_room_list_state_ui_()
{
    refreshSyncStatus();
}

// ---------------------------------------------------------------------------
// Multi-account orchestration
// ---------------------------------------------------------------------------

void MainWindow::switchActiveAccount(int new_idx) {
    if (new_idx < 0 || new_idx >= static_cast<int>(accounts_.size())) return;
    if (new_idx == active_account_index_ && client_) return;

    // Unsubscribe the previous account's open room so its timeline stops
    // streaming updates to the message list when we swap surfaces.
    if (client_ && !current_room_id_.empty()) {
        client_->unsubscribe_room(current_room_id_);
    }
    current_room_id_.clear();
    clearMessages();

    active_account_index_ = new_idx;
    auto& s = *accounts_[new_idx];
    client_        = s.client.get();
    event_handler_ = s.bridge.get();   // keep ShellBase's non-owning alias in sync

    my_user_id_      = s.user_id;
    my_display_name_ = s.display_name;
    my_avatar_url_   = s.avatar_url;
    pending_restore_room_ = s.last_room;

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
    client_        = nullptr;
    event_handler_ = nullptr;

    // Reset visible state regardless of where we go next.
    current_room_id_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    rooms_.clear();
    refreshRoomList();
    clearMessages();
    if (recoverySurface_) recoverySurface_->setVisible(false);
    recovery_banner_dismissed_ = false;
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

// ── Cross-signing / SAS verification hooks ────────────────────────────────────

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!verifSurface_ || !verifShared_) return;
    if (is_verified) {
        verifSurface_->setVisible(false);
        return;
    }
    if (verification_banner_dismissed_) return;
    // Show Prompt state when unverified.
    if (!verifSurface_->isVisible()) {
        active_verification_flow_id_.clear();
        verifShared_->set_state(tesseract::views::VerificationBanner::State::Prompt);
        verifSurface_->setVisible(true);
        verifSurface_->relayout();
    }
}

void MainWindow::handle_verification_request_ui_(
    std::string flow_id, std::string /*user_id*/,
    std::string /*device_id*/, bool incoming)
{
    if (!verifSurface_ || !verifShared_) return;
    active_verification_flow_id_ = flow_id;
    if (incoming) {
        verifShared_->set_state(tesseract::views::VerificationBanner::State::IncomingRequest);
    } else {
        // Our request was accepted — switch to Waiting, immediately start SAS.
        verifShared_->set_state(tesseract::views::VerificationBanner::State::Waiting);
        if (client_) client_->start_sas(flow_id);
    }
    verifSurface_->setVisible(true);
    verifSurface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!verifSurface_ || !verifShared_) return;
    verifShared_->set_emojis(emojis);
    verifSurface_->setFixedHeight(124);
    verifSurface_->setVisible(true);
    verifSurface_->relayout();
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    if (!verifSurface_ || !verifShared_) return;
    verifShared_->set_state(tesseract::views::VerificationBanner::State::Done);
    verifSurface_->setFixedHeight(48);
    verifSurface_->relayout();
    // Hide after 1.5 s via on_done callback (already wired in constructor).
    QTimer::singleShot(1500, this, [this] {
        if (verifShared_)
            verifShared_->on_done ? verifShared_->on_done() : void();
    });
}

void MainWindow::handle_verification_cancelled_ui_(
    std::string /*flow_id*/, std::string reason)
{
    if (!verifSurface_ || !verifShared_) return;
    verifShared_->set_state(tesseract::views::VerificationBanner::State::Cancelled);
    verifShared_->set_cancel_reason(std::move(reason));
    verifSurface_->setFixedHeight(48);
    verifSurface_->setVisible(true);
    verifSurface_->relayout();
}

} // namespace qt6
