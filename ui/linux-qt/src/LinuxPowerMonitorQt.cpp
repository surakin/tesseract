#include "LinuxPowerMonitorQt.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariant>

namespace qt6
{

namespace
{
namespace pw = tesseract::power;

// Read one D-Bus property, returning an invalid QVariant on any failure.
QVariant read_prop(const QString& service, const QString& path,
                   const QString& iface, const QString& name)
{
    QDBusInterface props(service, path, pw::kPropsIface,
                         QDBusConnection::systemBus());
    if (!props.isValid())
    {
        return {};
    }
    QDBusReply<QVariant> r = props.call("Get", iface, name);
    return r.isValid() ? r.value() : QVariant{};
}
} // namespace

LinuxPowerMonitorQt::LinuxPowerMonitorQt()
{
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
    {
        return;
    }

    // --- power-profiles-daemon: try the new bus name, then the legacy one ---
    if (read_prop(pw::kPpdService, pw::kPpdPath, pw::kPpdIface, "ActiveProfile")
            .isValid())
    {
        ppd_service_ = pw::kPpdService;
        ppd_path_ = pw::kPpdPath;
        ppd_iface_ = pw::kPpdIface;
    }
    else if (read_prop(pw::kPpdServiceLegacy, pw::kPpdPathLegacy,
                       pw::kPpdIfaceLegacy, "ActiveProfile")
                 .isValid())
    {
        ppd_service_ = pw::kPpdServiceLegacy;
        ppd_path_ = pw::kPpdPathLegacy;
        ppd_iface_ = pw::kPpdIfaceLegacy;
    }

    refresh_on_battery_();
    refresh_power_saver_();

    bus.connect(pw::kUPowerService, pw::kUPowerPath, pw::kPropsIface,
                "PropertiesChanged", this,
                SLOT(onUPowerPropsChanged(QString, QVariantMap, QStringList)));
    if (!ppd_service_.isEmpty())
    {
        bus.connect(ppd_service_, ppd_path_, pw::kPropsIface, "PropertiesChanged",
                    this,
                    SLOT(onPpdPropsChanged(QString, QVariantMap, QStringList)));
    }
}

void LinuxPowerMonitorQt::onUPowerPropsChanged(const QString&,
                                               const QVariantMap& changed,
                                               const QStringList&)
{
    if (changed.contains("OnBattery"))
    {
        state_.set_on_battery(changed.value("OnBattery").toBool());
        notify_();
    }
    else
    {
        refresh_on_battery_();
    }
}

void LinuxPowerMonitorQt::onPpdPropsChanged(const QString&,
                                            const QVariantMap& changed,
                                            const QStringList&)
{
    if (changed.contains("ActiveProfile"))
    {
        const bool saver =
            changed.value("ActiveProfile").toString() ==
            QLatin1String(tesseract::power::kPowerSaverProfile);
        state_.set_power_saver(saver);
        notify_();
    }
    else
    {
        refresh_power_saver_();
    }
}

void LinuxPowerMonitorQt::refresh_on_battery_()
{
    QVariant v = read_prop(tesseract::power::kUPowerService,
                           tesseract::power::kUPowerPath,
                           tesseract::power::kUPowerIface, "OnBattery");
    if (v.isValid())
    {
        state_.set_on_battery(v.toBool());
    }
}

void LinuxPowerMonitorQt::refresh_power_saver_()
{
    if (ppd_service_.isEmpty())
    {
        return;
    }
    QVariant v = read_prop(ppd_service_, ppd_path_, ppd_iface_, "ActiveProfile");
    if (v.isValid())
    {
        state_.set_power_saver(
            v.toString() ==
            QLatin1String(tesseract::power::kPowerSaverProfile));
    }
}

void LinuxPowerMonitorQt::notify_()
{
    // Qt delivers D-Bus signals on the thread that owns the connection — here
    // the main (UI) thread — so invoking on_change directly is safe.
    if (on_change)
    {
        on_change();
    }
}

} // namespace qt6
