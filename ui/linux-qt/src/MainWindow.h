#pragma once
#include <QMainWindow>
#include <QHash>
#include <QListView>
#include <QPixmap>
#include <QScrollArea>
#include <QTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QStandardItemModel>
#include <QVBoxLayout>

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

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onSendClicked();
    void onRoomSelectionChanged(const QModelIndex& current, const QModelIndex& previous);
    void onEventReceived(tesseract::Event* ev);
    void onRoomsUpdated(std::vector<tesseract::RoomInfo> rooms);
    void onSyncError(QString context, QString description, bool soft_logout);
    void onTimelineReset(QString roomId);

private:
    void     doLogin();
    void     populateRooms(const std::vector<tesseract::RoomInfo>& rooms);
    void     appendMessageBubble(const tesseract::Event& ev);
    void     clearMessages();
    QWidget* createBubbleRow(const tesseract::Event& ev);
    QPixmap  makeCirclePixmap(const QPixmap& src, int size);
    QPixmap  makeInitialsPixmap(const QString& name, int size);

    static constexpr int kRoomAvatarSize  = 36;
    static constexpr int kMsgAvatarSize   = 32;
    static constexpr int kMaxImageWidth   = 320;
    static constexpr int kMaxImageHeight  = 200;
    static constexpr int kBubbleMaxWidth  = 520;

    QListView*           roomList_       = nullptr;
    QStandardItemModel*  roomModel_      = nullptr;
    QScrollArea*         msgScrollArea_  = nullptr;
    QWidget*             msgContainer_   = nullptr;
    QVBoxLayout*         msgLayout_      = nullptr;
    QTextEdit*           composeEdit_    = nullptr;
    QPushButton*         sendButton_     = nullptr;

    tesseract::Client             client_;
    std::unique_ptr<EventBridge>  bridge_;
    std::vector<tesseract::RoomInfo> rooms_;
    std::string                   currentRoomId_;
    std::string                   myUserId_;
    QHash<QString, QPixmap>       avatarCache_;
    QHash<QString, QPixmap>       userAvatarCache_;
    QHash<QString, QPixmap>       imageCache_;
};

} // namespace qt6
