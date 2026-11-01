// launch_detached is POSIX-only (Windows opens links through ShellExecute).
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "detached_launch.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{

// Writes an executable shell script to `path`.
void write_detached_launch_script(const fs::path& path, const std::string& body)
{
    {
        std::ofstream f(path);
        f << "#!/bin/sh\n" << body << "\n";
    }
    fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
}

std::string read_detached_launch_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

struct DetachedLaunchTmpDir
{
    fs::path path;
    // Unique per test, so tests that run in parallel never remove each other's files.
    DetachedLaunchTmpDir()
        : path(fs::temp_directory_path() /
               ("tesseract_test_detached_launch_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~DetachedLaunchTmpDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("launch_detached dispatches the opener and reports success", "[detached_launch]")
{
    CHECK(tesseract::detail::launch_detached("true", "unused"));
}

TEST_CASE("launch_detached passes the argument to the opener unchanged", "[detached_launch]")
{
    DetachedLaunchTmpDir dir;
    const fs::path out = dir.path / "argv1.txt";
    const fs::path opener = dir.path / "record-arg.sh";
    write_detached_launch_script(opener, "printf '%s' \"$1\" > \"" + out.string() + "\"");

    // Quotes, a command substitution, a backtick and a semicolon must reach the
    // opener as plain text, not be run by the launcher.
    const std::string arg = "it's \"quoted\" $(echo no) `echo no`; echo no";
    REQUIRE(tesseract::detail::launch_detached(opener.string().c_str(), arg));

    // The opener runs in the background, so wait for its output.
    for (int i = 0; i < 100 && read_detached_launch_file(out) != arg; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(read_detached_launch_file(out) == arg);
}

TEST_CASE("launch_detached returns without waiting for the opener", "[detached_launch]")
{
    DetachedLaunchTmpDir dir;
    const fs::path opener = dir.path / "slow-opener.sh";
    write_detached_launch_script(opener, "sleep 4");

    const auto start = std::chrono::steady_clock::now();
    REQUIRE(tesseract::detail::launch_detached(opener.string().c_str(), "unused"));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(2));
}

#endif // !_WIN32
