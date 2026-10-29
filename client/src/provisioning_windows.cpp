#include "tesseract/provisioning.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{

std::optional<std::string> read_value(HKEY root, const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Tesseract-Matrix", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)
        != ERROR_SUCCESS)
        return std::nullopt;

    std::optional<std::string> result;
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes)
            == ERROR_SUCCESS
        && type == REG_SZ && bytes >= sizeof(wchar_t))
    {
        std::wstring wide(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(wide.data()), &bytes)
            == ERROR_SUCCESS)
        {
            while (!wide.empty() && wide.back() == L'\0')
                wide.pop_back();
            int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
            if (n > 0)
            {
                std::string utf8(static_cast<size_t>(n), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                    utf8.data(), n, nullptr, nullptr);
                result = std::move(utf8);
            }
        }
    }
    RegCloseKey(key);
    return result;
}

} // namespace

namespace tesseract::detail
{

ProvisionedValues read_platform_provisioning_raw()
{
    // Per-user value overrides the machine-wide one.
    auto read = [](const wchar_t* name) -> std::optional<std::string> {
        if (auto v = read_value(HKEY_CURRENT_USER, name); v && !v->empty())
            return v;
        return read_value(HKEY_LOCAL_MACHINE, name);
    };
    return {read(L"DefaultHomeserver"), read(L"DefaultUsername")};
}

} // namespace tesseract::detail
