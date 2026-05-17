#include "LinuxNotifier.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusReply>
#include <QImage>
#include <cctype>

namespace {
// XDG portal notification ids must match [a-zA-Z0-9_-]+. Matrix room ids
// contain '!', ':' and '.', so map anything outside the allowed set to '_'.
QString sanitize_portal_id(const std::string& s)
{
    QString out;
    out.reserve(static_cast<int>(s.size()));
    for (unsigned char c : s)
    {
        out += (std::isalnum(c) || c == '_' || c == '-')
                   ? QChar(c) : QChar('_');
    }
    return out.isEmpty() ? QStringLiteral("_") : out;
}

// Several notification daemons (dunst, mako, GNOME Shell) render a subset
// of Pango markup in the body. Server-supplied message text must be escaped
// so it can't inject markup / garble the notification.
QString escape_markup(const std::string& s)
{
    return QString::fromStdString(s).toHtmlEscaped();
}
} // namespace

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

bool LinuxNotifierQt::use_portal() const
{
    return qEnvironmentVariableIsSet("FLATPAK_ID");
}

void LinuxNotifierQt::notify(const tesseract::Notification& n)
{
    // freedesktop notifications have a single image slot: prefer the
    // message image / sticker (already privacy-gated upstream), fall back
    // to the room avatar.
    const std::vector<uint8_t>& pic =
        !n.image_bytes.empty() ? n.image_bytes : n.avatar_bytes;

    // Build image-data hint: decode pic, convert to RGBA8888, marshal as
    // the (iiibiiay) D-Bus struct the freedesktop Notify spec defines.
    QVariantMap hints;
    if (!pic.empty())
    {
        QImage img;
        if (img.loadFromData(
                reinterpret_cast<const uchar*>(pic.data()),
                static_cast<int>(pic.size())))
        {
            img = img.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
            QDBusArgument arg;
            arg.beginStructure();
            arg << img.width() << img.height() << img.bytesPerLine()
                << true << 8 << 4;
            arg.beginArray(qMetaTypeId<uchar>());
            const uchar*  bits = img.constBits();
            const qsizetype sz = img.sizeInBytes();
            for (qsizetype i = 0; i < sz; ++i)
            {
                arg << bits[i];
            }
            arg.endArray();
            arg.endStructure();
            hints[QStringLiteral("image-data")] = QVariant::fromValue(arg);
        }
    }

    if (use_portal())
    {
        QVariantMap portalMap{
            { "title", escape_markup(n.sender) },
            { "body",  escape_markup(n.body) }
        };
        if (!pic.empty())
        {
            // bytes-icon: pass the raw encoded bytes directly.
            portalMap["icon"] = QVariantList{
                QStringLiteral("bytes-icon"),
                QByteArray(reinterpret_cast<const char*>(pic.data()),
                           static_cast<int>(pic.size()))
            };
        }
        portal_.call("AddNotification",
                     sanitize_portal_id(n.room_id), portalMap);
        return;
    }

    const uint32_t replaces = room_to_id_.count(n.room_id)
                              ? room_to_id_.at(n.room_id) : 0u;
    QDBusReply<uint> reply = iface_.call(
        "Notify",
        QString("Tesseract"),
        replaces,
        QString("tesseract"),
        escape_markup(n.sender),
        escape_markup(n.body),
        QStringList{ "default", "Open" },
        hints,
        5000);

    if (reply.isValid())
    {
        const uint32_t id = reply.value();
        id_to_room_[id]        = n.room_id;
        room_to_id_[n.room_id] = id;
    }
}

void LinuxNotifierQt::onActionInvoked(uint id, const QString& /*action*/)
{
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end())
    {
        on_activate_(it->second);
    }
}

void LinuxNotifierQt::onNotificationClosed(uint id, uint /*reason*/)
{
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end())
    {
        room_to_id_.erase(it->second);
        id_to_room_.erase(it);
    }
}
