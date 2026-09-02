#pragma once
#include <atomic>

namespace tesseract::power
{

// D-Bus identifiers shared by the Qt6/GTK4 power probes.
//
// UPower publishes the AC/battery state as the `OnBattery` property on the
// manager object.
inline constexpr const char* kUPowerService = "org.freedesktop.UPower";
inline constexpr const char* kUPowerPath = "/org/freedesktop/UPower";
inline constexpr const char* kUPowerIface = "org.freedesktop.UPower";

// power-profiles-daemon publishes the active profile as the `ActiveProfile`
// string property; "power-saver" means the user asked the system to conserve
// energy. Older builds expose it under net.hadess.PowerProfiles, newer ones
// under org.freedesktop.UPower.PowerProfiles at the same path — try both.
inline constexpr const char* kPpdServiceLegacy = "net.hadess.PowerProfiles";
inline constexpr const char* kPpdPathLegacy = "/net/hadess/PowerProfiles";
inline constexpr const char* kPpdIfaceLegacy = "net.hadess.PowerProfiles";
inline constexpr const char* kPpdService = "org.freedesktop.UPower.PowerProfiles";
inline constexpr const char* kPpdPath = "/org/freedesktop/UPower/PowerProfiles";
inline constexpr const char* kPpdIface = "org.freedesktop.UPower.PowerProfiles";

inline constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";
inline constexpr const char* kPowerSaverProfile = "power-saver";

// Best-effort cached power state: both signals default to false (not saving
// power) when the backing service is unavailable, so Low Power Mode's Auto
// setting stays inert on systems without UPower / power-profiles-daemon.
class State
{
public:
    bool os_power_saver_active() const
    {
        return saver_.load(std::memory_order_relaxed);
    }
    bool on_battery_discharging() const
    {
        return on_battery_.load(std::memory_order_relaxed);
    }

    // Returns true when the value actually changed (so callers can skip a
    // redundant on_change notification).
    bool set_power_saver(bool v)
    {
        return saver_.exchange(v, std::memory_order_relaxed) != v;
    }
    bool set_on_battery(bool v)
    {
        return on_battery_.exchange(v, std::memory_order_relaxed) != v;
    }

private:
    std::atomic<bool> saver_{false};
    std::atomic<bool> on_battery_{false};
};

} // namespace tesseract::power
