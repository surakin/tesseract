#include "MainWindow.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QMetaType>

// Register types for queued (cross-thread) signal/slot delivery.
// Must happen before the signals are connected.
Q_DECLARE_METATYPE(matrix::Message)
Q_DECLARE_METATYPE(std::vector<matrix::RoomInfo>)

namespace qt6 {

// ---------------------------------------------------------------------------
// EventBridge – called on the Rust sync thread, emits queued signals
// ---------------------------------------------------------------------------

void EventBridge::on_message(const matrix::Message& msg) {
    emit messageReceived(msg);          // Qt::QueuedConnection by default
}

void EventBridge::on_rooms_updated(
    const std::vector<matrix::RoomInfo>& rooms)
{
    emit roomsUpdated(rooms);
}

void EventBridge::on_sync_error(
    const std::string& /*context*/,
    const std::string& description)
{
    emit syncError(QString::fromStdString(description));
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<matrix::Message>();
    qRegisterMetaType<std::vector<matrix::RoomInfo>>();

    setWindowTitle("Matrix Client");
    resize(1024, 768);

    // ---- Central widget ----
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* hLayout = new QHBoxLayout(central);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    // ---- Room list ----
    roomList_ = new QListWidget(this);
    roomList_->setFixedWidth(220);
    hLayout->addWidget(roomList_);

    // ---- Right panel ----
    auto* vLayout = new QVBoxLayout;
    vLayout->setContentsMargins(4, 4, 4, 4);
    vLayout->setSpacing(4);
    hLayout->addLayout(vLayout);

    msgView_ = new QTextEdit(this);
    msgView_->setReadOnly(true);
    vLayout->addWidget(msgView_, 1);

    auto* inputRow = new QHBoxLayout;
    inputLine_ = new QLineEdit(this);
    sendButton_ = new QPushButton("Send", this);
    inputRow->addWidget(inputLine_, 1);
    inputRow->addWidget(sendButton_);
    vLayout->addLayout(inputRow);

    // ---- Status bar ----
    statusBar()->showMessage("Not logged in");

    // ---- Connections ----
    connect(sendButton_, &QPushButton::clicked,
            this, &MainWindow::onSendClicked);

    connect(roomList_, &QListWidget::currentItemChanged,
            this, &MainWindow::onRoomSelected);

    // Build bridge and connect cross-thread signals (auto = QueuedConnection
    // when sender and receiver live in different threads).
    bridge_ = std::make_unique<EventBridge>(this);

    connect(bridge_.get(), &EventBridge::messageReceived,
            this,          &MainWindow::onMessageReceived);
    connect(bridge_.get(), &EventBridge::roomsUpdated,
            this,          &MainWindow::onRoomsUpdated);
    connect(bridge_.get(), &EventBridge::syncError,
            this,          &MainWindow::onSyncError);

    // Trigger login once the event loop is running.
    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    client_.stop_sync();
}

// ---------------------------------------------------------------------------

void MainWindow::doLogin() {
    // TODO: Replace with a proper login dialog widget.
    bool ok  = false;
    QString hs   = QInputDialog::getText(this, "Login", "Homeserver:",
                                         QLineEdit::Normal,
                                         "https://matrix.org", &ok);
    if (!ok) return;

    QString user = QInputDialog::getText(this, "Login", "Username:", QLineEdit::Normal, "", &ok);
    if (!ok) return;

    QString pass = QInputDialog::getText(this, "Login", "Password:", QLineEdit::Password, "", &ok);
    if (!ok) return;

    statusBar()->showMessage("Logging in…");

    auto res = client_.login(
        hs.toStdString(), user.toStdString(), pass.toStdString());

    if (res) {
        client_.start_sync(bridge_.get());
        statusBar()->showMessage("Connected");
        onRoomsUpdated(client_.list_rooms());
    } else {
        QMessageBox::critical(this, "Login failed",
                              QString::fromStdString(res.message));
        statusBar()->showMessage("Not logged in");
    }
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

    currentRoomId_ = rooms_[idx].id;
    msgView_->clear();

    auto msgs = client_.room_messages(currentRoomId_, 50);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        appendMessage(QString::fromStdString(it->sender),
                      QString::fromStdString(it->body));
}

void MainWindow::onMessageReceived(matrix::Message msg) {
    if (msg.room_id == currentRoomId_)
        appendMessage(QString::fromStdString(msg.sender),
                      QString::fromStdString(msg.body));

    // Refresh room list unread counts.
    populateRooms(client_.list_rooms());
}

void MainWindow::onRoomsUpdated(std::vector<matrix::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    populateRooms(rooms_);
}

void MainWindow::onSyncError(QString description) {
    statusBar()->showMessage("Sync error: " + description, 8000);
}

// ---------------------------------------------------------------------------

void MainWindow::populateRooms(const std::vector<matrix::RoomInfo>& rooms) {
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
