#include <catch2/catch_test_macros.hpp>
#include "tesseract/paths.h"
#include "tesseract/secret_store.h"
#include "tesseract/session_store.h"

#include <filesystem>

TEST_CASE("config_dir returns a non-empty path", "[paths]")
{
    auto p = tesseract::config_dir();
    CHECK_FALSE(p.empty());
    // Path is absolute on every supported platform.
    CHECK(p.is_absolute());
}

TEST_CASE("data_dir returns a non-empty absolute path", "[paths]")
{
    auto p = tesseract::data_dir();
    CHECK_FALSE(p.empty());
    CHECK(p.is_absolute());
#if defined(_WIN32) || defined(__APPLE__)
    // No XDG-style data/config split on these platforms.
    CHECK(p == tesseract::config_dir());
#endif
}

TEST_CASE("SessionStore::path lives under config_dir", "[paths][session]")
{
    auto cfg = tesseract::config_dir();
    auto session = std::filesystem::path(tesseract::SessionStore::path());
    CHECK(session.parent_path() == cfg);
    CHECK(session.filename() == "session.json");
}

TEST_CASE("is_valid_profile_name accepts only safe names", "[paths][profile]")
{
    CHECK(tesseract::is_valid_profile_name("work"));
    CHECK(tesseract::is_valid_profile_name("Test_2-b"));
    CHECK(tesseract::is_valid_profile_name(std::string(32, 'a')));
    CHECK_FALSE(tesseract::is_valid_profile_name(""));
    CHECK_FALSE(tesseract::is_valid_profile_name(std::string(33, 'a')));
    CHECK_FALSE(tesseract::is_valid_profile_name("../x"));
    CHECK_FALSE(tesseract::is_valid_profile_name("a b"));
    CHECK_FALSE(tesseract::is_valid_profile_name("a/b"));
    CHECK_FALSE(tesseract::is_valid_profile_name("é"));
}

TEST_CASE("set_profile suffixes every app folder", "[paths][profile]")
{
    const auto cfg = tesseract::config_dir();
    const auto data = tesseract::data_dir();
    const auto cache = tesseract::cache_dir();
    REQUIRE(tesseract::profile().empty());
    CHECK(tesseract::profile_suffix().empty());

    tesseract::set_profile("work");
    CHECK(tesseract::profile() == "work");
    CHECK(tesseract::profile_suffix() == "-work");
    CHECK(tesseract::config_dir() ==
          cfg.parent_path() / (cfg.filename().string() + "-work"));
    CHECK(tesseract::data_dir() ==
          data.parent_path() / (data.filename().string() + "-work"));
    CHECK(tesseract::cache_dir() ==
          cache.parent_path() / (cache.filename().string() + "-work"));

    // Invalid names are ignored, empty restores the default.
    tesseract::set_profile("../evil");
    CHECK(tesseract::profile() == "work");
    tesseract::set_profile("");
    CHECK(tesseract::profile().empty());
    CHECK(tesseract::config_dir() == cfg);
}

TEST_CASE("SecretStore::key_for scopes by profile", "[paths][profile]")
{
    REQUIRE(tesseract::profile().empty());
    CHECK(tesseract::SecretStore::key_for("@a:b.org") == "@a:b.org");
    tesseract::set_profile("work");
    CHECK(tesseract::SecretStore::key_for("@a:b.org") == "profile:work/@a:b.org");
    tesseract::set_profile("");
    CHECK(tesseract::SecretStore::key_for("@a:b.org") == "@a:b.org");
}
