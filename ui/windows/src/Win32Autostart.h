#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tesseract/autostart.h>

namespace win32
{

// IAutostart impl backed by the per-user Run registry key
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Tesseract). No admin
// rights required — mirrors the existing raw-Win32 registry pattern used
// for the matrix: URL-protocol and AppUserModelId registrations in main.cpp.
// Selects the manifest StartupTask for MSIX and the per-user Run key for NSIS.
class Win32Autostart final : public tesseract::IAutostart
{
public:
    bool is_enabled() const override;
    bool set_enabled(bool enabled) override;
};

} // namespace win32
