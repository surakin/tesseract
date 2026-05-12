#include <catch2/catch_test_macros.hpp>
#include "tesseract/paths.h"
#include "tesseract/session_store.h"

#include <filesystem>

TEST_CASE("config_dir returns a non-empty path", "[paths]") {
    auto p = tesseract::config_dir();
    CHECK_FALSE(p.empty());
    // Path is absolute on every supported platform.
    CHECK(p.is_absolute());
}

TEST_CASE("SessionStore::path lives under config_dir", "[paths][session]") {
    auto cfg = tesseract::config_dir();
    auto session = std::filesystem::path(tesseract::SessionStore::path());
    CHECK(session.parent_path() == cfg);
    CHECK(session.filename() == "session.json");
}
