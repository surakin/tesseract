#pragma once
#include <tesseract/power_monitor.h>

#include "power_state.h"

namespace mac
{

// IPowerMonitor impl backed by NSProcessInfo.isLowPowerModeEnabled (+ the
// NSProcessInfoPowerStateDidChange notification) for the OS Low Power Mode
// signal, and IOKit's IOPSGetProvidingPowerSourceType (+ an
// IOPSNotificationCreateRunLoopSource callback) for the AC/battery signal.
// Both cached in atomics so the getters are cheap on the notification path.
// Notifications are delivered on the main run loop, so on_change fires on the
// UI thread. The observers are removed in the destructor before the object is
// released, so callbacks may safely touch `this`.
class MacPowerMonitor final : public tesseract::IPowerMonitor
{
public:
    MacPowerMonitor();
    ~MacPowerMonitor() override;

    MacPowerMonitor(const MacPowerMonitor&) = delete;
    MacPowerMonitor& operator=(const MacPowerMonitor&) = delete;

    bool os_power_saver_active() const override
    {
        return state_.os_power_saver_active();
    }
    bool on_battery_discharging() const override
    {
        return state_.on_battery_discharging();
    }

    // Re-read both signals from the OS and fire on_change if anything moved.
    // Public so the C-style IOKit run-loop callback can reach it.
    void refresh();

private:
    tesseract::power::State state_;
    void* power_state_observer_ = nullptr; // id (retained)
    void* ps_run_loop_source_ = nullptr;   // CFRunLoopSourceRef (retained)
};

} // namespace mac
