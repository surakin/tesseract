#include <catch2/catch_test_macros.hpp>

#include "tesseract/client.h"
#include "tesseract/open_media.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using V = tesseract::Client::MediaTypeVerdict;

TEST_CASE("open_media writes private per-call files and purges them", "[open_media]")
{
    const std::vector<std::uint8_t> data{1, 2, 3};
    auto a = tesseract::write_open_media_file("photo.png", data);
    auto b = tesseract::write_open_media_file("photo.png", data);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a != *b); // unique subdirectory per call keeps the exact name
    CHECK(a->filename() == "photo.png");
    CHECK(fs::file_size(*a) == 3);
#ifndef _WIN32
    CHECK((fs::status(*a).permissions() & fs::perms::group_all) == fs::perms::none);
    CHECK((fs::status(*a).permissions() & fs::perms::others_all) == fs::perms::none);
#endif

    tesseract::purge_open_media_dir();
    CHECK_FALSE(fs::exists(*a));
    CHECK_FALSE(fs::exists(*b));
}

TEST_CASE("open_media refuses path-like names and files outside its directory", "[open_media]")
{
    const std::vector<std::uint8_t> data{1};
    CHECK_FALSE(tesseract::write_open_media_file("../evil", data));
    CHECK_FALSE(tesseract::write_open_media_file("a/b.png", data));
    CHECK_FALSE(tesseract::write_open_media_file("..", data));
    CHECK_FALSE(tesseract::write_open_media_file("", data));
    CHECK_FALSE(tesseract::write_open_media_file("a.png:evil", data));
    CHECK_FALSE(tesseract::write_open_media_file("a\n.png", data));
    CHECK_FALSE(tesseract::open_local_file(fs::temp_directory_path() / "x.png"));
    CHECK_FALSE(tesseract::open_local_file("relative.png"));
}

TEST_CASE("Client::check_media_file classifies through the FFI", "[open_media]")
{
    const std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    CHECK(tesseract::Client::check_media_file("a.png", "image/png", png).verdict == V::Ok);
    const auto r = tesseract::Client::check_media_file("a.jpg", "image/jpeg", png);
    CHECK(r.verdict == V::ContentMismatch);
    CHECK(r.renamed_filename == "a.png");
    CHECK(tesseract::Client::check_media_file("x.exe", "image/png", png).verdict == V::Executable);
    const std::vector<std::uint8_t> proj{'<', 'P', 'r', 'o', 'j', 'e', 'c', 't', '>'};
    CHECK(tesseract::Client::check_media_file("Build.csproj", "text/plain", proj).verdict != V::Ok);
}

TEST_CASE("playlists are never handed to the OS launcher", "[open_media]")
{
    const std::string text = "#EXTM3U\nhttp://attacker.example/a.mp3\n";
    const std::vector<std::uint8_t> m3u(text.begin(), text.end());
    CHECK(tesseract::Client::check_media_file("song.m3u", "audio/x-mpegurl", m3u).verdict ==
          V::Executable);
    CHECK(tesseract::Client::check_media_file("attachment", "audio/x-mpegurl", m3u).verdict ==
          V::Executable);
    CHECK(tesseract::Client::check_media_file("song.mp3", "audio/mpeg", m3u).verdict ==
          V::Executable);
    // Stop here rather than launch a real player if the content sniff regressed.
    REQUIRE(tesseract::Client::check_media_file("song.mp3", "", m3u).verdict == V::Executable);

    auto a = tesseract::write_open_media_file("song.mp3", m3u);
    REQUIRE(a);
    CHECK_FALSE(tesseract::open_local_file(*a));
    auto b = tesseract::write_open_media_file("attachment", m3u);
    REQUIRE(b);
    CHECK_FALSE(tesseract::open_local_file(*b));
    tesseract::purge_open_media_dir();
}

TEST_CASE("open_local_file refuses executables even inside the media dir", "[open_media]")
{
    const std::vector<std::uint8_t> elf{0x7f, 'E', 'L', 'F', 2, 1, 1};
    auto p = tesseract::write_open_media_file("photo.png", elf);
    REQUIRE(p);
    CHECK_FALSE(tesseract::open_local_file(*p));
    auto q = tesseract::write_open_media_file("run.sh", {'e', 'c', 'h', 'o'});
    REQUIRE(q);
    CHECK_FALSE(tesseract::open_local_file(*q));
    tesseract::purge_open_media_dir();
}

TEST_CASE("open_media handles non-ASCII names without throwing", "[open_media]")
{
    // An ELF header: open_local_file refuses it after the name conversion, so
    // the test never launches a real viewer.
    const std::vector<std::uint8_t> data{0x7f, 'E', 'L', 'F', 2, 1, 1};
    auto p = tesseract::write_open_media_file("写真-\xF0\x9F\x93\xB7.png", data);
    REQUIRE(p);
    CHECK_NOTHROW(tesseract::open_local_file(*p));
    tesseract::purge_open_media_dir();
}

TEST_CASE("open_media shortens an over-long name and keeps its extension", "[open_media]")
{
    const std::vector<std::uint8_t> data{1, 2, 3};
    const std::string long_name = std::string(300, 'x') + ".png";
    auto written = tesseract::write_open_media_file(long_name, data);
    REQUIRE(written);
    const std::string got = written->filename().string();
    CHECK(got.size() <= 255);
    CHECK(got.ends_with(".png"));
#if defined(_WIN32)
    // Windows' whole-path limit shortens it further; it must still fit,
    // including the Zone.Identifier stream written beside the file.
    CHECK(written->native().size() + 16 < 260);
    CHECK(got.size() > 32); // cut to fit, not dropped
#else
    CHECK(got.size() > 200 - 1); // cut to the cap, not dropped
#endif
    CHECK(fs::file_size(*written) == 3);
    tesseract::purge_open_media_dir();
}

TEST_CASE("Client::sanitize_filename strips directories through the FFI", "[open_media]")
{
    using tesseract::Client;
    CHECK(Client::sanitize_filename("/home/u/.config/autostart/x.desktop") == "x.desktop");
    CHECK(Client::sanitize_filename("C:\\Users\\x\\a.png") == "a.png");
    CHECK(Client::sanitize_filename("CON.txt") == "_CON.txt");
    CHECK(Client::sanitize_filename("a\nb.txt") == "ab.txt");
    CHECK(Client::sanitize_filename("..").empty());
    CHECK(Client::sanitize_filename("").empty());
}
