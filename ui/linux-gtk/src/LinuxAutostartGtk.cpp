#include "LinuxAutostartGtk.h"

#include <tesseract/launch_args.h>
#include <tesseract/paths.h>

#include <climits>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace
{

std::filesystem::path autostart_desktop_path()
{
    // Per --profile, so each profile has its own login item.
    const std::string file_name =
        "tesseract-matrix-gtk" + tesseract::profile_suffix() + ".desktop";
    // tesseract::config_dir() resolves to $XDG_CONFIG_HOME/tesseract (or
    // ~/.config/tesseract); autostart entries live in the sibling
    // .../autostart/ directory per the XDG autostart spec.
    return tesseract::config_dir().parent_path() / "autostart" / file_name;
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

    std::string profile_args;
    for (const auto& arg : tesseract::profile_relaunch_args(tesseract::profile()))
    {
        profile_args += " " + arg;
    }

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return false;

    f << "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Tesseract\n"
         "Comment=Matrix chat client\n"
         "Exec=" << exe << " --autostart" << profile_args << "\n"
         "Icon=tesseract\n"
         "Categories=Network;InstantMessaging;Chat;\n"
         "StartupNotify=false\n"
         "X-GNOME-Autostart-enabled=true\n";

    return static_cast<bool>(f);
}
