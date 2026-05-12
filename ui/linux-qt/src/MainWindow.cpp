#include "MainWindow.h"
#include "LoginView.h"
#include "EmojiPicker.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"

#include <QThreadPool>
#include <QMenu>
#include <QAction>

#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMetaType>
#include <QKeyEvent>
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
    tesseract::SessionStore::save(session_json);
}

void EventBridge::on_backup_progress(const tesseract::BackupProgress& progress) {
    emit backupProgress(progress);
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

    loginView_ = new LoginView(client_, contentStack_);
    contentStack_->addWidget(loginView_);
    connect(loginView_, &LoginView::loginSucceeded,
            this,       &MainWindow::onLoginSucceeded);

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

    // ---- User identity strip (footer) ----
    userStrip_ = new QWidget(sidePanel);
    userStrip_->setObjectName("userStrip");
    userStrip_->setStyleSheet(
        "#userStrip { background-color:#E8EAEE; border-top:1px solid #D0D3D8; }");
    userStrip_->setFixedHeight(48);
    userStrip_->setVisible(false);
    userStrip_->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        auto* uLayout = new QHBoxLayout(userStrip_);
        uLayout->setContentsMargins(8, 6, 8, 6);
        uLayout->setSpacing(8);

        userAvatarLabel_ = new QLabel(userStrip_);
        userAvatarLabel_->setFixedSize(32, 32);
        uLayout->addWidget(userAvatarLabel_);

        userNameLabel_ = new QLabel(userStrip_);
        userNameLabel_->setStyleSheet("font-size:12px; font-weight:bold; color:#111111;");
        userNameLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
        uLayout->addWidget(userNameLabel_, 1);
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
            auto it = tk_images_.find(mxc);
            return it == tk_images_.end() ? nullptr : it->second.get();
        });
    msgSurface_->set_root(std::move(msg_view_owner));
    vLayout->addWidget(msgSurface_, 1);

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
    composeTextArea_->set_placeholder("Message…");
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
        const auto limit = client_.media_upload_limit();
        if (limit > 0 && bytes.size() > limit) {
            statusBar()->showMessage(
                QStringLiteral("File exceeds server limit (%1)")
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

    composeShared_->on_send  = [this](const std::string&) { onSendClicked(); };
    composeShared_->on_send_image = [this](std::vector<std::uint8_t> bytes,
                                             std::string mime,
                                             std::string filename,
                                             std::string caption,
                                             std::uint32_t /*src_w*/,
                                             std::uint32_t /*src_h*/) {
        if (currentRoomId_.empty()) return;
        const bool compress =
            tesseract::Settings::instance().image_quality
            == tesseract::Settings::ImageQuality::Compressed;
        auto enc = composeSurface_->host().encode_for_send(
            bytes.data(), bytes.size(), compress);
        if (enc.bytes.empty()) {
            statusBar()->showMessage("Image decode failed", 4000);
            return;
        }
        // After compression mime/extension may have changed to JPEG.
        std::string out_name = filename;
        if (enc.mime == "image/jpeg") {
            auto dot = out_name.find_last_of('.');
            if (dot != std::string::npos) out_name = out_name.substr(0, dot);
            out_name += ".jpg";
        }
        auto res = client_.send_image(currentRoomId_, enc.bytes, enc.mime,
                                        out_name, caption,
                                        enc.width, enc.height);
        if (!res) {
            statusBar()->showMessage(
                "Send image failed: " + QString::fromStdString(res.message),
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
                                            std::string caption) {
        if (currentRoomId_.empty()) return;
        auto res = client_.send_file(currentRoomId_, bytes, mime,
                                      filename, caption);
        if (!res) {
            statusBar()->showMessage(
                "Send file failed: " + QString::fromStdString(res.message),
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

    vLayout->addWidget(composeSurface_);

    // Emoji picker: build the floating panel, wire selection → cursor
    // insert + account-data bump. Recents live in the SDK now (synced via
    // `io.element.recent_emoji`), so no local-disk load is needed.
    emojiPicker_ = new EmojiPicker(this);
    emojiPicker_->setClient(&client_);
    emojiPicker_->onSelected = [this](const QString& glyph) {
        // Reaction mode: a message's "+" chip set pendingReactionEventId_
        // before opening the picker. Route the glyph through
        // send_reaction (toggle semantics handled Rust-side) and skip
        // the compose-bar insert.
        if (!pendingReactionEventId_.empty()) {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            if (!currentRoomId_.empty()) {
                client_.send_reaction(currentRoomId_, ev, glyph.toStdString());
            }
            client_.recent_emoji_bump(glyph.toStdString());
            emojiPicker_->hide();
            return;
        }
        if (!composeTextArea_) return;
        std::string cur = composeTextArea_->text();
        cur += glyph.toStdString();
        composeTextArea_->set_text(cur);
        if (composeShared_) composeShared_->set_current_text(cur);
        composeTextArea_->set_focused(true);
        client_.recent_emoji_bump(glyph.toStdString());
    };

    // Reaction-chip click: toggle (Rust handles add/remove).
    messageListView_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            if (currentRoomId_.empty()) return;
            client_.send_reaction(currentRoomId_, event_id, key);
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

    statusBar()->showMessage("Not logged in");
    // Room selection is delivered through RoomListView's on_room_selected
    // callback wired in the surface-construction block above.

    bridge_ = std::make_unique<EventBridge>(this);
    connect(bridge_.get(), &EventBridge::timelineReset,
            this,          &MainWindow::onTimelineReset);
    connect(bridge_.get(), &EventBridge::messageInserted,
            this,          &MainWindow::onMessageInserted);
    connect(bridge_.get(), &EventBridge::messageUpdated,
            this,          &MainWindow::onMessageUpdated);
    connect(bridge_.get(), &EventBridge::messageRemoved,
            this,          &MainWindow::onMessageRemoved);
    connect(bridge_.get(), &EventBridge::roomsUpdated,
            this,          &MainWindow::onRoomsUpdated);
    connect(bridge_.get(), &EventBridge::syncError,
            this,          &MainWindow::onSyncError);
    connect(bridge_.get(), &EventBridge::backupProgress,
            this,          &MainWindow::onBackupProgress);

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
    client_.stop_sync();
    // LoginView is a child widget, normally destroyed during ~QMainWindow
    // — but that runs *after* client_ has been destroyed, and ~LoginView
    // calls client_.cancel_oauth(). Tear it down here while client_ is
    // still alive.
    delete loginView_;
    loginView_ = nullptr;
}

// ---------------------------------------------------------------------------

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == roomHeaderTopic_ && event->type() == QEvent::Resize) {
        updateTopicElision();
    }
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------

void MainWindow::doLogin() {
    if (auto saved = tesseract::SessionStore::load()) {
        statusBar()->showMessage("Restoring session…");
        auto res = client_.restore_session(*saved);
        if (res) {
            myUserId_      = client_.get_user_id();
            myDisplayName_ = client_.get_display_name();
            myAvatarUrl_   = client_.get_avatar_url();
            populateUserStrip();
            client_.start_sync(bridge_.get());
            statusBar()->showMessage("Connected");
            contentStack_->setCurrentWidget(mainContent_);
            maybeShowRecoveryBanner();
            return;
        }
        tesseract::SessionStore::clear();
        statusBar()->showMessage(
            "Saved session expired: " + QString::fromStdString(res.message),
            6000);
    }

    loginView_->reset();
    contentStack_->setCurrentWidget(loginView_);
    statusBar()->showMessage("Not logged in");
}

void MainWindow::onLoginSucceeded() {
    myUserId_      = client_.get_user_id();
    myDisplayName_ = client_.get_display_name();
    myAvatarUrl_   = client_.get_avatar_url();
    populateUserStrip();
    tesseract::SessionStore::save(client_.export_session());
    client_.start_sync(bridge_.get());
    statusBar()->showMessage("Connected");
    contentStack_->setCurrentWidget(mainContent_);
    maybeShowRecoveryBanner();
}

void MainWindow::onSendClicked() {
    if (currentRoomId_.empty() || !composeTextArea_) return;

    QString text = QString::fromStdString(composeTextArea_->text()).trimmed();
    if (text.isEmpty()) return;

    auto res = client_.send_message(currentRoomId_, text.toStdString());
    if (res) {
        composeTextArea_->set_text("");
        if (composeShared_) composeShared_->set_current_text({});
    } else {
        statusBar()->showMessage(QString::fromStdString(res.message), 4000);
    }
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
        client_.unsubscribe_room(currentRoomId_);

    currentRoomId_ = room_id;

    for (const auto& r : rooms_)
        if (r.id == currentRoomId_) { updateRoomHeader(r); break; }

    auto res = client_.subscribe_room(currentRoomId_);
    if (!res) {
        statusBar()->showMessage(
            "Subscribe failed: " + QString::fromStdString(res.message), 4000);
        return;
    }
    auto& state = pagination_[currentRoomId_];
    state.in_flight = false;
    auto pr = client_.paginate_back_with_status(currentRoomId_, kPaginationBatch);
    state.reached_start = pr.ok && pr.reached_start;
    client_.start_background_backfill();
}

void MainWindow::onTimelineReset(
    QString roomId, std::vector<tesseract::Event*> snapshot)
{
    const bool current = roomId.toStdString() == currentRoomId_;
    if (current) {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto* ev : snapshot) {
            if (!ev) continue;
            ensureRowMedia(*ev);
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
    if (ev && roomId.toStdString() == currentRoomId_
        && ev->type != tesseract::EventType::Unhandled)
    {
        ensureRowMedia(*ev);
        messageListView_->insert_message(index, toRowData(*ev));
        msgSurface_->relayout();
    }
    delete ev;
}

void MainWindow::onMessageUpdated(
    QString roomId, std::size_t index, tesseract::Event* ev)
{
    if (ev && roomId.toStdString() == currentRoomId_
        && ev->type != tesseract::EventType::Unhandled)
    {
        ensureRowMedia(*ev);
        messageListView_->update_message(index, toRowData(*ev));
        msgSurface_->relayout();
    }
    delete ev;
}

void MainWindow::onMessageRemoved(QString roomId, std::size_t index) {
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
    std::thread([this, room_id]{
        auto res = client_.paginate_back_with_status(room_id, kPaginationBatch);
        bool reached = res.ok && res.reached_start;
        QMetaObject::invokeMethod(
            this,
            "onPaginateFinished",
            Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(room_id)),
            Q_ARG(bool, reached));
    }).detach();
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
    rooms_ = std::move(rooms);
    refreshRoomList();
    if (!currentRoomId_.empty())
        for (const auto& r : rooms_)
            if (r.id == currentRoomId_) { updateRoomHeader(r); break; }
}

void MainWindow::onSyncError(
    QString context, QString description, bool soft_logout)
{
    if (context == "sync_reconnect") {
        statusBar()->showMessage("Sync error: reconnecting…");
        client_.stop_sync();
        doLogin();
    } else if (context == "sync_auth_error") {
        if (soft_logout) {
            if (auto saved = tesseract::SessionStore::load()) {
                statusBar()->showMessage("Reconnecting session…");
                if (client_.restore_session(*saved)) {
                    myUserId_      = client_.get_user_id();
                    myDisplayName_ = client_.get_display_name();
                    myAvatarUrl_   = client_.get_avatar_url();
                    populateUserStrip();
                    client_.start_sync(bridge_.get());
                    statusBar()->showMessage("Reconnected");
                    maybeShowRecoveryBanner();
                    return;
                }
            }
        }
        tesseract::SessionStore::clear();
        client_.stop_sync();
        statusBar()->showMessage("Session expired; please log in again.");
        doLogin();
    } else {
        statusBar()->showMessage("Sync error: " + description, 8000);
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
            auto bytes = client_.fetch_avatar_bytes(info.id);
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

void MainWindow::ensureRoomAvatar(const tesseract::RoomInfo& r) {
    if (r.avatar_url.empty()) return;
    const std::string& mxc = r.avatar_url;
    if (tk_avatars_.count(mxc)) return;

    auto bytes = client_.fetch_avatar_bytes(r.id);
    if (bytes.empty()) return;

    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                          static_cast<int>(bytes.size())))
        return;
    QImage scaled = img.scaled(kRoomAvatarSize, kRoomAvatarSize,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    tk_avatars_.emplace(mxc, tk::qt6::make_image(std::move(scaled)));
}

void MainWindow::ensureUserAvatar(const std::string& mxc) {
    if (mxc.empty() || tk_avatars_.count(mxc)) return;
    auto bytes = client_.fetch_media_bytes(mxc);
    if (bytes.empty()) return;
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                          static_cast<int>(bytes.size())))
        return;
    QImage scaled = img.scaled(kMsgAvatarSize, kMsgAvatarSize,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    tk_avatars_.emplace(mxc, tk::qt6::make_image(std::move(scaled)));
}

void MainWindow::ensureMediaImage(const std::string& url, int max_w, int max_h) {
    if (url.empty() || tk_images_.count(url)) return;
    auto bytes = client_.fetch_media_bytes(url);
    if (bytes.empty()) return;
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                          static_cast<int>(bytes.size())))
        return;
    QImage scaled = img.scaled(max_w, max_h,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    tk_images_.emplace(url, tk::qt6::make_image(std::move(scaled)));
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
        showRooms(rooms_);
        roomNavBar_->setVisible(false);
    } else {
        const std::string& space_id = spaceStack_.back();
        auto child_ids = client_.space_children(space_id);
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
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        ensureMediaImage(img.image_url, kMaxImageWidth, kMaxImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        ensureMediaImage(s.image_url, kMaxStickerSize, kMaxStickerSize);
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
    if (!client_.needs_recovery()) return;
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

    auto* runner = QRunnable::create([this, k = key]() {
        auto res = client_.recover(k);
        emit recoverFinished(res.ok, QString::fromStdString(res.message));
    });
    QThreadPool::globalInstance()->start(runner);
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
        && !client_.needs_recovery())
    {
        if (recoverySurface_) recoverySurface_->setVisible(false);
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

    QPixmap avatar;
    if (!myAvatarUrl_.empty()) {
        auto bytes = client_.fetch_media_bytes(myAvatarUrl_);
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
    QAction* logoutAct = menu.addAction(tr("Logout"));
    QAction* picked = menu.exec(userStrip_->mapToGlobal(pos));
    if (picked == logoutAct)
        doLogout();
}

void MainWindow::doLogout() {
    auto res = client_.logout();
    tesseract::SessionStore::clear();
    client_.stop_sync();

    // Reset visible state.
    if (!currentRoomId_.empty())
        client_.unsubscribe_room(currentRoomId_);
    currentRoomId_.clear();
    myUserId_.clear();
    myDisplayName_.clear();
    myAvatarUrl_.clear();
    rooms_.clear();
    refreshRoomList();
    clearMessages();
    userStrip_->setVisible(false);
    if (recoverySurface_) recoverySurface_->setVisible(false);
    recoveryBannerDismissed_ = false;
    roomHeader_->setVisible(false);

    statusBar()->showMessage(res
        ? "Signed out"
        : QString::fromStdString("Sign out failed: " + res.message),
        res ? 3000 : 6000);

    loginView_->reset();
    contentStack_->setCurrentWidget(loginView_);
}

} // namespace qt6
