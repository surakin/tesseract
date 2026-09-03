#include "Win32PowerMonitor.h"

namespace win32
{

namespace
{
constexpr const wchar_t* kPowerSinkClass = L"TesseractPowerMonitorSink";

// Power-setting GUIDs. Defined locally (with their well-known, stable values)
// rather than referencing the SDK symbols, which need <initguid.h> / a lib to
// materialise and GUID_POWER_SAVING_STATUS is missing from older SDK headers.
constexpr GUID kAcDcPowerSource = {
    0x5D3E9A59,
    0xE9D5,
    0x4B00,
    {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
constexpr GUID kPowerSavingStatus = {
    0xE00958C0,
    0xC213,
    0x4ACE,
    {0x9B, 0x36, 0x4E, 0x0A, 0x53, 0xB1, 0x9E, 0x4A}};

// SYSTEM_POWER_STATUS::SystemStatusFlag: low bit set ⇒ Windows battery saver on.
bool battery_saver_from_status(const SYSTEM_POWER_STATUS& sps)
{
    return (sps.SystemStatusFlag & 0x01) != 0;
}
} // namespace

LRESULT CALLBACK Win32PowerMonitor::wnd_proc_(HWND hwnd, UINT msg, WPARAM wParam,
                                              LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<Win32PowerMonitor*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE)
    {
        auto* p = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
        if (p && p->DataLength >= sizeof(DWORD))
        {
            const DWORD v = *reinterpret_cast<const DWORD*>(p->Data);
            bool changed = false;
            if (IsEqualGUID(p->PowerSetting, kAcDcPowerSource))
            {
                // 0 = AC, 1 = battery (DC), 2 = short-term (UPS) → treat as battery.
                changed = self->state_.set_on_battery(v != 0);
            }
            else if (IsEqualGUID(p->PowerSetting, kPowerSavingStatus))
            {
                changed = self->state_.set_power_saver(v != 0);
            }
            if (changed && self->on_change)
            {
                self->on_change();
            }
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Win32PowerMonitor::refresh_from_system_()
{
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps))
    {
        return;
    }
    // ACLineStatus: 0 offline (battery), 1 online, 255 unknown.
    state_.set_on_battery(sps.ACLineStatus == 0);
    state_.set_power_saver(battery_saver_from_status(sps));
    // BatteryFlag 128 == BATTERY_FLAG_NO_BATTERY, 255 == unknown.
    has_battery_ = sps.BatteryFlag != 128 && sps.BatteryFlag != 255;
}

Win32PowerMonitor::Win32PowerMonitor(HINSTANCE inst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Win32PowerMonitor::wnd_proc_;
    wc.hInstance = inst;
    wc.lpszClassName = kPowerSinkClass;
    RegisterClassExW(&wc); // harmless if already registered

    hwnd_ = CreateWindowExW(0, kPowerSinkClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            inst, this);

    refresh_from_system_();

    if (hwnd_)
    {
        acdc_notify_ = RegisterPowerSettingNotification(
            hwnd_, &kAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
        saver_notify_ = RegisterPowerSettingNotification(
            hwnd_, &kPowerSavingStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
}

Win32PowerMonitor::~Win32PowerMonitor()
{
    if (acdc_notify_)
    {
        UnregisterPowerSettingNotification(acdc_notify_);
    }
    if (saver_notify_)
    {
        UnregisterPowerSettingNotification(saver_notify_);
    }
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
    }
}

} // namespace win32
