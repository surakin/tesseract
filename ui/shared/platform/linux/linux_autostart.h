#pragma once
#include <tesseract/autostart.h>
#include <tesseract/launch_args.h>
#include <tesseract/paths.h>

#include <climits>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

// XDG autostart registration shared by the Linux shells. Writes/removes a
// <desktop_basename>[-profile].desktop file under $XDG_CONFIG_HOME/autostart/
// (default ~/.config/autostart/). The basename matches the desktop file the
// shell's package installs ("tesseract-matrix" for Qt6, "tesseract-matrix-gtk"
// for GTK4; see packaging/debian/rules, packaging/arch/PKGBUILD.in).
// is_enabled() checks file existence — the app fully owns writing/removing
// this file, so existence alone is a reliable signal.
class LinuxAutostart final : public tesseract::IAutostart
{
public:
    explicit LinuxAutostart(std::string desktop_basename)
        : desktop_basename_(std::move(desktop_basename))
    {
    }

    bool is_enabled() const override
    {
        std::error_code ec;
        return std::filesystem::exists(desktop_path_(), ec);
    }

    bool set_enabled(bool enabled) override
    {
        auto path = desktop_path_();

        if (!enabled)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            // Idempotent: "already absent" is success.
            return !ec || ec == std::errc::no_such_file_or_directory;
        }

        auto exe = resolve_exe_path_();
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

private:
    std::filesystem::path desktop_path_() const
    {
        // Per --profile, so each profile has its own login item.
        const std::string file_name =
            desktop_basename_ + tesseract::profile_suffix() + ".desktop";
        // tesseract::config_dir() resolves to $XDG_CONFIG_HOME/tesseract (or
        // ~/.config/tesseract); autostart entries live in the sibling
        // .../autostart/ directory per the XDG autostart spec.
        return tesseract::config_dir().parent_path() / "autostart" / file_name;
    }

    // Resolve the running binary's absolute path so the Exec= line works
    // regardless of install prefix (deb/rpm/PKGBUILD/AppImage all differ).
    static std::string resolve_exe_path_()
    {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return {};
        buf[n] = '\0';
        return std::string(buf, static_cast<std::size_t>(n));
    }

    std::string desktop_basename_;
};
