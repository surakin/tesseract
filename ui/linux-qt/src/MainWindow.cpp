#include "MainWindow.h"
#include "LoginDialog.h"
#include "RoomListDelegate.h"

#include <tesseract/session_store.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMetaType>
#include <QKeyEvent>
#include <QLabel>
#include <QFrame>
#include <QScrollBar>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QTimer>
#include <QStandardItem>
#include <algorithm>

Q_DECLARE_METATYPE(tesseract::Event*)
Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)

namespace qt6 {

// ---------------------------------------------------------------------------
// EventBridge
// ---------------------------------------------------------------------------

void EventBridge::on_message(tesseract::Event* ev) {
    emit eventReceived(ev);
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

void EventBridge::on_timeline_reset(const std::string& room_id) {
    emit timelineReset(QString::fromStdString(room_id));
}

void EventBridge::on_session_saved(const std::string& session_json) {
    tesseract::SessionStore::save(session_json);
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
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();

    setWindowTitle("Tesseract");
    resize(1100, 768);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* hLayout = new QHBoxLayout(central);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    // ---- Sidebar (room list) ----
    auto* sidePanel = new QWidget(this);
    sidePanel->setFixedWidth(260);
    sidePanel->setObjectName("sidePanel");
    sidePanel->setStyleSheet("#sidePanel { background-color: #F0F2F5; border-right: 1px solid #D0D3D8; }");

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
    spaceNameLabel_->setStyleSheet("font-weight: bold; font-size: 12px;");
    navLayout->addWidget(backButton_);
    navLayout->addWidget(spaceNameLabel_, 1);
    roomNavBar_->setVisible(false);
    connect(backButton_, &QPushButton::clicked, this, &MainWindow::onSpaceBack);
    sideLayout->addWidget(roomNavBar_);

    roomModel_ = new QStandardItemModel(this);
    roomList_  = new QListView(sidePanel);
    roomList_->setModel(roomModel_);
    roomList_->setItemDelegate(new RoomListDelegate(roomList_));
    roomList_->setFrameShape(QFrame::NoFrame);
    roomList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    roomList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    roomList_->setUniformItemSizes(true);
    roomList_->setMouseTracking(true);
    sideLayout->addWidget(roomList_);
    hLayout->addWidget(sidePanel);

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
    roomHeaderName_->setStyleSheet("font-size:15px; font-weight:bold; color:#111111;");
    nameVBox->addWidget(roomHeaderName_);

    roomHeaderTopic_ = new QLabel(nameBlock);
    roomHeaderTopic_->setStyleSheet("font-size:12px; color:#65676B;");
    roomHeaderTopic_->setVisible(false);
    roomHeaderTopic_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    roomHeaderTopic_->installEventFilter(this);
    nameVBox->addWidget(roomHeaderTopic_);

    headerLayout->addWidget(nameBlock, 1);
    vLayout->addWidget(roomHeader_);

    // Message scroll area
    msgScrollArea_ = new QScrollArea(chatPanel);
    msgScrollArea_->setWidgetResizable(true);
    msgScrollArea_->setFrameShape(QFrame::NoFrame);
    msgScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    msgScrollArea_->setStyleSheet("QScrollArea { background-color: #FFFFFF; }");

    msgContainer_ = new QWidget(msgScrollArea_);
    msgContainer_->setStyleSheet("background-color: #FFFFFF;");
    msgLayout_    = new QVBoxLayout(msgContainer_);
    msgLayout_->setAlignment(Qt::AlignTop);
    msgLayout_->setSpacing(2);
    msgLayout_->setContentsMargins(12, 12, 12, 12);
    msgScrollArea_->setWidget(msgContainer_);
    connect(msgScrollArea_->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this](int, int max) {
        if (autoScrollPending_) {
            autoScrollPending_ = false;
            msgScrollArea_->verticalScrollBar()->setValue(max);
        }
    });
    vLayout->addWidget(msgScrollArea_, 1);

    // Compose bar
    auto* composeBar = new QWidget(chatPanel);
    composeBar->setStyleSheet("background-color: #F0F2F5; border-top: 1px solid #D0D3D8;");
    auto* composeLayout = new QHBoxLayout(composeBar);
    composeLayout->setContentsMargins(12, 8, 12, 8);
    composeLayout->setSpacing(8);

    composeEdit_ = new QTextEdit(composeBar);
    composeEdit_->setPlaceholderText("Message…");
    composeEdit_->setFixedHeight(40);
    composeEdit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    composeEdit_->setStyleSheet(
        "QTextEdit { border: 1px solid #CED0D4; border-radius: 20px; "
        "padding: 8px 14px; background-color: #FFFFFF; font-size: 14px; }");
    composeEdit_->installEventFilter(this);

    sendButton_ = new QPushButton("Send", composeBar);
    sendButton_->setFixedSize(64, 40);
    sendButton_->setStyleSheet(
        "QPushButton { background-color: #0084FF; color: white; border: none; "
        "border-radius: 20px; font-size: 14px; font-weight: bold; }"
        "QPushButton:hover { background-color: #0077E5; }"
        "QPushButton:pressed { background-color: #006BD6; }");

    composeLayout->addWidget(composeEdit_, 1);
    composeLayout->addWidget(sendButton_);
    vLayout->addWidget(composeBar);

    statusBar()->showMessage("Not logged in");

    // ---- Auto-grow compose field (max 5 lines) ----
    connect(composeEdit_->document(), &QTextDocument::contentsChanged,
            this, [this]() {
        int docH = static_cast<int>(composeEdit_->document()->size().height());
        QFontMetrics fm(composeEdit_->font());
        int maxH  = fm.lineSpacing() * 5 + 20;
        composeEdit_->setFixedHeight(std::clamp(docH + 16, 40, maxH));
    });

    // ---- Signals ----
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSendClicked);
    connect(roomList_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &MainWindow::onRoomSelectionChanged);

    bridge_ = std::make_unique<EventBridge>(this);
    connect(bridge_.get(), &EventBridge::eventReceived,
            this,          &MainWindow::onEventReceived);
    connect(bridge_.get(), &EventBridge::roomsUpdated,
            this,          &MainWindow::onRoomsUpdated);
    connect(bridge_.get(), &EventBridge::syncError,
            this,          &MainWindow::onSyncError);
    connect(bridge_.get(), &EventBridge::timelineReset,
            this,          &MainWindow::onTimelineReset);

    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    client_.stop_sync();
}

// ---------------------------------------------------------------------------

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == roomHeaderTopic_ && event->type() == QEvent::Resize) {
        updateTopicElision();
    }
    if (obj == composeEdit_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return
            && !(ke->modifiers() & Qt::ShiftModifier))
        {
            onSendClicked();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------

void MainWindow::doLogin() {
    if (auto saved = tesseract::SessionStore::load()) {
        statusBar()->showMessage("Restoring session…");
        auto res = client_.restore_session(*saved);
        if (res) {
            myUserId_ = client_.get_user_id();
            client_.start_sync(bridge_.get());
            statusBar()->showMessage("Connected");
            return;
        }
        tesseract::SessionStore::clear();
        statusBar()->showMessage(
            "Saved session expired: " + QString::fromStdString(res.message),
            6000);
    }

    LoginDialog dlg(client_, this);
    if (dlg.exec() != QDialog::Accepted) {
        statusBar()->showMessage("Not logged in");
        return;
    }

    myUserId_ = client_.get_user_id();
    tesseract::SessionStore::save(client_.export_session());
    client_.start_sync(bridge_.get());
    statusBar()->showMessage("Connected");
}

void MainWindow::onSendClicked() {
    if (currentRoomId_.empty()) return;

    QString text = composeEdit_->toPlainText().trimmed();
    if (text.isEmpty()) return;

    auto res = client_.send_message(currentRoomId_, text.toStdString());
    if (res)
        composeEdit_->clear();
    else
        statusBar()->showMessage(QString::fromStdString(res.message), 4000);
}

void MainWindow::onRoomSelectionChanged(
    const QModelIndex& current, const QModelIndex& /*previous*/)
{
    if (!current.isValid()) return;

    QString roomId = roomModel_->data(current, RoomIdRole).toString();
    if (roomId.isEmpty()) return;

    // If the selected item is a space, drill into it instead of opening a timeline.
    if (roomModel_->data(current, IsSpaceRole).toBool()) {
        spaceStack_.push_back(roomId.toStdString());
        refreshRoomList();
        return;
    }

    const std::string newId = roomId.toStdString();
    if (!currentRoomId_.empty() && currentRoomId_ != newId)
        client_.unsubscribe_room(currentRoomId_);

    currentRoomId_ = newId;

    for (const auto& r : rooms_)
        if (r.id == currentRoomId_) { updateRoomHeader(r); break; }

    auto res = client_.subscribe_room(currentRoomId_);
    if (!res) {
        statusBar()->showMessage(
            "Subscribe failed: " + QString::fromStdString(res.message), 4000);
        return;
    }
    client_.paginate_back(currentRoomId_, 50);
}

void MainWindow::onEventReceived(tesseract::Event* ev) {
    if (ev && ev->room_id == currentRoomId_)
        appendMessage(*ev);
    delete ev;
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
                    myUserId_ = client_.get_user_id();
                    client_.start_sync(bridge_.get());
                    statusBar()->showMessage("Reconnected");
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

void MainWindow::onTimelineReset(QString roomId) {
    if (roomId.toStdString() == currentRoomId_)
        clearMessages();
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
    msgEventWidgets_.clear();
    while (msgLayout_->count() > 0) {
        QLayoutItem* item = msgLayout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void MainWindow::showRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<const tesseract::RoomInfo*> sorted;
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(&r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(&r);

    roomModel_->clear();

    for (const auto* rp : sorted) {
        const auto& r = *rp;

        // Fetch and cache avatar on first sight.
        if (!r.avatar_url.empty()) {
            QString qurl = QString::fromStdString(r.avatar_url);
            if (!avatarCache_.contains(qurl)) {
                auto bytes = client_.fetch_avatar_bytes(r.id);
                if (!bytes.empty()) {
                    QPixmap pm;
                    pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                    static_cast<uint>(bytes.size()));
                    if (!pm.isNull())
                        avatarCache_[qurl] = pm.scaled(
                            kRoomAvatarSize, kRoomAvatarSize,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            }
        }

        auto* item = new QStandardItem;
        item->setData(QString::fromStdString(r.name), Qt::DisplayRole);
        item->setData(QString::fromStdString(r.last_message_body), LastMessageRole);
        item->setData(static_cast<int>(r.unread_count), UnreadCountRole);
        item->setData(QString::fromStdString(r.id), RoomIdRole);
        item->setData(r.is_space, IsSpaceRole);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

        if (!r.avatar_url.empty()) {
            QString qurl = QString::fromStdString(r.avatar_url);
            if (avatarCache_.contains(qurl))
                item->setData(avatarCache_[qurl], Qt::DecorationRole);
        }

        roomModel_->appendRow(item);
    }
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

void MainWindow::appendMessage(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    // Update in place if we already have this event (sender profile resolved / edit).
    QString qid = QString::fromStdString(ev.event_id);
    if (!qid.isEmpty() && msgEventWidgets_.contains(qid)) {
        QWidget* existing = msgEventWidgets_.value(qid);
        QString newName = QString::fromStdString(
            ev.sender_name.empty() ? ev.sender : ev.sender_name);
        if (auto* lbl = existing->findChild<QLabel*>("senderName"))
            lbl->setText(newName);
        QString avatarUrl = QString::fromStdString(ev.sender_avatar_url);
        if (!avatarUrl.isEmpty() && !userAvatarCache_.contains(avatarUrl)) {
            auto bytes = client_.fetch_media_bytes(ev.sender_avatar_url);
            if (!bytes.empty()) {
                QPixmap pm;
                pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                static_cast<uint>(bytes.size()));
                if (!pm.isNull())
                    userAvatarCache_[avatarUrl] = makeCirclePixmap(pm, kMsgAvatarSize);
            }
        }
        if (auto* lbl = existing->findChild<QLabel*>("avatar")) {
            if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl))
                lbl->setPixmap(userAvatarCache_[avatarUrl]);
            else
                lbl->setPixmap(makeInitialsPixmap(newName, kMsgAvatarSize));
        }
        if (ev.type == tesseract::EventType::Text) {
            if (auto* lbl = existing->findChild<QLabel*>("body"))
                lbl->setText(QString::fromStdString(ev.body).toHtmlEscaped());
        }
        return;
    }

    // Fetch/cache sender avatar.
    QString avatarUrl = QString::fromStdString(ev.sender_avatar_url);
    if (!avatarUrl.isEmpty() && !userAvatarCache_.contains(avatarUrl)) {
        auto bytes = client_.fetch_media_bytes(ev.sender_avatar_url);
        if (!bytes.empty()) {
            QPixmap pm;
            pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                            static_cast<uint>(bytes.size()));
            if (!pm.isNull())
                userAvatarCache_[avatarUrl] = makeCirclePixmap(pm, kMsgAvatarSize);
        }
    }

    // Fetch/cache image for image and sticker events.
    auto fetchAndCacheImage = [&](const std::string& url, int maxW, int maxH) {
        QString qurl = QString::fromStdString(url);
        if (qurl.isEmpty() || imageCache_.contains(qurl)) return;
        auto bytes = client_.fetch_media_bytes(url);
        if (bytes.empty()) return;
        QPixmap pm;
        pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                        static_cast<uint>(bytes.size()));
        if (!pm.isNull())
            imageCache_[qurl] = pm.scaled(maxW, maxH,
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };

    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        fetchAndCacheImage(img.image_url, kMaxImageWidth, kMaxImageHeight);
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        fetchAndCacheImage(s.image_url, kMaxStickerSize, kMaxStickerSize);
    }

    QWidget* row = createMessageRow(ev);
    if (!qid.isEmpty())
        msgEventWidgets_[qid] = row;
    autoScrollPending_ = true;
    msgLayout_->addWidget(row);
}

QWidget* MainWindow::createMessageRow(const tesseract::Event& ev) {
    QString sender    = QString::fromStdString(ev.sender);
    QString name      = QString::fromStdString(ev.sender_name);
    if (name.isEmpty()) name = sender;
    QString avatarUrl = QString::fromStdString(ev.sender_avatar_url);

    QString tsStr;
    if (ev.timestamp > 0) {
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(ev.timestamp));
        tsStr = dt.toString("hh:mm");
    }

    auto* row = new QWidget(msgContainer_);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 3, 0, 3);
    rowLayout->setSpacing(8);

    auto* avatarLabel = new QLabel(row);
    avatarLabel->setObjectName("avatar");
    avatarLabel->setFixedSize(kMsgAvatarSize, kMsgAvatarSize);
    if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl))
        avatarLabel->setPixmap(userAvatarCache_[avatarUrl]);
    else
        avatarLabel->setPixmap(makeInitialsPixmap(name, kMsgAvatarSize));

    auto* contentBox    = new QWidget(row);
    auto* contentLayout = new QVBoxLayout(contentBox);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(2);
    contentBox->setMaximumWidth(kMsgMaxWidth);

    auto* nameLabel = new QLabel(name, contentBox);
    nameLabel->setObjectName("senderName");
    nameLabel->setStyleSheet(
        "font-weight: bold; font-size: 12px; color: #555; background: transparent;");
    contentLayout->addWidget(nameLabel);

    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        QString imgUrl = QString::fromStdString(img.image_url);
        if (!imgUrl.isEmpty() && imageCache_.contains(imgUrl)) {
            auto* imgLabel = new QLabel(contentBox);
            imgLabel->setPixmap(imageCache_[imgUrl]);
            imgLabel->setScaledContents(false);
            contentLayout->addWidget(imgLabel);
        }
        // MSC2530: show body as caption only when a distinct filename was supplied.
        if (!img.filename.empty() && !img.body.empty()) {
            auto* bodyLabel = new QLabel(
                QString::fromStdString(img.body).toHtmlEscaped(), contentBox);
            bodyLabel->setWordWrap(true);
            bodyLabel->setStyleSheet("color: #1C1E21; background: transparent;");
            contentLayout->addWidget(bodyLabel);
        }
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        QString stickerUrl = QString::fromStdString(s.image_url);
        if (!stickerUrl.isEmpty() && imageCache_.contains(stickerUrl)) {
            auto* imgLabel = new QLabel(contentBox);
            imgLabel->setPixmap(imageCache_[stickerUrl]);
            imgLabel->setScaledContents(false);
            contentLayout->addWidget(imgLabel);
        }
        // Sticker body is alt-text only; never displayed.
    } else if (ev.type == tesseract::EventType::File) {
        const auto& file = static_cast<const tesseract::FileEvent&>(ev);
        QString fileText = QString("📎 %1").arg(
            QString::fromStdString(file.file_name));
        if (file.file_size > 0) {
            double kb = file.file_size / 1024.0;
            if (kb < 1024)
                fileText += QString(" (%1 KB)").arg(kb, 0, 'f', 1);
            else
                fileText += QString(" (%1 MB)").arg(kb / 1024.0, 0, 'f', 1);
        }
        auto* fileLabel = new QLabel(fileText, contentBox);
        fileLabel->setWordWrap(true);
        fileLabel->setStyleSheet("color: #1C1E21; background: transparent;");
        contentLayout->addWidget(fileLabel);
    } else {
        auto* bodyLabel = new QLabel(
            QString::fromStdString(ev.body).toHtmlEscaped(), contentBox);
        bodyLabel->setObjectName("body");
        bodyLabel->setWordWrap(true);
        bodyLabel->setStyleSheet("color: #1C1E21; background: transparent;");
        bodyLabel->setTextFormat(Qt::PlainText);
        contentLayout->addWidget(bodyLabel);
    }

    if (!tsStr.isEmpty()) {
        auto* tsLabel = new QLabel(tsStr, contentBox);
        tsLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
        tsLabel->setStyleSheet("color: #999; font-size: 10px; background: transparent;");
        contentLayout->addWidget(tsLabel);
    }

    rowLayout->addWidget(avatarLabel, 0, Qt::AlignTop);
    rowLayout->addWidget(contentBox);
    rowLayout->addStretch();
    return row;
}

} // namespace qt6
