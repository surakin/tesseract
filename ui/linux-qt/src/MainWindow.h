#pragma once
#include <QMainWindow>
#include <QListWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QSplitter>

#include <matrix/client.hpp>
#include <matrix/event_handler.hpp>

#include <memory>
#include <vector>

namespace qt6 {

/// Qt signal/slot bridge for SDK callbacks (runs on background thread → queued).
class EventBridge final : public QObject, public matrix::IEventHandler {
    Q_OBJECT
public:
    explicit EventBridge(QObject* parent = nullptr) : QObject(parent) {}

    // IEventHandler – called on the sync thread
    void on_message(const matrix::Message& msg) override;
    void on_rooms_updated(const std::vector<matrix::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description) override;

signals:
    void messageReceived(matrix::Message msg);
    void roomsUpdated(std::vector<matrix::RoomInfo> rooms);
    void syncError(QString description);
};

// ---------------------------------------------------------------------------

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSendClicked();
    void onRoomSelected(QListWidgetItem* current, QListWidgetItem* previous);
    void onMessageReceived(matrix::Message msg);
    void onRoomsUpdated(std::vector<matrix::RoomInfo> rooms);
    void onSyncError(QString description);

private:
    void doLogin();
    void populateRooms(const std::vector<matrix::RoomInfo>& rooms);
    void appendMessage(const QString& sender, const QString& body);

    QSplitter*   splitter_    = nullptr;
    QListWidget* roomList_    = nullptr;
    QTextEdit*   msgView_     = nullptr;
    QLineEdit*   inputLine_   = nullptr;
    QPushButton* sendButton_  = nullptr;

    matrix::MatrixClient          client_;
    std::unique_ptr<EventBridge>  bridge_;
    std::vector<matrix::RoomInfo> rooms_;
    std::string                   currentRoomId_;
};

} // namespace qt6
