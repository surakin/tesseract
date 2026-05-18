#include "LinuxScreenLockQt.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariant>

namespace qt6
{

namespace
{
constexpr const char* kService = "org.freedesktop.login1";
constexpr const char* kSessIf = "org.freedesktop.login1.Session";
// "auto" resolves to the calling process's own session.
constexpr const char* kSessObj = "/org/freedesktop/login1/session/auto";
} // namespace

LinuxScreenLockQt::LinuxScreenLockQt()
{
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
    {
        return;
    }

    session_path_ = QString::fromLatin1(kSessObj);
    refresh_locked_hint();

    // Lock / Unlock are signals on the Session interface. The daemon emits
    // them when any locker requests a lock-screen state change.
    bus.connect(kService, session_path_, kSessIf, "Lock", this, SLOT(onLock()));
    bus.connect(kService, session_path_, kSessIf, "Unlock", this,
                SLOT(onUnlock()));
}

void LinuxScreenLockQt::refresh_locked_hint()
{
    QDBusInterface props(kService, session_path_,
                         "org.freedesktop.DBus.Properties",
                         QDBusConnection::systemBus());
    if (!props.isValid())
    {
        return;
    }
    QDBusReply<QVariant> r = props.call("Get", QString::fromLatin1(kSessIf),
                                        QStringLiteral("LockedHint"));
    if (r.isValid())
    {
        locked_ = r.value().toBool();
    }
}

} // namespace qt6
