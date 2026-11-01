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
    auto r =
        sh::clamp_to_screens(helpers_geo(10, 10, 800, 600, false), 1000, 700, {{0, 0, 1920, 1080}});
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
    auto r = sh::clamp_to_screens(saved, 1000, 700, {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}});
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

TEST_CASE("clamp_to_screens: only the title-bar strip must be visible", "[shell_helpers][geometry]")
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

TEST_CASE("format_typing_text covers zero, one, two and many typists", "[shell_helpers][typing]")
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
    CHECK(sh::to_client_presence(TS::Unavailable) == tesseract::PresenceState::Unavailable);
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
TEST_CASE("write_private_text_file creates an owner-only file", "[shell_helpers][private_file]")
{
    namespace fs = std::filesystem;
    const fs::path p =
        fs::temp_directory_path() / ("tesseract_private_" + std::to_string(::getpid()) + ".txt");
    fs::remove(p);
    std::string err;
    REQUIRE(sh::write_private_text_file(p.string(), "secret key", err));
    CHECK(err.empty());
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    std::ifstream in(p);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content == "secret key");
    fs::remove(p);
}

TEST_CASE("write_private_text_file tightens and truncates an existing file",
          "[shell_helpers][private_file]")
{
    namespace fs = std::filesystem;
    const fs::path p =
        fs::temp_directory_path() / ("tesseract_private2_" + std::to_string(::getpid()) + ".txt");
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
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content == "new");
    fs::remove(p);
}

TEST_CASE("write_private_text_file reports an unwritable path", "[shell_helpers][private_file]")
{
    std::string err;
    CHECK_FALSE(
        sh::write_private_text_file("/nonexistent-dir-for-tesseract-test/key.txt", "x", err));
    CHECK_FALSE(err.empty());
}
#endif

TEST_CASE("quote_command_line_arg leaves an ordinary value as the usual quoted form",
          "[shell][shell_helpers][jumplist]")
{
    CHECK(sh::quote_command_line_arg(L"!room:example.org") == L"\"!room:example.org\"");
    CHECK(sh::quote_command_line_arg(L"a\\b") == L"\"a\\b\"");
    CHECK(sh::quote_command_line_arg(L"") == L"\"\"");
}

TEST_CASE("quote_command_line_arg escapes quotes and the backslashes before them",
          "[shell][shell_helpers][jumplist]")
{
    // An embedded quote is escaped, so it cannot close the argument.
    CHECK(sh::quote_command_line_arg(L"a\"b") == L"\"a\\\"b\"");
    // A backslash before a quote is doubled, and the quote gets its own backslash.
    CHECK(sh::quote_command_line_arg(L"a\\\"b") == L"\"a\\\\\\\"b\"");
    // A trailing backslash is doubled so it does not escape the closing quote.
    CHECK(sh::quote_command_line_arg(L"a\\") == L"\"a\\\\\"");
}

TEST_CASE("a room id cannot break out of its Jump List argument",
          "[shell][shell_helpers][jumplist]")
{
    // The value that would add a flag if it were pasted between plain quotes.
    const std::wstring hostile = L"x\" --logoutall";
    const std::wstring quoted = sh::quote_command_line_arg(hostile);
    // Still one quoted run: the only unescaped quotes are the first and last.
    CHECK(quoted == L"\"x\\\" --logoutall\"");
    CHECK(quoted.front() == L'"');
    CHECK(quoted.back() == L'"');
}

namespace
{
namespace fs = std::filesystem;

std::vector<std::uint8_t> tile_png()
{
    // The PNG signature plus a few bytes: enough to be a tile to is_png_bytes.
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
}

// A private directory per test; the tile sits at <dir>/2.png.
fs::path tile_test_dir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / "tesseract_tile_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("a map tile is stored and read back as the same PNG", "[shell_helpers][tile_disk]")
{
    const fs::path dir = tile_test_dir("roundtrip");
    const fs::path p = dir / "2.png";
    CHECK(sh::write_tile_disk(p, tile_png()));
    CHECK(sh::read_tile_disk(p) == tile_png());
    // The temporary name is renamed away, never left beside the tile.
    CHECK_FALSE(fs::exists(dir / "2.png.part"));
    fs::remove_all(dir);
}

TEST_CASE("a non-PNG tile is refused on store", "[shell_helpers][tile_disk]")
{
    const fs::path dir = tile_test_dir("refused");
    const fs::path p = dir / "2.png";
    CHECK_FALSE(sh::write_tile_disk(p, {'<', 'h', 't', 'm', 'l', '>'}));
    CHECK_FALSE(fs::exists(p));
    fs::remove_all(dir);
}

TEST_CASE("an empty or truncated tile on disk is dropped, not served", "[shell_helpers][tile_disk]")
{
    const fs::path dir = tile_test_dir("dropped");
    const fs::path empty = dir / "empty.png";
    {
        std::ofstream f(empty, std::ios::binary);
    }
    CHECK(fs::exists(empty));
    CHECK(sh::read_tile_disk(empty).empty());
    CHECK_FALSE(fs::exists(empty));

    const fs::path truncated = dir / "truncated.png";
    {
        std::ofstream f(truncated, std::ios::binary);
        f.write("\x89PN", 3); // a write cut short inside the signature
    }
    CHECK(sh::read_tile_disk(truncated).empty());
    CHECK_FALSE(fs::exists(truncated));
    fs::remove_all(dir);
}

TEST_CASE("a missing tile reads as empty and creates nothing", "[shell_helpers][tile_disk]")
{
    const fs::path dir = tile_test_dir("missing");
    CHECK(sh::read_tile_disk(dir / "2.png").empty());
    CHECK_FALSE(fs::exists(dir / "2.png"));
    fs::remove_all(dir);
}

TEST_CASE("gallery_caption_or_fallback keeps a typed caption and counts items otherwise",
          "[shell_helpers]")
{
    using tesseract::shell_helpers::gallery_caption_or_fallback;
    CHECK(gallery_caption_or_fallback("holiday", 3) == "holiday");
    CHECK(gallery_caption_or_fallback("", 3) == "Sent 3 items");
    CHECK(gallery_caption_or_fallback("", 1) == "Sent 1 item");
}

TEST_CASE("parse_mutual_rooms_json reads a page and reports errors",
          "[shell_helpers][mutual_rooms]")
{
    auto p = sh::parse_mutual_rooms_json(
        R"({"joined":["!a:x","!b:x"],"count":7,"next_batch":"tok","error":""})");
    CHECK(p.error.empty());
    CHECK(p.joined == std::vector<std::string>{"!a:x", "!b:x"});
    CHECK(p.count == 7);
    CHECK(p.next_batch == "tok");

    p = sh::parse_mutual_rooms_json(R"({"joined":[],"count":0,"next_batch":"","error":"boom"})");
    CHECK(p.error == "boom");
    CHECK(p.joined.empty());

    CHECK_FALSE(sh::parse_mutual_rooms_json("not json").error.empty());
    CHECK_FALSE(sh::parse_mutual_rooms_json("").error.empty());
}

TEST_CASE("alias_server_via yields the alias's server and nothing for other ids",
          "[shell][helpers]")
{
    CHECK(sh::alias_server_via("#room:server.org") == std::vector<std::string>{"server.org"});
    CHECK(sh::alias_server_via("  #room:server.org \n") == std::vector<std::string>{"server.org"});
    CHECK(sh::alias_server_via("#room:server.org:8448") ==
          std::vector<std::string>{"server.org:8448"});
    CHECK(sh::alias_server_via("!id:server.org").empty());
    CHECK(sh::alias_server_via("#nocolon").empty());
    CHECK(sh::alias_server_via("#room:").empty());
    CHECK(sh::alias_server_via("").empty());
}
