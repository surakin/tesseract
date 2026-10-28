#include <catch2/catch_test_macros.hpp>

#include "platform/linux/linux_autostart.h"

#include <tesseract/paths.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

// Points XDG_CONFIG_HOME at a scratch dir for one test, then restores it.
struct ScopedConfigHome
{
    ScopedConfigHome()
    {
        if (const char* old = std::getenv("XDG_CONFIG_HOME"))
        {
            had_old_ = true;
            old_ = old;
        }
        root_ = fs::temp_directory_path() / "tesseract-autostart-test";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
        ::setenv("XDG_CONFIG_HOME", root_.c_str(), 1);
    }
    ~ScopedConfigHome()
    {
        if (had_old_)
            ::setenv("XDG_CONFIG_HOME", old_.c_str(), 1);
        else
            ::unsetenv("XDG_CONFIG_HOME");
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path root_;
    std::string old_;
    bool had_old_ = false;
};

} // namespace

TEST_CASE("LinuxAutostart writes and removes the .desktop file",
          "[linux_autostart]")
{
    ScopedConfigHome home;
    LinuxAutostart autostart("tesseract-matrix-test");
    const fs::path file = tesseract::config_dir().parent_path() / "autostart" /
                          ("tesseract-matrix-test" + tesseract::profile_suffix() +
                           ".desktop");

    CHECK_FALSE(autostart.is_enabled());

    REQUIRE(autostart.set_enabled(true));
    CHECK(autostart.is_enabled());
    REQUIRE(fs::exists(file));

    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string contents = ss.str();
    CHECK(contents.find("[Desktop Entry]") != std::string::npos);
    CHECK(contents.find("Exec=") != std::string::npos);
    CHECK(contents.find("--autostart") != std::string::npos);

    CHECK(autostart.set_enabled(false));
    CHECK_FALSE(autostart.is_enabled());
    CHECK_FALSE(fs::exists(file));

    // Idempotent: removing an already-absent entry still succeeds.
    CHECK(autostart.set_enabled(false));
}
