#pragma once
#include "power_state.h"
#include <tesseract/power_monitor.h>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace qt6
{

// IPowerMonitor impl backed by UPower + power-profiles-daemon on the system
// bus. Reads UPower's `OnBattery` and PPD's `ActiveProfile` properties and
// subscribes to `PropertiesChanged` on both. Best-effort: if a service is
// unavailable the corresponding signal stays false, so Low Power Mode's Auto
// setting behaves as it did before the feature existed.
class LinuxPowerMonitorQt final : public QObject, public tesseract::IPowerMonitor
{
    Q_OBJECT
public:
    LinuxPowerMonitorQt();
    ~LinuxPowerMonitorQt() override = default;

    bool os_power_saver_active() const override
    {
        return state_.os_power_saver_active();
    }
    bool on_battery_discharging() const override
    {
        return state_.on_battery_discharging();
    }

private slots:
    void onUPowerPropsChanged(const QString& iface, const QVariantMap& changed,
                              const QStringList& invalidated);
    void onPpdPropsChanged(const QString& iface, const QVariantMap& changed,
                           const QStringList& invalidated);

private:
    void refresh_on_battery_();
    void refresh_power_saver_();
    void notify_();

    // Which PPD bus name actually answered (legacy net.hadess vs new
    // org.freedesktop) — used for the property re-read on PropertiesChanged.
    QString ppd_service_;
    QString ppd_path_;
    QString ppd_iface_;
    tesseract::power::State state_;
};

} // namespace qt6
