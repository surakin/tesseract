#include "LinuxAutostartGtk.h"

#include <tesseract/paths.h>

#include <climits>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace
{

constexpr const char* kDesktopFileName = "tesseract-matrix-gtk.desktop";

std::filesystem::path autostart_desktop_path()
{
    // tesseract::config_dir() resolves to $XDG_CONFIG_HOME/tesseract (or
    // ~/.config/tesseract); autostart entries live in the sibling
    // .../autostart/ directory per the XDG autostart spec.
    return tesseract::config_dir().parent_path() / "autostart" / kDesktopFileName;
}

// Resolve the running binary's absolute path so the Exec= line works
// regardless of install prefix (deb/rpm/PKGBUILD/AppImage all differ).
std::string resolve_exe_path()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::string(buf, static_cast<std::size_t>(n));
}

} // namespace

bool LinuxAutostartGtk::is_enabled() const
{
    std::error_code ec;
    return std::filesystem::exists(autostart_desktop_path(), ec);
}

bool LinuxAutostartGtk::set_enabled(bool enabled)
{
    auto path = autostart_desktop_path();

    if (!enabled)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Idempotent: "already absent" is success.
        return !ec || ec == std::errc::no_such_file_or_directory;
    }

    auto exe = resolve_exe_path();
    if (exe.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return false;

    f << "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Tesseract\n"
         "Comment=Matrix chat client\n"
         "Exec=" << exe << " --autostart\n"
         "Icon=tesseract\n"
         "Categories=Network;InstantMessaging;Chat;\n"
         "StartupNotify=false\n"
         "X-GNOME-Autostart-enabled=true\n";

    return static_cast<bool>(f);
}
