#include "LinuxNotifier.h"
#include <QDBusConnection>
#include <QDBusReply>

LinuxNotifierQt::LinuxNotifierQt(std::function<void(std::string)> on_activate,
                                   QObject* parent)
    : QObject(parent)
    , iface_("org.freedesktop.Notifications",
             "/org/freedesktop/Notifications",
             "org.freedesktop.Notifications",
             QDBusConnection::sessionBus())
    , portal_("org.freedesktop.portal.Desktop",
              "/org/freedesktop/portal/desktop",
              "org.freedesktop.portal.Notification",
              QDBusConnection::sessionBus())
    , on_activate_(std::move(on_activate))
{
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "ActionInvoked",
        this, SLOT(onActionInvoked(uint, const QString&)));

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed",
        this, SLOT(onNotificationClosed(uint, uint)));
}

bool LinuxNotifierQt::use_portal() const {
    return qEnvironmentVariableIsSet("FLATPAK_ID");
}

void LinuxNotifierQt::notify(const tesseract::Notification& n) {
    if (use_portal()) {
        portal_.call(
            "AddNotification",
            QString::fromStdString(n.room_id),
            QVariantMap{
                { "title", QString::fromStdString(n.sender) },
                { "body",  QString::fromStdString(n.body) }
            });
        return;
    }

    const uint32_t replaces = room_to_id_.count(n.room_id)
                              ? room_to_id_.at(n.room_id) : 0u;
    QDBusReply<uint> reply = iface_.call(
        "Notify",
        QString("Tesseract"),
        replaces,
        QString("tesseract"),
        QString::fromStdString(n.sender),
        QString::fromStdString(n.body),
        QStringList{ "default", "Open" },
        QVariantMap{},
        5000);

    if (reply.isValid()) {
        const uint32_t id = reply.value();
        id_to_room_[id]        = n.room_id;
        room_to_id_[n.room_id] = id;
    }
}

void LinuxNotifierQt::onActionInvoked(uint id, const QString& /*action*/) {
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end())
        on_activate_(it->second);
}

void LinuxNotifierQt::onNotificationClosed(uint id, uint /*reason*/) {
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end()) {
        room_to_id_.erase(it->second);
        id_to_room_.erase(it);
    }
}
