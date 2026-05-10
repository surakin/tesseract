#pragma once
#include <QMainWindow>
#include <QHash>
#include <QListWidget>
#include <QPixmap>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QSplitter>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>

#include <memory>
#include <vector>

namespace qt6 {

/// Qt signal/slot bridge for SDK callbacks (runs on background thread → queued).
class EventBridge final : public QObject, public tesseract::IEventHandler {
    Q_OBJECT
public:
    explicit EventBridge(QObject* parent = nullptr) : QObject(parent) {}

    // IEventHandler – called on the sync thread
    void on_message(tesseract::Event* ev) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_timeline_reset(const std::string& room_id) override;
    void on_session_saved(const std::string& session_json) override;

signals:
    void eventReceived(tesseract::Event* ev);
    void roomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void syncError(QString context, QString description, bool soft_logout);
    void timelineReset(QString roomId);
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
    void onEventReceived(tesseract::Event* ev);
    void onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void onSyncError(QString context, QString description, bool soft_logout);
    void onTimelineReset(QString roomId);

private:
    void doLogin();
    void populateRooms(const std::vector<tesseract::RoomInfo>& rooms);
    void appendEvent(const tesseract::Event& ev);

    static constexpr int kRoomAvatarSize = 36;
    static constexpr int kUserAvatarSize = 24;
    static constexpr int kMaxImageSize = 400;

    QSplitter*   splitter_    = nullptr;
    QListWidget* roomList_    = nullptr;
    QTextEdit*   msgView_     = nullptr;
    QLineEdit*   inputLine_   = nullptr;
    QPushButton* sendButton_  = nullptr;

    tesseract::Client             client_;
    std::unique_ptr<EventBridge>  bridge_;
    std::vector<tesseract::RoomInfo> rooms_;
    std::string                   currentRoomId_;
    /// avatar_url → scaled QPixmap; keyed on URL so a changed avatar causes re-fetch.
    QHash<QString, QPixmap>       avatarCache_;
    /// sender_avatar_url → scaled QPixmap for inline message avatars.
    QHash<QString, QPixmap>       userAvatarCache_;
    /// image_url → scaled QPixmap for inline image messages.
    QHash<QString, QPixmap>       imageCache_;
};

} // namespace qt6