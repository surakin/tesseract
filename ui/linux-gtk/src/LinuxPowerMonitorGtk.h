#pragma once
#include "power_state.h"
#include <tesseract/power_monitor.h>
#include <gio/gio.h>
#include <string>

namespace gtk4
{

// IPowerMonitor impl backed by UPower + power-profiles-daemon over GDBus on
// the system bus. Reads UPower's `OnBattery` and PPD's `ActiveProfile`
// properties and subscribes to `PropertiesChanged` on both. Best-effort: an
// unavailable service leaves the corresponding signal false, so Low Power
// Mode's Auto setting behaves as it did before the feature existed.
class LinuxPowerMonitorGtk final : public tesseract::IPowerMonitor
{
public:
    LinuxPowerMonitorGtk();
    ~LinuxPowerMonitorGtk() override;

    LinuxPowerMonitorGtk(const LinuxPowerMonitorGtk&) = delete;
    LinuxPowerMonitorGtk& operator=(const LinuxPowerMonitorGtk&) = delete;

    bool os_power_saver_active() const override
    {
        return state_.os_power_saver_active();
    }
    bool on_battery_discharging() const override
    {
        return state_.on_battery_discharging();
    }
    bool has_battery() const override
    {
        return has_battery_;
    }

private:
    static void on_upower_props(GDBusConnection*, const char* sender,
                                const char* path, const char* iface,
                                const char* signal, GVariant* params,
                                gpointer user_data);
    static void on_ppd_props(GDBusConnection*, const char* sender,
                             const char* path, const char* iface,
                             const char* signal, GVariant* params,
                             gpointer user_data);

    void refresh_on_battery_();
    void refresh_power_saver_();
    void detect_battery_(); // one-shot at construction
    void notify_();

    // Read one D-Bus property; returns nullptr on failure (caller g_variant_unref).
    GVariant* get_prop_(const char* service, const char* path,
                        const char* iface, const char* name);

    GDBusConnection* bus_ = nullptr; // owned (g_object_unref)
    guint sub_upower_ = 0;
    guint sub_ppd_ = 0;
    // Which PPD bus name answered (legacy net.hadess vs new org.freedesktop).
    std::string ppd_service_;
    std::string ppd_path_;
    std::string ppd_iface_;
    bool has_battery_ = false;
    tesseract::power::State state_;
};

} // namespace gtk4
