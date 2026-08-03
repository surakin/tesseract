#pragma once

#include <filesystem>
#include <string>

namespace win32::package_context
{

inline constexpr wchar_t kUnpackagedAumid[] = L"io.gnomos.Tesseract";

bool is_packaged();
std::wstring package_family_name();
std::wstring effective_aumid();
std::filesystem::path local_state_path();

// Pure helpers kept public for focused unit tests.
std::wstring select_aumid(bool packaged, const std::wstring& packaged_aumid);
std::wstring msix_version(const std::string& project_version);

} // namespace win32::package_context
