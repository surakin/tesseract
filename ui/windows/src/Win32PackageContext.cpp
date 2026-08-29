#include "Win32PackageContext.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>

#include <algorithm>
#include <vector>

namespace win32::package_context
{
namespace
{
template <typename Fn>
std::wstring query_package_string(Fn fn)
{
    UINT32 length = 0;
    const LONG first = fn(&length, nullptr);
    if (first != ERROR_INSUFFICIENT_BUFFER || length == 0)
        return {};
    std::vector<wchar_t> value(length, L'\0');
    if (fn(&length, value.data()) != ERROR_SUCCESS)
        return {};
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return {value.begin(), value.end()};
}
} // namespace

bool is_packaged()
{
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) ==
           ERROR_INSUFFICIENT_BUFFER;
}

std::wstring package_family_name()
{
    return query_package_string(
        [](UINT32* length, wchar_t* value)
        { return GetCurrentPackageFamilyName(length, value); });
}

std::wstring select_aumid(bool packaged, const std::wstring& packaged_aumid)
{
    return packaged && !packaged_aumid.empty()
        ? packaged_aumid
        : std::wstring{kUnpackagedAumid};
}

std::wstring effective_aumid()
{
    const bool packaged = is_packaged();
    std::wstring packaged_aumid;
    if (packaged)
    {
        packaged_aumid = query_package_string(
            [](UINT32* length, wchar_t* value)
            { return GetCurrentApplicationUserModelId(length, value); });
    }
    return select_aumid(packaged, packaged_aumid);
}

std::filesystem::path local_state_path()
{
    const auto family = package_family_name();
    if (family.empty()) return {};
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                    nullptr, &raw)))
        return {};
    std::filesystem::path result = std::filesystem::path(raw) / L"Packages" /
                                   family / L"LocalState";
    CoTaskMemFree(raw);
    return result;
}

std::wstring msix_version(const std::string& project_version)
{
    std::wstring result(project_version.begin(), project_version.end());
    const auto dots = static_cast<int>(
        std::count(result.begin(), result.end(), L'.'));
    for (int i = dots; i < 3; ++i) result += L".0";
    return result;
}

} // namespace win32::package_context
