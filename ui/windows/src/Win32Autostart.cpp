#include "Win32Autostart.h"
#include "Win32PackageContext.h"

#include <string>

#include "winrt_coroutine_shim.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>

namespace win32
{

namespace
{
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[]  = L"Tesseract";
} // namespace

bool Win32Autostart::is_enabled() const
{
    if (package_context::is_packaged())
    {
        try
        {
            using winrt::Windows::ApplicationModel::StartupTask;
            using winrt::Windows::ApplicationModel::StartupTaskState;
            const auto state = StartupTask::GetAsync(L"TesseractStartup").get().State();
            return state == StartupTaskState::Enabled ||
                   state == StartupTaskState::EnabledByPolicy;
        }
        catch (const winrt::hresult_error&)
        {
            return false;
        }
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
    {
        return false;
    }
    LONG result = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool Win32Autostart::set_enabled(bool enabled)
{
    if (package_context::is_packaged())
    {
        try
        {
            using winrt::Windows::ApplicationModel::StartupTask;
            using winrt::Windows::ApplicationModel::StartupTaskState;
            auto task = StartupTask::GetAsync(L"TesseractStartup").get();
            if (!enabled)
            {
                task.Disable();
                return true;
            }
            const auto state = task.RequestEnableAsync().get();
            return state == StartupTaskState::Enabled ||
                   state == StartupTaskState::EnabledByPolicy;
        }
        catch (const winrt::hresult_error&)
        {
            return false;
        }
    }
    if (!enabled)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key)
            != ERROR_SUCCESS)
        {
            // Key doesn't exist at all: nothing to remove, already disabled.
            return true;
        }
        LONG result = RegDeleteValueW(key, kValueName);
        RegCloseKey(key);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    wchar_t exe_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0)
        return false;

    std::wstring cmd = std::wstring(L"\"") + exe_path + L"\" --autostart";

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
    {
        return false;
    }
    LONG result = RegSetValueExW(
        key, kValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(cmd.c_str()),
        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

} // namespace win32
