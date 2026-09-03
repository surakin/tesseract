#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tesseract/power_monitor.h>

#include "power_state.h"

namespace win32
{

// IPowerMonitor impl backed by a hidden message-only window plus
// RegisterPowerSettingNotification for GUID_ACDC_POWER_SOURCE (AC vs battery)
// and GUID_POWER_SAVING_STATUS (Windows battery saver, Win10+). Initial state
// comes from GetSystemPowerStatus. The cached signals live in atomics so the
// getters are cheap on the notification path. Best-effort: if the window or a
// registration fails, the corresponding signal simply stays false.
class Win32PowerMonitor final : public tesseract::IPowerMonitor
{
public:
    explicit Win32PowerMonitor(HINSTANCE inst);
    ~Win32PowerMonitor() override;

    Win32PowerMonitor(const Win32PowerMonitor&) = delete;
    Win32PowerMonitor& operator=(const Win32PowerMonitor&) = delete;

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
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    void refresh_from_system_();

    HWND hwnd_ = nullptr;
    HPOWERNOTIFY acdc_notify_ = nullptr;
    HPOWERNOTIFY saver_notify_ = nullptr;
    bool has_battery_ = false;
    tesseract::power::State state_;
};

} // namespace win32
