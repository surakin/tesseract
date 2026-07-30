#include "LinuxNotifier.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusReply>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMetaType>
#include "../../shared/linux_portal.h"
#include "tk/i18n.h"

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
    std::function<void(std::string, std::string)> on_activate,
    std::function<void(std::string, std::string, std::string)> on_reply,
    QObject* parent)
    : QObject(parent),
      iface_("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
             "org.freedesktop.Notifications", QDBusConnection::sessionBus()),
      portal_(
          "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.Notification", QDBusConnection::sessionBus()),
      on_activate_(std::move(on_activate)),
      on_reply_(std::move(on_reply))
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

    // KDE Plasma-only extension: fired when the user submits text via the
    // inline-reply action (see ui/shared/linux_notification_reply.h). Other
    // daemons never emit this signal, so this connection is simply dormant
    // there — no capability check needed before subscribing.
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "NotificationReplied", this,
        SLOT(onNotificationReplied(uint, const QString&)));

    // XDG Desktop Portal notification signal — includes an xdg_activation_v1
    // token on Wayland, enabling reliable window focus after notification click.
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Notification", "ActionInvoked", this,
        SLOT(onPortalActionInvoked(QString, QString, QVariantList)));

    // Capability probes, done once here rather than lazily from notify() —
    // see the header comment on legacy_reply_supported_/portal_reply_supported_
    // for why a lazily-cached shared static is unsafe for this.
    QDBusReply<QStringList> caps = iface_.call("GetCapabilities");
    if (caps.isValid())
    {
        std::vector<std::string> v;
        v.reserve(static_cast<std::size_t>(caps.value().size()));
        for (const QString& s : caps.value())
        {
            v.push_back(s.toStdString());
        }
        legacy_reply_supported_ = tesseract::linux_notify::supports_inline_reply(v);
    }

    const QVariant portal_version = portal_.property("version");
    portal_reply_supported_ =
        portal_version.isValid() &&
        portal_version.toUInt() >= tesseract::linux_notify::kPortalReplyMinVersion;
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
        // Record mapping so onPortalActionInvoked can look up the room. Each
        // notification for the same room reuses the same id (replacing the
        // prior one), so this just tracks the latest event_id per room.
        portal_id_to_room_[pid.toStdString()] = {n.room_id, n.event_id};
        QVariantMap portalMap{{"title", escape_markup(n.sender)},
                              {"body", escape_markup(n.body)},
                              {"default-action", QStringLiteral("default")}};
        if (portal_reply_supported_)
        {
            // im.reply-with-text (portal interface v2+): a "category" of
            // im.received plus a button with this purpose gets the
            // compositor to render its own inline-reply widget. The
            // button's label IS genuinely shown (unlike the legacy KDE
            // extension below), so it's localized.
            portalMap[QStringLiteral("category")] =
                QString::fromUtf8(tesseract::linux_notify::kPortalImCategory);
            QVariantMap button{
                {QStringLiteral("label"),
                 QString::fromStdString(tk::tr("Reply"))},
                {QStringLiteral("action"),
                 QString::fromUtf8(tesseract::linux_notify::kPortalReplyAction)},
                {QStringLiteral("purpose"),
                 QString::fromUtf8(
                     tesseract::linux_notify::kPortalReplyPurpose)}};
            // "buttons" is aa{sv} (array of dict), not av (array of variant)
            // — a plain QVariantList of QVariantMap would marshal as the
            // latter. Building the QDBusArgument explicitly is the
            // documented Qt idiom for this kind of nested compound type.
            QDBusArgument buttons_arg;
            buttons_arg.beginArray(qMetaTypeId<QVariantMap>());
            buttons_arg << button;
            buttons_arg.endArray();
            portalMap[QStringLiteral("buttons")] =
                QVariant::fromValue(buttons_arg);
        }
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
    QStringList actions{"default", "Open"};
    if (legacy_reply_supported_)
    {
        // The label is unused by KDE for this special action id — it renders
        // its own hardcoded reply affordance instead. Not localized: this is
        // a protocol-level literal, must match KDE's hardcoded string.
        actions << QString::fromUtf8(
                       tesseract::linux_notify::kInlineReplyAction)
                << QStringLiteral("Reply");
        // Plain text, not markup — these render as a widget placeholder /
        // button label in Plasma's own reply UI, not Pango-parsed body text,
        // so (unlike sender/body) they must not go through escape_markup.
        hints[QString::fromUtf8(tesseract::linux_notify::kHintPlaceholder)] =
            QString::fromStdString(tk::tr("Reply\xe2\x80\xa6"));
        hints[QString::fromUtf8(tesseract::linux_notify::kHintSubmitLabel)] =
            QString::fromStdString(tk::tr("Send"));
    }
    QDBusReply<uint> reply =
        iface_.call("Notify", QString("Tesseract"), 0u, QString(""),
                    escape_markup(n.sender), escape_markup(n.body), actions,
                    hints, 5000);

    if (reply.isValid())
    {
        id_to_room_[reply.value()] = {n.room_id, n.event_id};
    }
}

void LinuxNotifierQt::onActionInvoked(uint id, const QString& /*action*/)
{
    auto it = id_to_room_.find(id);
    if (it != id_to_room_.end())
    {
        on_activate_(it->second.room_id,
                     ""); // no activation token via legacy D-Bus
    }
}

void LinuxNotifierQt::onNotificationClosed(uint id, uint /*reason*/)
{
    id_to_room_.erase(id);
}

void LinuxNotifierQt::onNotificationReplied(uint id, const QString& text)
{
    auto it = id_to_room_.find(id);
    if (it == id_to_room_.end())
    {
        return; // stale id — daemon already closed/expired this toast
    }
    on_reply_(it->second.room_id, it->second.event_id, text.toStdString());
}

void LinuxNotifierQt::onPortalActionInvoked(const QString& notification_id,
                                            const QString& action,
                                            const QVariantList& parameter)
{
    auto it = portal_id_to_room_.find(notification_id.toStdString());
    if (it == portal_id_to_room_.end())
    {
        return;
    }

    if (action == QString::fromUtf8(tesseract::linux_notify::kPortalReplyAction))
    {
        // im.reply-with-text: our action id isn't "app."-prefixed, so it's
        // non-exported — the daemon delivers the typed text as the third
        // element (index 2) of this signal's parameter array.
        if (parameter.size() >= 3)
        {
            on_reply_(it->second.room_id, it->second.event_id,
                      parameter.at(2).toString().toStdString());
        }
        return;
    }

    // Click-to-open: the portal sends ActionInvoked with an av (array of
    // variants) whose elements are, in order: optional target, platform-data
    // a{sv} (portal >= 1.16, contains "activation-token" on Wayland),
    // optional response. Search for the first a{sv} element with the token.
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

    on_activate_(it->second.room_id, std::move(token));
}
