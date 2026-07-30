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
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "ActionInvoked", this,
        SLOT(onActionInvoked(uint, const QString&)));

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "NotificationClosed", this,
        SLOT(onNotificationClosed(uint, uint)));

    // KDE/GNOME de-facto extension (not in the base freedesktop.org spec):
    // fires just before ActionInvoked with a Wayland xdg_activation_v1 token,
    // provided notify() sends a "desktop-entry" hint the daemon can resolve
    // to this app. Daemons that don't implement it simply never fire this,
    // so subscribing unconditionally is safe.
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "ActivationToken", this,
        SLOT(onActivationToken(uint, const QString&)));

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
    // Only Flatpak actually requires the portal — direct D-Bus calls to the
    // notification daemon are blocked by the sandbox's D-Bus proxy there.
    // Wayland used to route here too, for the portal's activation-token
    // support, but the legacy interface's own ActivationToken extension
    // (see the constructor) covers that on both KDE and GNOME without the
    // portal's downsides — notably being stuck on Notification interface
    // v1 on KDE as of this writing, with no inline-reply support at all on
    // that path (see ActivationToken's history/reasoning).
    return qEnvironmentVariableIsSet("FLATPAK_ID");
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
        correlation_.record_portal(pid.toStdString(), n.room_id, n.event_id);
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

    // Required for the daemon's ActivationToken signal to know which app to
    // mint a Wayland xdg_activation_v1 token for (see the ActivationToken
    // subscription in the constructor) — mirrors KDE's own
    // knotifications/src/notifybypopup.cpp exactly, including stripping a
    // ".desktop" suffix if present.
    QString desktop_entry = QGuiApplication::desktopFileName();
    if (desktop_entry.endsWith(QLatin1String(".desktop")))
    {
        desktop_entry.chop(8);
    }
    if (!desktop_entry.isEmpty())
    {
        hints[QStringLiteral("desktop-entry")] = desktop_entry;
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
        correlation_.record(reply.value(), n.room_id, n.event_id);
    }
}

void LinuxNotifierQt::onActionInvoked(uint id, const QString& /*action*/)
{
    if (auto found = correlation_.find(id))
    {
        // ActivationToken (if the daemon supports it) always arrives before
        // ActionInvoked for the same id — take_token() returns empty if the
        // daemon never sent one.
        on_activate_(found->room_id, correlation_.take_token(id));
    }
}

void LinuxNotifierQt::onNotificationClosed(uint id, uint /*reason*/)
{
    correlation_.forget(id);
}

void LinuxNotifierQt::onActivationToken(uint id, const QString& token)
{
    correlation_.stash_token(id, token.toStdString());
}

void LinuxNotifierQt::onNotificationReplied(uint id, const QString& text)
{
    auto found = correlation_.find(id);
    if (!found)
    {
        return; // stale id — daemon already closed/expired this toast
    }
    on_reply_(found->room_id, found->event_id, text.toStdString());
}

void LinuxNotifierQt::onPortalActionInvoked(const QString& notification_id,
                                            const QString& action,
                                            const QVariantList& parameter)
{
    auto found = correlation_.find_portal(notification_id.toStdString());
    if (!found)
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
            on_reply_(found->room_id, found->event_id,
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

    on_activate_(found->room_id, std::move(token));
}
