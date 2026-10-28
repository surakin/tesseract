#include <catch2/catch_test_macros.hpp>

// single_instance.cpp is compiled into tesseract_tk for Linux and macOS only.
#if !defined(_WIN32)

#include "platform/single_instance.h"

#include <tesseract/paths.h>

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

TEST_CASE("activation payload: round trip")
{
    tk::ActivationRequest req{"tok", "matrix:r/room:example.org",
                              "open-room", "!abc:example.org"};
    const auto back = tk::parse_activation_payload(tk::format_activation_payload(req));
    CHECK(back.token == req.token);
    CHECK(back.uri == req.uri);
    CHECK(back.action == req.action);
    CHECK(back.room_id == req.room_id);
}

TEST_CASE("activation payload: legacy two-line form")
{
    const auto req = tk::parse_activation_payload("tok\nmatrix:u/a:b\n");
    CHECK(req.token == "tok");
    CHECK(req.uri == "matrix:u/a:b");
    CHECK(req.action.empty());
    CHECK(req.room_id.empty());
}

TEST_CASE("activation payload: empty and unterminated input")
{
    CHECK(tk::parse_activation_payload("").token.empty());
    const auto req = tk::parse_activation_payload("\n\nopen-settings");
    CHECK(req.token.empty());
    CHECK(req.uri.empty());
    CHECK(req.action == "open-settings");
}

TEST_CASE("activation payload: embedded newlines cannot inject fields")
{
    tk::ActivationRequest req{"", "matrix:u/a:b\nopen-settings", "", ""};
    const auto back = tk::parse_activation_payload(tk::format_activation_payload(req));
    CHECK(back.uri == "matrix:u/a:bopen-settings");
    CHECK(back.action.empty());
}

namespace
{

// Sets/unsets an environment variable for one test and restores it afterwards.
struct EnvGuard
{
    std::string name;
    std::optional<std::string> saved;
    EnvGuard(const char* var, const char* value) : name(var)
    {
        if (const char* v = std::getenv(var))
            saved = v;
        if (value)
            setenv(var, value, 1);
        else
            unsetenv(var);
    }
    ~EnvGuard()
    {
        if (saved)
            setenv(name.c_str(), saved->c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

struct RuntimeDirEnv : EnvGuard
{
    explicit RuntimeDirEnv(const char* value) : EnvGuard("XDG_RUNTIME_DIR", value) {}
};

// Switches to a throwaway profile so lock tests never touch a real instance.
struct ProfileGuard
{
    std::string saved = tesseract::profile();
    explicit ProfileGuard(const std::string& tag)
    {
        tesseract::set_profile(tag + std::to_string(getpid() % 100000));
    }
    ~ProfileGuard()
    {
        tesseract::set_profile(saved);
    }
};

std::filesystem::path make_temp_dir(const char* tag, std::filesystem::perms perms)
{
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() /
                 ("tesseract-test-" + std::string(tag) + "-" + std::to_string(getpid()));
    fs::remove_all(p);
    fs::create_directory(p);
    fs::permissions(p, perms);
    return p;
}

bool is_private_dir(const std::string& path)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 077) == 0;
}

} // namespace

#if !defined(__APPLE__)
TEST_CASE("private_runtime_dir: uses a private XDG_RUNTIME_DIR")
{
    namespace fs = std::filesystem;
    const fs::path dir = make_temp_dir("xdg-ok", fs::perms::owner_all);
    RuntimeDirEnv env(dir.c_str());
    CHECK(tk::private_runtime_dir() == dir.string());
    fs::remove_all(dir);
}

TEST_CASE("private_runtime_dir: rejects a group/world-accessible XDG_RUNTIME_DIR")
{
    namespace fs = std::filesystem;
    const fs::path dir = make_temp_dir(
        "xdg-open", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec);
    RuntimeDirEnv env(dir.c_str());
    const std::string got = tk::private_runtime_dir();
    CHECK(got != dir.string());
    CHECK(is_private_dir(got));
    fs::remove_all(dir);
}

TEST_CASE("private_runtime_dir: rejects a symlinked XDG_RUNTIME_DIR")
{
    namespace fs = std::filesystem;
    const fs::path target = make_temp_dir("xdg-target", fs::perms::owner_all);
    const fs::path link = target.string() + "-link";
    fs::remove(link);
    fs::create_directory_symlink(target, link);
    RuntimeDirEnv env(link.c_str());
    CHECK(tk::private_runtime_dir() != link.string());
    fs::remove(link);
    fs::remove_all(target);
}

TEST_CASE("private_runtime_dir: falls back to a private /tmp dir without XDG_RUNTIME_DIR")
{
    RuntimeDirEnv env(nullptr);
    const std::string got = tk::private_runtime_dir();
    CHECK(got == "/tmp/tesseract-" + std::to_string(getuid()));
    CHECK(is_private_dir(got));
}

TEST_CASE("private_runtime_dir: Flatpak uses the per-app subdirectory")
{
    namespace fs = std::filesystem;
    const fs::path dir = make_temp_dir("xdg-flatpak", fs::perms::owner_all);
    fs::create_directories(dir / "app" / "io.example.Test");
    fs::permissions(dir / "app" / "io.example.Test", fs::perms::owner_all);
    RuntimeDirEnv env(dir.c_str());
    EnvGuard flatpak("FLATPAK_ID", "io.example.Test");
    CHECK(tk::private_runtime_dir() == (dir / "app" / "io.example.Test").string());
    fs::remove_all(dir);
}

TEST_CASE("private_runtime_dir: Flatpak without the app subdirectory uses XDG_RUNTIME_DIR")
{
    namespace fs = std::filesystem;
    const fs::path dir = make_temp_dir("xdg-flatpak-missing", fs::perms::owner_all);
    RuntimeDirEnv env(dir.c_str());
    EnvGuard flatpak("FLATPAK_ID", "io.example.Test");
    CHECK(tk::private_runtime_dir() == dir.string());
    fs::remove_all(dir);
}
#endif

namespace
{

std::string legacy_lock_for_test()
{
    return "/tmp/tesseract-" + std::to_string(getuid()) + tesseract::profile_suffix() + ".lock";
}

void remove_new_lock_files()
{
    const std::string dir = tk::private_runtime_dir();
    if (!dir.empty())
        unlink((dir + "/tesseract" + tesseract::profile_suffix() + ".lock").c_str());
}

} // namespace

TEST_CASE("legacy lock held by another process blocks acquire")
{
    ProfileGuard profile("sitestA");
    const std::string legacy = legacy_lock_for_test();
    int setup = open(legacy.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    REQUIRE(setup >= 0);
    close(setup);

    int ready[2];
    int release[2];
    REQUIRE(pipe(ready) == 0);
    REQUIRE(pipe(release) == 0);
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        close(ready[0]);
        close(release[1]);
        int fd = open(legacy.c_str(), O_RDWR);
        const bool locked = fd >= 0 && flock(fd, LOCK_EX) == 0;
        const char b = locked ? 'y' : 'n';
        [[maybe_unused]] auto w = write(ready[1], &b, 1);
        char c;
        [[maybe_unused]] auto r = read(release[0], &c, 1);
        _exit(0);
    }
    close(ready[1]);
    close(release[0]);
    char got = 0;
    REQUIRE(read(ready[0], &got, 1) == 1);
    REQUIRE(got == 'y');

    CHECK_FALSE(tk::acquire_single_instance_lock(std::chrono::milliseconds(300)).acquired);

    close(release[1]);
    close(ready[0]);
    waitpid(child, nullptr, 0);
    unlink(legacy.c_str());
    remove_new_lock_files();
}

TEST_CASE("stale legacy lock with no holder does not block")
{
    ProfileGuard profile("sitestB");
    const std::string legacy = legacy_lock_for_test();
    int fd = open(legacy.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    close(fd);

    CHECK(tk::acquire_single_instance_lock(std::chrono::milliseconds(300)).acquired);

    unlink(legacy.c_str());
    remove_new_lock_files();
}

#endif
