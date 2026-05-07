#include "MainWindow.h"
#include "LoginDialog.h"

#include <tesseract/session_store.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QMetaType>

Q_DECLARE_METATYPE(tesseract::Message)
Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)

namespace qt6 {

// ---------------------------------------------------------------------------
// EventBridge – called on the Rust sync thread, emits queued signals
// ---------------------------------------------------------------------------

void EventBridge::on_message(const tesseract::Message& msg) {
    emit messageReceived(msg);
}

void EventBridge::on_rooms_updated(
    const std::vector<tesseract::RoomInfo>& rooms)
{
    emit roomsUpdated(rooms);
}

void EventBridge::on_sync_error(
    const std::string& /*context*/,
    const std::string& description)
{
    emit syncError(QString::fromStdString(description));
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
    qRegisterMetaType<tesseract::Message>();
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

    connect(bridge_.get(), &EventBridge::messageReceived,
            this,          &MainWindow::onMessageReceived);
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

void MainWindow::onMessageReceived(tesseract::Message msg) {
    if (msg.room_id == currentRoomId_)
        appendMessage(QString::fromStdString(msg.sender),
                      QString::fromStdString(msg.body));
    // Room list unread counts update via on_rooms_updated from RoomListService.
}

void MainWindow::onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populateRooms(rooms_);
}

void MainWindow::onSyncError(QString description) {
    statusBar()->showMessage("Sync error: " + description, 8000);
}

void MainWindow::onTimelineReset(QString roomId) {
    if (roomId.toStdString() == currentRoomId_)
        msgView_->clear();
}

// ---------------------------------------------------------------------------

void MainWindow::populateRooms(const std::vector<tesseract::RoomInfo>& rooms) {
    roomList_->clear();
    for (const auto& r : rooms) {
        QString label = QString::fromStdString(r.name);
        if (r.unread_count > 0)
            label += QString(" (%1)").arg(r.unread_count);
        roomList_->addItem(label);
    }
}

void MainWindow::appendMessage(
    const QString& sender,
    const QString& body)
{
    msgView_->append("<b>" + sender.toHtmlEscaped() + ":</b> " +
                     body.toHtmlEscaped());

    QScrollBar* sb = msgView_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

} // namespace qt6
