#include "LinuxNotifier.h"
#include <QDBusConnection>
#include <QDBusReply>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include "../../shared/linux_portal.h"

namespace
{

// Several notification daemons (dunst, mako, GNOME Shell) render a subset
// of Pango markup in the body. Server-supplied message text must be escaped
// so it can't inject markup / garble the notification.
QString escape_markup(const std::string& s)
{
    return QString::fromStdString(s).toHtmlEscaped();
}

// Write pic bytes (PNG/JPEG/WebP) to a fixed temp path and return a
// file:// URI. Returns an empty string on failure.
// A fixed path is safe because daemons read image-path synchronously when
// they handle the Notify call, so there is no race between notifications.
QString write_image_path(const std::vector<uint8_t>& pic)
{
    if (pic.empty())
    {
        return {};
    }
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(pic.data()),
                          static_cast<int>(pic.size())))
    {
        return {};
    }
    img = img.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString path =
        QDir::tempPath() + QStringLiteral("/tesseract-notif.png");
    if (!img.save(path, "PNG"))
    {
        return {};
    }
    return QStringLiteral("file://") + path;
}

} // namespace

LinuxNotifierQt::LinuxNotifierQt(
    std::function<void(std::string, std::string)> on_activate, QObject* parent)
    : QObject(parent),
      iface_("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
             "org.freedesktop.Notifications", QDBusConnection::sessionBus()),
      portal_(
          "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.Notification", QDBusConnection::sessionBus()),
      on_activate_(std::move(on_activate))
{
    // Freedesktop notification signals (no activation token available here).
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "ActionInvoked", this,
        SLOT(onActionInvoked(uint, const QString&)));

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "NotificationClosed", this,
        SLOT(onNotificationClosed(uint, uint)));

    // XDG Desktop Portal notification signal — includes an xdg_activation_v1
    // token on Wayland, enabling reliable window focus after notification click.
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Notification", "ActionInvoked", this,
        SLOT(onPortalActionInvoked(QString, QString, QVariantList)));
}

bool LinuxNotifierQt::use_portal() const
{
    // Use the portal on Wayland for activation-token support, or inside Flatpak
    // where direct D-Bus calls to the notification daemon are blocked.
    return qEnvironmentVariableIsSet("FLATPAK_ID") ||
           QGuiApplication::platformName() == QLatin1String("wayland");
}

void LinuxNotifierQt::notify(const tesseract::Notification& n)
{
    // Prefer the message image / sticker, fall back to the room avatar.
    const std::vector<uint8_t>& pic =
        !n.image_bytes.empty() ? n.image_bytes : n.avatar_bytes;

    if (use_portal())
    {
        const QString pid = QString::fromStdString(
            tesseract::linux_portal::sanitize_notification_id(n.room_id));
        // Record mapping so onPortalActionInvoked can look up the room.
        portal_id_to_room_[pid.toStdString()] = n.room_id;
        QVariantMap portalMap{{"title", escape_markup(n.sender)},
                              {"body", escape_markup(n.body)},
                              {"default-action", QStringLiteral("default")}};
        // Portal "icon" uses (sv); themed icons are simplest to marshal.
        // Avatar bytes require GIcon serialisation which is not straightforward
        // over Qt D-Bus, so skip for now — the app icon fallback is fine.
        portal_.call("AddNotification", pid, portalMap);
        return;
    }

    // Write the avatar / image to a temp file and pass it as image-path.
    // This is simpler and more universally supported than the image-data
    // (iiibiiay) raw-pixel hint, which many daemons mishandle when the
    // value arrives as a bare QDBusArgument variant.
    QVariantMap hints;
    const QString img_path = write_image_path(pic);
    if (!img_path.isEmpty())
    {
        hints[QStringLiteral("image-path")] = img_path;
    }

    // Always pass replaces_id=0 so every notification generates a fresh popup.
    // Using replaces causes the daemon to update the existing toast in place
    // without re-triggering the animation or sound, making subsequent messages
    // from the same room invisible to the user.
    QDBusReply<uint> reply =
        iface_.call("Notify", QString("Tesseract"), 0u, QString(""),
                    escape_markup(n.sender), escape_markup(n.body),
                    QStringList{"default", "Open"}, hints, 5000);

    if (reply.isValid())
    {
        id_to_room_[reply.value()] = n.room_id;
    }
}

void LinuxNotifierQt::onActionInvoked(uint id, const QString& /*action*/)
{
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end())
    {
        on_activate_(it->second, ""); // no activation token via legacy D-Bus
    }
}

void LinuxNotifierQt::onNotificationClosed(uint id, uint /*reason*/)
{
    id_to_room_.erase(id);
}

void LinuxNotifierQt::onPortalActionInvoked(const QString& notification_id,
                                            const QString& /*action*/,
                                            const QVariantList& parameter)
{
    auto it = portal_id_to_room_.find(notification_id.toStdString());
    if (it == portal_id_to_room_.end())
    {
        return;
    }

    // The portal sends ActionInvoked with an av (array of variants) whose
    // elements are, in order: optional target, platform-data a{sv} (portal
    // >= 1.16, contains "activation-token" on Wayland), optional response.
    // Search for the first a{sv} element that carries the token.
    std::string token;
    const auto key = QStringLiteral("activation-token");
    for (const QVariant& v : parameter)
    {
        const QVariantMap m = qdbus_cast<QVariantMap>(v);
        if (m.contains(key))
        {
            token = m.value(key).toString().toStdString();
            break;
        }
    }

    on_activate_(it->second, std::move(token));
}
