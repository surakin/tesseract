#include <catch2/catch_test_macros.hpp>
#include <tesseract/settings.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Write arbitrary content to a file, creating parent directories first.
static void write_file(const fs::path& p, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// Helper: reset the singleton to defaults before each test.
static void reset_settings()
{
    tesseract::Settings::instance().theme_pref =
        tesseract::Settings::ThemePreference::System;
    tesseract::Settings::instance().notifications_enabled = true;
}

// Each test uses a unique subdirectory under the OS temp path.
static fs::path make_tmp_dir(const std::string& suffix)
{
    auto base =
        fs::temp_directory_path() / ("tesseract_test_settings_" + suffix);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

// ---------------------------------------------------------------------------

TEST_CASE("Settings round-trip: Light")
{
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

TEST_CASE("Settings round-trip: Dark")
{
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

TEST_CASE("Settings round-trip: System")
{
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

TEST_CASE("Settings load missing file defaults to System")
{
    reset_settings();
    auto dir = make_tmp_dir("missing");
    // Do not create app_settings.json.

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Light; // dirty state
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

TEST_CASE("Settings load unknown theme value defaults to System")
{
    reset_settings();
    auto dir = make_tmp_dir("unknown");
    write_file(dir / "app_settings.json", "{\"theme\":\"neon\"}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings load ignores extra unknown JSON fields")
{
    reset_settings();
    auto dir = make_tmp_dir("extra_fields");
    write_file(dir / "app_settings.json",
               "{\"future_option\":true,\"theme\":\"dark\",\"another\":42}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Dark);

    fs::remove_all(dir);
}

TEST_CASE("Settings save creates config directory if needed")
{
    reset_settings();
    // Use a nested path that does not yet exist.
    auto dir = make_tmp_dir("mkdir") / "sub" / "deeper";

    auto& s = tesseract::Settings::instance();
    s.theme_pref = tesseract::Settings::ThemePreference::Light;
    s.save_to_disk(dir); // must not throw or crash

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Light);

    fs::remove_all(make_tmp_dir("mkdir")); // clean up root
}

TEST_CASE("Settings notifications_enabled round-trip: false")
{
    reset_settings();
    auto dir = make_tmp_dir("notif_false");

    auto& s = tesseract::Settings::instance();
    s.notifications_enabled = false;
    s.save_to_disk(dir);

    reset_settings(); // resets notifications_enabled back to true
    s.load_from_disk(dir);
    REQUIRE(s.notifications_enabled == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings notifications_enabled round-trip: true")
{
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

TEST_CASE("Settings notifications_enabled missing key keeps default true")
{
    reset_settings();
    auto dir = make_tmp_dir("notif_missing_key");
    // Write a file with no notifications_enabled key at all.
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    REQUIRE(s.notifications_enabled == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings persist group_inactive_rooms + threshold", "[settings]")
{
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_inactive_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.group_inactive_rooms = true;
    s.inactive_room_threshold_days = 90;
    s.save_to_disk(dir);

    s.group_inactive_rooms = false;
    s.inactive_room_threshold_days = 30;
    s.load_from_disk(dir);

    CHECK(s.group_inactive_rooms == true);
    CHECK(s.inactive_room_threshold_days == 90);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Settings persist autoscroll_unread_rooms", "[settings]")
{
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_autoscroll_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.autoscroll_unread_rooms = false; // default is true
    s.save_to_disk(dir);

    s.autoscroll_unread_rooms = true;
    s.load_from_disk(dir);
    CHECK(s.autoscroll_unread_rooms == false);

    std::filesystem::remove_all(dir);
    s.autoscroll_unread_rooms = true; // restore default for other tests
}

TEST_CASE("Settings load wrong-typed field falls back to defaults", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("wrong_type");
    // notifications_enabled expects bool but receives a string — this causes
    // nlohmann::json::type_error (not parse_error) during field reading.
    write_file(dir / "app_settings.json",
               "{\"notifications_enabled\": \"yes\"}");

    auto& s = tesseract::Settings::instance();
    s.notifications_enabled = true; // default

    // Must not throw; load_from_disk should fall back to defaults on type_error.
    REQUIRE_NOTHROW(s.load_from_disk(dir));
    // The singleton should still hold its default value after the fallback.
    REQUIRE(s.notifications_enabled == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings round-trip: room section collapsed", "[settings]")
{
    auto dir = make_tmp_dir("section_collapsed");

    auto& s = tesseract::Settings::instance();
    // Set all flags to their non-default values.
    s.room_section_invites_collapsed   = true;
    s.room_section_favorites_collapsed = true;
    s.room_section_dms_collapsed       = true;
    s.room_section_rooms_collapsed     = true;
    s.room_section_spaces_collapsed    = true;
    s.room_section_inactive_collapsed  = false; // default is true, flip it

    s.save_to_disk(dir);

    // Reset to defaults before reload.
    s.room_section_invites_collapsed   = false;
    s.room_section_favorites_collapsed = false;
    s.room_section_dms_collapsed       = false;
    s.room_section_rooms_collapsed     = false;
    s.room_section_spaces_collapsed    = false;
    s.room_section_inactive_collapsed  = true;

    s.load_from_disk(dir);

    CHECK(s.room_section_invites_collapsed   == true);
    CHECK(s.room_section_favorites_collapsed == true);
    CHECK(s.room_section_dms_collapsed       == true);
    CHECK(s.room_section_rooms_collapsed     == true);
    CHECK(s.room_section_spaces_collapsed    == true);
    CHECK(s.room_section_inactive_collapsed  == false);

    fs::remove_all(dir);
}
