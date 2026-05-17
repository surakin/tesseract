#include <catch2/catch_test_macros.hpp>
#include <tesseract/settings.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Write arbitrary content to a file, creating parent directories first.
static void write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// Helper: reset the singleton to defaults before each test.
static void reset_settings() {
    tesseract::Settings::instance().theme_pref =
        tesseract::Settings::ThemePreference::System;
    tesseract::Settings::instance().notifications_enabled = true;
}

// Each test uses a unique subdirectory under the OS temp path.
static fs::path make_tmp_dir(const std::string& suffix) {
    auto base = fs::temp_directory_path() / ("tesseract_test_settings_" + suffix);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

// ---------------------------------------------------------------------------

TEST_CASE("Settings round-trip: Light") {
    reset_settings();
    auto dir = make_tmp_dir("light");

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Light;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Light);

    fs::remove_all(dir);
}

TEST_CASE("Settings round-trip: Dark") {
    reset_settings();
    auto dir = make_tmp_dir("dark");

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Dark;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Dark);

    fs::remove_all(dir);
}

TEST_CASE("Settings round-trip: System") {
    reset_settings();
    auto dir = make_tmp_dir("system");

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::System;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings load missing file defaults to System") {
    reset_settings();
    auto dir = make_tmp_dir("missing");
    // Do not create app_settings.json.

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Light;  // dirty state
    s.load_from_disk(dir);
    // load_from_disk is a no-op when the file is absent — value stays as-is.
    // The documented contract is "missing file → keep defaults", which means
    // the caller is responsible for resetting before calling load_from_disk.
    // Here we verify the call doesn't crash or change the value unexpectedly
    // (no-op semantic: the function returns without touching theme_pref).
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Light);

    // Fresh state: starts at System, file absent → still System.
    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings load unknown theme value defaults to System") {
    reset_settings();
    auto dir = make_tmp_dir("unknown");
    write_file(dir / "app_settings.json", "{\"theme\":\"neon\"}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings load ignores extra unknown JSON fields") {
    reset_settings();
    auto dir = make_tmp_dir("extra_fields");
    write_file(dir / "app_settings.json",
               "{\"future_option\":true,\"theme\":\"dark\",\"another\":42}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Dark);

    fs::remove_all(dir);
}

TEST_CASE("Settings save creates config directory if needed") {
    reset_settings();
    // Use a nested path that does not yet exist.
    auto dir = make_tmp_dir("mkdir") / "sub" / "deeper";

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Light;
    s.save_to_disk(dir);  // must not throw or crash

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Light);

    fs::remove_all(make_tmp_dir("mkdir"));  // clean up root
}

TEST_CASE("Settings notifications_enabled round-trip: false") {
    reset_settings();
    auto dir = make_tmp_dir("notif_false");

    auto& s = tesseract::Settings::instance();
    s.notifications_enabled = false;
    s.save_to_disk(dir);

    reset_settings();  // resets notifications_enabled back to true
    s.load_from_disk(dir);
    REQUIRE(s.notifications_enabled == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings notifications_enabled round-trip: true") {
    reset_settings();
    auto dir = make_tmp_dir("notif_true");

    auto& s = tesseract::Settings::instance();
    s.notifications_enabled = true;
    s.save_to_disk(dir);

    // Dirty the field first so we prove load actually sets it.
    s.notifications_enabled = false;
    s.load_from_disk(dir);
    REQUIRE(s.notifications_enabled == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings notifications_enabled missing key keeps default true") {
    reset_settings();
    auto dir = make_tmp_dir("notif_missing_key");
    // Write a file with no notifications_enabled key at all.
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    REQUIRE(s.notifications_enabled == true);

    fs::remove_all(dir);
}
