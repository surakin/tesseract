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

    const std::string newId = roomId.toStdString();
    if (!currentRoomId_.empty() && currentRoomId_ != newId)
        client_.unsubscribe_room(currentRoomId_);

    currentRoomId_ = newId;

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
        appendMessageBubble(*ev);
    delete ev;
}

void MainWindow::onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populateRooms(rooms_);
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

void MainWindow::clearMessages() {
    msgEventWidgets_.clear();
    while (msgLayout_->count() > 0) {
        QLayoutItem* item = msgLayout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void MainWindow::populateRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    roomModel_->clear();

    for (const auto& r : rooms) {
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
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

        if (!r.avatar_url.empty()) {
            QString qurl = QString::fromStdString(r.avatar_url);
            if (avatarCache_.contains(qurl))
                item->setData(avatarCache_[qurl], Qt::DecorationRole);
        }

        roomModel_->appendRow(item);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::appendMessageBubble(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) return;

    // Update in place if we already have this event (sender profile resolved / edit).
    QString qid = QString::fromStdString(ev.event_id);
    if (!qid.isEmpty() && msgEventWidgets_.contains(qid)) {
        QWidget* existing = msgEventWidgets_.value(qid);
        bool isOwn = (!myUserId_.empty() && ev.sender == myUserId_);
        if (!isOwn) {
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

    // Fetch/cache image for image events.
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        QString imgUrl = QString::fromStdString(img.image_url);
        if (!imgUrl.isEmpty() && !imageCache_.contains(imgUrl)) {
            auto bytes = client_.fetch_media_bytes(img.image_url);
            if (!bytes.empty()) {
                QPixmap pm;
                pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                static_cast<uint>(bytes.size()));
                if (!pm.isNull()) {
                    imageCache_[imgUrl] = pm.scaled(
                        kMaxImageWidth, kMaxImageHeight,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            }
        }
    }

    QWidget* row = createBubbleRow(ev);
    if (!qid.isEmpty())
        msgEventWidgets_[qid] = row;
    msgLayout_->addWidget(row);

    // Scroll to bottom after layout pass.
    QTimer::singleShot(0, this, [this]() {
        msgScrollArea_->verticalScrollBar()->setValue(
            msgScrollArea_->verticalScrollBar()->maximum());
    });
}

QWidget* MainWindow::createBubbleRow(const tesseract::Event& ev) {
    bool isOwn = (!myUserId_.empty() && ev.sender == myUserId_);

    QString sender    = QString::fromStdString(ev.sender);
    QString name      = QString::fromStdString(ev.sender_name);
    if (name.isEmpty()) name = sender;
    QString avatarUrl = QString::fromStdString(ev.sender_avatar_url);

    // Timestamp string.
    QString tsStr;
    if (ev.timestamp > 0) {
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(ev.timestamp));
        tsStr = dt.toString("hh:mm");
    }

    // ---- Outer row ----
    auto* row = new QWidget(msgContainer_);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 3, 0, 3);
    rowLayout->setSpacing(8);

    // ---- Bubble frame ----
    auto* bubble = new QFrame;
    bubble->setMaximumWidth(kBubbleMaxWidth);
    if (isOwn) {
        bubble->setStyleSheet(
            "QFrame { background-color: #0084FF; border-radius: 18px; }");
    } else {
        bubble->setStyleSheet(
            "QFrame { background-color: #E4E6EB; border-radius: 18px; }");
    }

    auto* bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(14, 10, 14, 8);
    bubbleLayout->setSpacing(4);

    // ---- Bubble content ----
    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        QString imgUrl = QString::fromStdString(img.image_url);
        if (!imgUrl.isEmpty() && imageCache_.contains(imgUrl)) {
            auto* imgLabel = new QLabel(bubble);
            imgLabel->setPixmap(imageCache_[imgUrl]);
            imgLabel->setScaledContents(false);
            bubbleLayout->addWidget(imgLabel);
        }
        if (!img.body.empty()) {
            auto* bodyLabel = new QLabel(
                QString::fromStdString(img.body).toHtmlEscaped(), bubble);
            bodyLabel->setWordWrap(true);
            bodyLabel->setStyleSheet(isOwn ? "color: white; background: transparent;"
                                           : "color: #1C1E21; background: transparent;");
            bubbleLayout->addWidget(bodyLabel);
        }
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
        auto* fileLabel = new QLabel(fileText, bubble);
        fileLabel->setWordWrap(true);
        fileLabel->setStyleSheet(isOwn ? "color: white; background: transparent;"
                                       : "color: #1C1E21; background: transparent;");
        bubbleLayout->addWidget(fileLabel);
    } else {
        // Text
        auto* bodyLabel = new QLabel(
            QString::fromStdString(ev.body).toHtmlEscaped(), bubble);
        bodyLabel->setObjectName("body");
        bodyLabel->setWordWrap(true);
        bodyLabel->setStyleSheet(isOwn ? "color: white; background: transparent;"
                                       : "color: #1C1E21; background: transparent;");
        bodyLabel->setTextFormat(Qt::PlainText);
        bubbleLayout->addWidget(bodyLabel);
    }

    // Timestamp
    if (!tsStr.isEmpty()) {
        auto* tsLabel = new QLabel(tsStr, bubble);
        tsLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
        tsLabel->setStyleSheet(
            isOwn ? "color: rgba(255,255,255,0.7); font-size: 10px; background: transparent;"
                  : "color: #999; font-size: 10px; background: transparent;");
        bubbleLayout->addWidget(tsLabel);
    }

    // ---- Row assembly ----
    if (isOwn) {
        rowLayout->addStretch();
        rowLayout->addWidget(bubble);
    } else {
        // Sender avatar
        auto* avatarLabel = new QLabel(row);
        avatarLabel->setObjectName("avatar");
        avatarLabel->setFixedSize(kMsgAvatarSize, kMsgAvatarSize);
        if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl))
            avatarLabel->setPixmap(userAvatarCache_[avatarUrl]);
        else
            avatarLabel->setPixmap(makeInitialsPixmap(name, kMsgAvatarSize));

        // Container: name + bubble stacked vertically
        auto* otherBox    = new QWidget(row);
        auto* otherLayout = new QVBoxLayout(otherBox);
        otherLayout->setContentsMargins(0, 0, 0, 0);
        otherLayout->setSpacing(2);

        auto* nameLabel = new QLabel(name, otherBox);
        nameLabel->setObjectName("senderName");
        nameLabel->setStyleSheet(
            "font-weight: bold; font-size: 12px; color: #555; background: transparent;");
        otherLayout->addWidget(nameLabel);
        otherLayout->addWidget(bubble);

        rowLayout->addWidget(avatarLabel, 0, Qt::AlignTop);
        rowLayout->addWidget(otherBox);
        rowLayout->addStretch();
    }

    return row;
}

} // namespace qt6
