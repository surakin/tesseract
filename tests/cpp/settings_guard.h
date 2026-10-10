#pragma once

// RAII snapshot/restore of the process-wide Settings singleton (which is
// non-copyable). Tests that mutate Settings::instance() hold one so later test
// cases in the same process see the values they started with. The snapshot
// round-trips through save_to_disk/load_from_disk in a private temp dir.

#include <tesseract/settings.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tesseract::test
{

class SettingsGuard
{
public:
    SettingsGuard()
    {
        static std::atomic<int> counter{0};
#if defined(_WIN32)
        const auto pid = _getpid();
#else
        const auto pid = ::getpid();
#endif
        dir_ = std::filesystem::temp_directory_path() /
               ("tesseract-settings-guard-" + std::to_string(pid) + "-" +
                std::to_string(counter++));
        std::filesystem::create_directories(dir_);
        Settings::instance().save_to_disk(dir_);
    }
    ~SettingsGuard()
    {
        Settings::instance().load_from_disk(dir_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    SettingsGuard(const SettingsGuard&) = delete;
    SettingsGuard& operator=(const SettingsGuard&) = delete;

private:
    std::filesystem::path dir_;
};

} // namespace tesseract::test
