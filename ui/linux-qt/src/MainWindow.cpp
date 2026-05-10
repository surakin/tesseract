#include "MainWindow.h"
#include "LoginDialog.h"

#include <tesseract/session_store.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QMetaType>
#include <QTextDocument>
#include <QUrl>

Q_DECLARE_METATYPE(tesseract::Event*)
Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)

namespace qt6 {

// ---------------------------------------------------------------------------
// EventBridge – called on the Rust sync thread, emits queued signals
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
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<tesseract::Event*>();
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();

    setWindowTitle("Tesseract");
    resize(1024, 768);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* hLayout = new QHBoxLayout(central);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    roomList_ = new QListWidget(this);
    roomList_->setFixedWidth(220);
    hLayout->addWidget(roomList_);

    auto* vLayout = new QVBoxLayout;
    vLayout->setContentsMargins(4, 4, 4, 4);
    vLayout->setSpacing(4);
    hLayout->addLayout(vLayout);

    msgView_ = new QTextEdit(this);
    msgView_->setReadOnly(true);
    vLayout->addWidget(msgView_, 1);

    auto* inputRow = new QHBoxLayout;
    inputLine_  = new QLineEdit(this);
    sendButton_ = new QPushButton("Send", this);
    inputRow->addWidget(inputLine_, 1);
    inputRow->addWidget(sendButton_);
    vLayout->addLayout(inputRow);

    statusBar()->showMessage("Not logged in");

    connect(sendButton_, &QPushButton::clicked,
            this, &MainWindow::onSendClicked);
    connect(roomList_, &QListWidget::currentItemChanged,
            this, &MainWindow::onRoomSelected);

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

void MainWindow::doLogin() {
    if (auto saved = tesseract::SessionStore::load()) {
        statusBar()->showMessage("Restoring session…");
        auto res = client_.restore_session(*saved);
        if (res) {
            client_.start_sync(bridge_.get());
            statusBar()->showMessage("Connected");
            // Room list will arrive via on_rooms_updated once SyncService fires.
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

    tesseract::SessionStore::save(client_.export_session());
    client_.start_sync(bridge_.get());
    statusBar()->showMessage("Connected");
    // Room list arrives via on_rooms_updated from RoomListService.
}

void MainWindow::onSendClicked() {
    if (currentRoomId_.empty()) return;

    QString text = inputLine_->text().trimmed();
    if (text.isEmpty()) return;

    auto res = client_.send_message(currentRoomId_, text.toStdString());
    if (res) {
        inputLine_->clear();
    } else {
        statusBar()->showMessage(QString::fromStdString(res.message), 4000);
    }
}

void MainWindow::onRoomSelected(
    QListWidgetItem* current, QListWidgetItem* /*previous*/)
{
    if (!current) return;
    int idx = roomList_->row(current);
    if (idx < 0 || idx >= static_cast<int>(rooms_.size())) return;

    const std::string newRoomId = rooms_[idx].id;

    // Unsubscribe from previous room before subscribing to the new one.
    if (!currentRoomId_.empty() && currentRoomId_ != newRoomId)
        client_.unsubscribe_room(currentRoomId_);

    currentRoomId_ = newRoomId;

    // subscribe_room emits on_timeline_reset then cached messages; paginate
    // back to fill initial history (on_message callbacks arrive in order).
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
        appendEvent(*ev);
    delete ev;
}

void MainWindow::onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populateRooms(rooms_);
}

void MainWindow::onSyncError(QString context, QString description, bool soft_logout) {
    if (context == "sync_reconnect") {
        statusBar()->showMessage("Sync error: reconnecting…");
        client_.stop_sync();
        doLogin();
    } else if (context == "sync_auth_error") {
        if (soft_logout) {
            if (auto saved = tesseract::SessionStore::load()) {
                statusBar()->showMessage("Reconnecting session…");
                if (client_.restore_session(*saved)) {
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
        msgView_->clear();
}

// ---------------------------------------------------------------------------

void MainWindow::populateRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    roomList_->setIconSize(QSize(kRoomAvatarSize, kRoomAvatarSize));
    roomList_->clear();

    for (const auto& r : rooms) {
        // Populate avatar cache on first sight of this URL.
        if (!r.avatar_url.empty()) {
            QString qurl = QString::fromStdString(r.avatar_url);
            if (!avatarCache_.contains(qurl)) {
                auto bytes = client_.fetch_avatar_bytes(r.id);
                if (!bytes.empty()) {
                    QPixmap pm;
                    pm.loadFromData(
                        reinterpret_cast<const uchar*>(bytes.data()),
                        static_cast<uint>(bytes.size()));
                    if (!pm.isNull())
                        avatarCache_[qurl] = pm.scaled(
                            kRoomAvatarSize, kRoomAvatarSize,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            }
        }

        QString label = QString::fromStdString(r.name);
        if (r.unread_count > 0)
            label += QString(" (%1)").arg(r.unread_count);

        auto* item = new QListWidgetItem(label);
        if (!r.avatar_url.empty()) {
            QString qurl = QString::fromStdString(r.avatar_url);
            if (avatarCache_.contains(qurl))
                item->setIcon(QIcon(avatarCache_[qurl]));
        }
        roomList_->addItem(item);
    }
}

void MainWindow::appendEvent(const tesseract::Event& ev) {
    if (ev.type == tesseract::EventType::Unhandled) {
        // Skip unhandled event types for now
        return;
    }

    QString sender    = QString::fromStdString(ev.sender);
    QString name      = QString::fromStdString(ev.sender_name);
    if (name.isEmpty()) name = sender;
    QString avatarUrl = QString::fromStdString(ev.sender_avatar_url);

    // Fetch and cache sender avatar on first sight of this mxc URL.
    if (!avatarUrl.isEmpty() && !userAvatarCache_.contains(avatarUrl)) {
        auto bytes = client_.fetch_media_bytes(ev.sender_avatar_url);
        if (!bytes.empty()) {
            QPixmap pm;
            pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                            static_cast<uint>(bytes.size()));
            if (!pm.isNull())
                userAvatarCache_[avatarUrl] = pm.scaled(
                    kUserAvatarSize, kUserAvatarSize,
                    Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    // Re-register the avatar resource with the document.
    if (userAvatarCache_.contains(avatarUrl)) {
        msgView_->document()->addResource(
            QTextDocument::ImageResource,
            QUrl(avatarUrl),
            userAvatarCache_[avatarUrl]);
    }

    QString html;

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
                    if (img.width > 0 && img.height > 0) {
                        imageCache_[imgUrl] = pm.scaled(
                            static_cast<int>(std::min(static_cast<uint64_t>(kMaxImageSize), img.width)),
                            static_cast<int>(std::min(static_cast<uint64_t>(kMaxImageSize), img.height)),
                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    } else {
                        imageCache_[imgUrl] = pm.scaled(
                            kMaxImageSize, kMaxImageSize,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                }
            }
        }

        QString avatarImg;
        if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl)) {
            avatarImg = QString("<img src='%1' width='%2' height='%2'> ")
                .arg(avatarUrl.toHtmlEscaped())
                .arg(kUserAvatarSize);
        }

        if (imageCache_.contains(imgUrl)) {
            msgView_->document()->addResource(
                QTextDocument::ImageResource,
                QUrl(imgUrl),
                imageCache_[imgUrl]);
            html = avatarImg +
                   QString("<b>%1:</b><br>"
                           "<img src='%2' width='%3' height='%4'><br>")
                   .arg(name.toHtmlEscaped())
                   .arg(imgUrl.toHtmlEscaped())
                   .arg(imageCache_[imgUrl].width())
                   .arg(imageCache_[imgUrl].height());
            if (!img.body.empty()) {
                html += QString::fromStdString(img.body).toHtmlEscaped();
            }
        } else {
            html = avatarImg + QString("<b>%1:</b> [Image] %2")
                .arg(name.toHtmlEscaped())
                .arg(QString::fromStdString(img.body).toHtmlEscaped());
        }
    } else if (ev.type == tesseract::EventType::File) {
        const auto& file = static_cast<const tesseract::FileEvent&>(ev);
        QString fileUrl = QString::fromStdString(file.file_url);
        QString fileLink;
        if (!fileUrl.isEmpty()) {
            fileLink = QString(" <a href='%1'>📎 %2</a>")
                .arg(fileUrl.toHtmlEscaped())
                .arg(QString::fromStdString(file.file_name).toHtmlEscaped());
        } else {
            fileLink = QString(" 📎 %1")
                .arg(QString::fromStdString(file.file_name).toHtmlEscaped());
        }

        QString avatarImg;
        if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl)) {
            avatarImg = QString("<img src='%1' width='%2' height='%2'> ")
                .arg(avatarUrl.toHtmlEscaped())
                .arg(kUserAvatarSize);
        }
        html = avatarImg + QString("<b>%1:</b>%2 %3")
            .arg(name.toHtmlEscaped())
            .arg(fileLink)
            .arg(QString::fromStdString(file.body).toHtmlEscaped());
    } else if (ev.type == tesseract::EventType::Text) {
        // TextEvent
        QString avatarImg;
        if (!avatarUrl.isEmpty() && userAvatarCache_.contains(avatarUrl)) {
            avatarImg = QString("<img src='%1' width='%2' height='%2'> ")
                .arg(avatarUrl.toHtmlEscaped())
                .arg(kUserAvatarSize);
        }
        html = avatarImg + "<b>" + name.toHtmlEscaped() + ":</b> " +
               QString::fromStdString(ev.body).toHtmlEscaped();
    } else {
        // Unknown non-unhandled type — shouldn't happen, but skip gracefully
        return;
    }

    msgView_->append(html);

    QScrollBar* sb = msgView_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

} // namespace qt6
