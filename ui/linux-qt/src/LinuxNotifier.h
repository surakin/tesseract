#pragma once
#include <tesseract/notifier.h>
#include <QDBusInterface>
#include <QObject>
#include <functional>
#include <string>
#include <unordered_map>

class LinuxNotifierQt final : public QObject, public tesseract::INotifier
{
    Q_OBJECT
public:
    // Callback: (room_id, activation_token). Token is empty when unavailable;
    // non-empty on Wayland via the XDG Desktop Portal ActionInvoked signal.
    explicit LinuxNotifierQt(
        std::function<void(std::string, std::string)> on_activate,
        QObject* parent = nullptr);
    ~LinuxNotifierQt() override = default;
    void notify(const tesseract::Notification& n) override;

private slots:
    void onActionInvoked(uint id, const QString& action);
    void onNotificationClosed(uint id, uint reason);
    void onPortalActionInvoked(const QString& notification_id,
                               const QString& action,
                               const QVariantList& parameter);

private:
    bool use_portal() const;

    QDBusInterface iface_;
    QDBusInterface portal_;
    std::function<void(std::string, std::string)> on_activate_;
    std::unordered_map<uint32_t, std::string> id_to_room_;
    // Portal notifications use string IDs (sanitized room_id).
    std::unordered_map<std::string, std::string> portal_id_to_room_;
};
