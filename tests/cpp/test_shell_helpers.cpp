// Pure helpers from ui/shared/app/shell_helpers.cpp that the shells share:
// window-geometry clamping, typing-indicator text, enum mappings and the
// owner-only recovery-key file writer.

#include <catch2/catch_test_macros.hpp>

#include "app/shell_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sh = tesseract::shell_helpers;
using tesseract::Settings;

namespace
{
Settings::WindowGeometry helpers_geo(int x, int y, int w, int h, bool valid = true)
{
    Settings::WindowGeometry g;
    g.x = x;
    g.y = y;
    g.w = w;
    g.h = h;
    g.valid = valid;
    return g;
}
} // namespace

TEST_CASE("clamp_to_screens: an invalid saved geometry yields an invalid result",
          "[shell_helpers][geometry]")
{
    auto r = sh::clamp_to_screens(helpers_geo(10, 10, 800, 600, false), 1000, 700,
                                  {{0, 0, 1920, 1080}});
    CHECK_FALSE(r.valid);
}

TEST_CASE("clamp_to_screens: a window whose title bar is on a screen is kept",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(100, 100, 800, 600);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}});
    CHECK(r.valid);
    CHECK(r.x == 100);
    CHECK(r.y == 100);
    CHECK(r.w == 800);
    CHECK(r.h == 600);
}

TEST_CASE("clamp_to_screens: a window on a second screen is kept when it exists",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(2000, 50, 800, 600);
    auto r = sh::clamp_to_screens(
        saved, 1000, 700, {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}});
    CHECK(r.x == 2000);
}

TEST_CASE("clamp_to_screens: a window left on an unplugged screen is re-centred",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(2500, 100, 800, 600);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}});
    CHECK(r.valid);
    CHECK(r.w == 800);
    CHECK(r.h == 600);
    CHECK(r.x == (1920 - 800) / 2);
    CHECK(r.y == (1080 - 600) / 2);
}

TEST_CASE("clamp_to_screens: an oversized re-centred window is shrunk to 90%",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(9000, 9000, 4000, 3000);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1000, 800}});
    CHECK(r.w == 900);
    CHECK(r.h == 720);
}

TEST_CASE("clamp_to_screens: a missing saved size falls back to the default",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(9000, 9000, 0, 0);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}});
    CHECK(r.w == 1000);
    CHECK(r.h == 700);
}

TEST_CASE("clamp_to_screens: with no screens it centres within the default size",
          "[shell_helpers][geometry]")
{
    auto saved = helpers_geo(50, 50, 400, 300);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {});
    CHECK(r.valid);
    CHECK(r.w == 400);
    CHECK(r.h == 300);
    CHECK(r.x == (1000 - 400) / 2);
}

TEST_CASE("clamp_to_screens: only the title-bar strip must be visible",
          "[shell_helpers][geometry]")
{
    // The window body hangs far below the screen but its title bar is visible.
    auto saved = helpers_geo(100, 1000, 800, 600);
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}});
    CHECK(r.y == 1000);
    // Title bar entirely below the screen: moved back.
    saved = helpers_geo(100, 1100, 800, 600);
    r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}});
    CHECK(r.y != 1100);
}

TEST_CASE("format_typing_text covers zero, one, two and many typists",
          "[shell_helpers][typing]")
{
    CHECK(sh::format_typing_text({}).empty());

    const auto one = sh::format_typing_text({"Ann"});
    CHECK(one.find("Ann") != std::string::npos);

    const auto two = sh::format_typing_text({"Ann", "Bob"});
    CHECK(two.find("Ann") != std::string::npos);
    CHECK(two.find("Bob") != std::string::npos);

    const auto three = sh::format_typing_text({"Ann", "Bob", "Cy"});
    CHECK(three.find("Ann") != std::string::npos);
    CHECK(three.find("Bob") != std::string::npos);
    CHECK(three.find("Cy") == std::string::npos); // folded into the count
    CHECK(three.find('1') != std::string::npos);

    const auto five = sh::format_typing_text({"A", "B", "C", "D", "E"});
    CHECK(five.find('3') != std::string::npos);
    CHECK(five != three);
}

TEST_CASE("media preview and presence enum mappings", "[shell_helpers][mapping]")
{
    using Mode = tesseract::MediaPreviewConfig::Mode;
    CHECK(sh::mode_to_settings(Mode::Off) == Settings::MediaPreviews::Off);
    CHECK(sh::mode_to_settings(Mode::Private) == Settings::MediaPreviews::Private);
    CHECK(sh::mode_to_settings(Mode::On) == Settings::MediaPreviews::On);

    using TS = tesseract::PresenceTracker::State;
    CHECK(sh::to_client_presence(TS::Online) == tesseract::PresenceState::Online);
    CHECK(sh::to_client_presence(TS::Unavailable) ==
          tesseract::PresenceState::Unavailable);
    CHECK(sh::to_client_presence(TS::Offline) == tesseract::PresenceState::Offline);
}

TEST_CASE("theme accent maps one-to-one", "[shell_helpers][mapping]")
{
    using SA = Settings::ThemeAccent;
    CHECK(sh::to_tk_accent(SA::Blue) == tk::AccentTheme::Blue);
    CHECK(sh::to_tk_accent(SA::Forest) == tk::AccentTheme::Forest);
    CHECK(sh::to_tk_accent(SA::Sunset) == tk::AccentTheme::Sunset);
    CHECK(sh::to_tk_accent(SA::Violet) == tk::AccentTheme::Violet);
    CHECK(sh::to_tk_accent(SA::System) == tk::AccentTheme::System);
}

#ifndef _WIN32
TEST_CASE("write_private_text_file creates an owner-only file",
          "[shell_helpers][private_file]")
{
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() /
                       ("tesseract_private_" + std::to_string(::getpid()) + ".txt");
    fs::remove(p);
    std::string err;
    REQUIRE(sh::write_private_text_file(p.string(), "secret key", err));
    CHECK(err.empty());
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    std::ifstream in(p);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "secret key");
    fs::remove(p);
}

TEST_CASE("write_private_text_file tightens and truncates an existing file",
          "[shell_helpers][private_file]")
{
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() /
                       ("tesseract_private2_" + std::to_string(::getpid()) + ".txt");
    {
        std::ofstream(p) << "a much longer previous content";
    }
    REQUIRE(::chmod(p.c_str(), 0644) == 0);
    std::string err;
    REQUIRE(sh::write_private_text_file(p.string(), "new", err));
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    std::ifstream in(p);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "new");
    fs::remove(p);
}

TEST_CASE("write_private_text_file reports an unwritable path",
          "[shell_helpers][private_file]")
{
    std::string err;
    CHECK_FALSE(sh::write_private_text_file(
        "/nonexistent-dir-for-tesseract-test/key.txt", "x", err));
    CHECK_FALSE(err.empty());
}
#endif
