#pragma once
#include <functional>

namespace tesseract
{

// Cross-platform "is the machine trying to save power?" probe. Platform impls
// subscribe to the OS power notifications internally (WM_POWERBROADCAST +
// RegisterPowerSettingNotification on Win32, NSProcessInfoPowerStateDidChange +
// IOPSNotificationCreateRunLoopSource on macOS, UPower / power-profiles-daemon
// D-Bus PropertiesChanged on Linux) and cache both signals in atomics, so the
// getters are cheap to call on every notification. When either signal may have
// changed the impl invokes `on_change` on the UI thread.
//
// Mirrors the IScreenLock / IAutostart dependency-injection pattern: the
// concrete shell constructs the platform impl at startup and installs it on
// ShellBase via set_power_monitor_().
class IPowerMonitor
{
public:
    virtual ~IPowerMonitor() = default;

    // The OS energy-saver / battery-saver / "Low Power Mode" profile is on.
    virtual bool os_power_saver_active() const = 0;

    // The machine is running on battery and discharging (not plugged in).
    // Desktops and machines with no battery report false.
    virtual bool on_battery_discharging() const = 0;

    // Set by ShellBase after install; invoked by the platform impl on the UI
    // thread whenever either signal may have changed.
    std::function<void()> on_change;
};

// Stand-in when no detection is wired (unit tests / headless, or a platform
// whose monitor failed to construct). Reports "not saving power" so Low Power
// Mode's Auto setting behaves exactly as it did before the feature existed.
class NullPowerMonitor final : public IPowerMonitor
{
public:
    bool os_power_saver_active() const override
    {
        return false;
    }
    bool on_battery_discharging() const override
    {
        return false;
    }
};

} // namespace tesseract
