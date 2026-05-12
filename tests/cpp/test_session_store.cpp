#include <catch2/catch_test_macros.hpp>
#include "tesseract/session_store.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test fixture: redirect SessionStore to a private temp directory so tests
// don't touch the real user config and clean up after themselves.
// ---------------------------------------------------------------------------
struct SessionFixture {
    std::string dir;
#if defined(__APPLE__)
    std::string saved_home;
#endif

    SessionFixture() {
        static std::atomic<int> counter{0};
        auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir = (fs::temp_directory_path()
               / ("tesseract_unit_tests_" + std::to_string(n))).string();
        fs::create_directories(dir);
#if defined(_WIN32)
        _putenv_s("APPDATA", dir.c_str());
#elif defined(__APPLE__)
        // macOS config_dir() uses $HOME/Library/Application Support — redirect HOME.
        if (const char* h = std::getenv("HOME")) saved_home = h;
        setenv("HOME", dir.c_str(), 1);
#else
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#endif
    }

    ~SessionFixture() {
#if defined(_WIN32)
        _putenv_s("APPDATA", "");
#elif defined(__APPLE__)
        if (saved_home.empty())
            unsetenv("HOME");
        else
            setenv("HOME", saved_home.c_str(), 1);
#else
        unsetenv("XDG_CONFIG_HOME");
#endif
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// ---------------------------------------------------------------------------

TEST_CASE("path() ends with session.json", "[session_store]") {
    SessionFixture f;
    auto p = tesseract::SessionStore::path();
    CHECK(p.ends_with("session.json"));
}

TEST_CASE("save + load round-trips content", "[session_store]") {
    SessionFixture f;
    const std::string json = R"({"client_id":"test","token":"abc"})";

    REQUIRE(tesseract::SessionStore::save(json));
    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);
}

TEST_CASE("load returns nullopt when no file exists", "[session_store]") {
    SessionFixture f;
    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("save creates parent directories on first write", "[session_store]") {
    SessionFixture f;
    fs::remove_all(f.dir);  // wipe everything under the temp base

    REQUIRE(tesseract::SessionStore::save(R"({"ok":true})"));
    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == R"({"ok":true})");
}

TEST_CASE("clear removes the session file", "[session_store]") {
    SessionFixture f;
    REQUIRE(tesseract::SessionStore::save(R"({"x":1})"));
    REQUIRE(tesseract::SessionStore::load().has_value());

    tesseract::SessionStore::clear();
    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("load returns nullopt for an empty file", "[session_store]") {
    SessionFixture f;
    auto p = tesseract::SessionStore::path();
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream{p}.close();  // create an empty file

    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("save overwrites existing content", "[session_store]") {
    SessionFixture f;
    REQUIRE(tesseract::SessionStore::save(R"({"v":1})"));
    REQUIRE(tesseract::SessionStore::save(R"({"v":2})"));

    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == R"({"v":2})");
}
