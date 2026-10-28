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
    tesseract::Settings::instance().theme_accent =
        tesseract::Settings::ThemeAccent::System;
    tesseract::Settings::instance().notifications_enabled = true;
    tesseract::Settings::instance().audio_input_device_id  = {};
    tesseract::Settings::instance().audio_output_device_id = {};
    tesseract::Settings::instance().camera_device_id       = {};
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

TEST_CASE("Settings animate_avatars defaults on and round-trips")
{
    reset_settings();
    auto dir = make_tmp_dir("animate_avatars");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir); // no file → defaults
    CHECK(s.animate_avatars);

    s.animate_avatars = false;
    s.save_to_disk(dir);
    reset_settings();
    s.load_from_disk(dir);
    CHECK_FALSE(s.animate_avatars);

    reset_settings();
    fs::remove_all(dir);
}

TEST_CASE("Settings proxy_mode and proxy_url round-trip")
{
    using PM = tesseract::Settings::ProxyMode;
    reset_settings();
    auto dir = make_tmp_dir("proxy");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir); // no file → defaults
    CHECK(s.proxy_mode == PM::System);
    CHECK(s.proxy_url.empty());

    s.proxy_mode = PM::Manual;
    s.proxy_url = "http://u:p@proxy.example.com:3128";
    s.save_to_disk(dir);
    s.proxy_mode = PM::System;
    s.proxy_url.clear();
    s.load_from_disk(dir);
    CHECK(s.proxy_mode == PM::Manual);
    CHECK(s.proxy_url == "http://u:p@proxy.example.com:3128");

    s.proxy_mode = PM::None;
    s.save_to_disk(dir);
    s.proxy_mode = PM::System;
    s.load_from_disk(dir);
    CHECK(s.proxy_mode == PM::None);

    s.proxy_mode = PM::System;
    s.proxy_url.clear();
    reset_settings();
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

TEST_CASE("Settings theme_accent round-trip: Blue")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_blue");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Blue;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::Blue);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent round-trip: Forest")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_forest");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Forest;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::Forest);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent round-trip: Sunset")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_sunset");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Sunset;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::Sunset);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent round-trip: Violet")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_violet");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Violet;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::Violet);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent round-trip: System")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_system");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::System;
    s.save_to_disk(dir);

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent missing key defaults to System")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_missing");
    // A settings file predating theme_accent has no such key at all -- this
    // must preserve Windows' pre-existing "always match the OS accent color"
    // behavior, so it defaults to System rather than a fixed accent.
    write_file(dir / "app_settings.json", R"({"theme":"dark"})");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Violet; // dirty state
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::System);

    fs::remove_all(dir);
}

TEST_CASE("Settings theme_accent unknown value defaults to System")
{
    reset_settings();
    auto dir = make_tmp_dir("accent_unknown");
    write_file(dir / "app_settings.json", R"({"theme_accent":"chartreuse"})");

    auto& s = tesseract::Settings::instance();
    s.theme_accent = tesseract::Settings::ThemeAccent::Violet; // dirty state
    s.load_from_disk(dir);
    CHECK(s.theme_accent == tesseract::Settings::ThemeAccent::System);

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

TEST_CASE("Settings launch_at_login round-trip: true", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("launch_at_login_true");

    auto& s = tesseract::Settings::instance();
    s.launch_at_login = true;
    s.save_to_disk(dir);

    s.launch_at_login = false;
    s.load_from_disk(dir);
    CHECK(s.launch_at_login == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings launch_at_login round-trip: false", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("launch_at_login_false");

    auto& s = tesseract::Settings::instance();
    s.launch_at_login = false;
    s.save_to_disk(dir);

    s.launch_at_login = true;
    s.load_from_disk(dir);
    CHECK(s.launch_at_login == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings launch_at_login missing key defaults to false", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("launch_at_login_missing_key");
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.launch_at_login = true; // dirty it first to prove load resets it
    s.load_from_disk(dir);
    CHECK(s.launch_at_login == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings close_action round-trip: each value", "[settings]")
{
    using CA = tesseract::Settings::CloseAction;
    for (const CA v : {CA::Quit, CA::Minimize, CA::HideToTray})
    {
        reset_settings();
        auto dir = make_tmp_dir("close_action_round_trip");

        auto& s = tesseract::Settings::instance();
        s.close_action = v;
        s.save_to_disk(dir);

        // Dirty it to a different value so the load can't pass by accident.
        s.close_action = CA::Minimize;
        s.load_from_disk(dir);
        CHECK(s.close_action == v);

        fs::remove_all(dir);
    }
}

TEST_CASE("Settings close_action missing key defaults to HideToTray", "[settings]")
{
    using CA = tesseract::Settings::CloseAction;
    reset_settings();
    auto dir = make_tmp_dir("close_action_missing_key");
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.close_action = CA::Quit; // dirty it first to prove load resets it
    s.load_from_disk(dir);
    CHECK(s.close_action == CA::HideToTray);

    fs::remove_all(dir);
}

// An older build, or a hand-edited file, could carry a value this build does
// not know. It must not silently become Quit — that would make closing the
// window start killing the app for users who never asked for that.
TEST_CASE("Settings close_action unknown value defaults to HideToTray", "[settings]")
{
    using CA = tesseract::Settings::CloseAction;
    reset_settings();
    auto dir = make_tmp_dir("close_action_unknown");
    write_file(dir / "app_settings.json",
               "{\"close_action\":\"definitely-not-a-real-action\"}");

    auto& s = tesseract::Settings::instance();
    s.close_action = CA::Quit;
    s.load_from_disk(dir);
    CHECK(s.close_action == CA::HideToTray);

    fs::remove_all(dir);
}

TEST_CASE("Settings start_minimized round-trip: true", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("start_minimized_true");

    auto& s = tesseract::Settings::instance();
    s.start_minimized = true;
    s.save_to_disk(dir);

    s.start_minimized = false;
    s.load_from_disk(dir);
    CHECK(s.start_minimized == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings start_minimized round-trip: false", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("start_minimized_false");

    auto& s = tesseract::Settings::instance();
    s.start_minimized = false;
    s.save_to_disk(dir);

    s.start_minimized = true;
    s.load_from_disk(dir);
    CHECK(s.start_minimized == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings start_minimized missing key defaults to false", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("start_minimized_missing_key");
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.start_minimized = true; // dirty it first to prove load resets it
    s.load_from_disk(dir);
    CHECK(s.start_minimized == false);

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

TEST_CASE("Settings persist sidebar_width + sidebar_collapsed", "[settings]")
{
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_sidebar_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.sidebar_width = 337;
    s.sidebar_collapsed = true;
    s.save_to_disk(dir);

    s.sidebar_width = 0;
    s.sidebar_collapsed = false;
    s.load_from_disk(dir);

    CHECK(s.sidebar_width == 337);
    CHECK(s.sidebar_collapsed == true);

    // Missing keys keep defaults.
    std::filesystem::remove_all(dir);
    s.sidebar_width = 200;
    s.sidebar_collapsed = true;
    s.load_from_disk(dir); // no file → no-op
    CHECK(s.sidebar_width == 200);

    std::filesystem::remove_all(dir);
    s.sidebar_width = 0;
    s.sidebar_collapsed = false;
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

TEST_CASE("Settings persist message_layout", "[settings]")
{
    using ML = tesseract::Settings::MessageLayout;
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_layout_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.message_layout = ML::Bubbles; // default is Classic
    s.save_to_disk(dir);

    s.message_layout = ML::Classic;
    s.load_from_disk(dir);
    CHECK(s.message_layout == ML::Bubbles);

    s.message_layout = ML::Irc;
    s.save_to_disk(dir);
    s.message_layout = ML::Classic;
    s.load_from_disk(dir);
    CHECK(s.message_layout == ML::Irc);

    std::filesystem::remove_all(dir);
    s.message_layout = ML::Classic; // restore default for other tests
}

TEST_CASE("Settings persist room_list_density", "[settings]")
{
    using RD = tesseract::Settings::RoomListDensity;
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_density_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.room_list_density = RD::Compact; // default is Normal
    s.save_to_disk(dir);
    s.room_list_density = RD::Normal;
    s.load_from_disk(dir);
    CHECK(s.room_list_density == RD::Compact);

    std::filesystem::remove_all(dir);
    s.room_list_density = RD::Normal;
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

TEST_CASE("Settings round-trip: group_unread_rooms + room_section_unread_collapsed",
          "[settings]")
{
    auto dir = make_tmp_dir("group_unread");

    auto& s = tesseract::Settings::instance();
    s.group_unread_rooms            = true;
    s.room_section_unread_collapsed = true;
    s.save_to_disk(dir);

    s.group_unread_rooms            = false;
    s.room_section_unread_collapsed = false;
    s.load_from_disk(dir);

    CHECK(s.group_unread_rooms == true);
    CHECK(s.room_section_unread_collapsed == true);

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

TEST_CASE("device IDs default to empty string", "[settings][audio_video]")
{
    reset_settings();
    REQUIRE(tesseract::Settings::instance().audio_input_device_id.empty());
    REQUIRE(tesseract::Settings::instance().audio_output_device_id.empty());
    REQUIRE(tesseract::Settings::instance().camera_device_id.empty());
}

TEST_CASE("device IDs round-trip through disk", "[settings][audio_video]")
{
    reset_settings();
    auto dir = make_tmp_dir("device_ids");

    tesseract::Settings::instance().audio_input_device_id  = "hw:0,0";
    tesseract::Settings::instance().audio_output_device_id = "hw:0,1";
    tesseract::Settings::instance().camera_device_id       = "/dev/video0";
    tesseract::Settings::instance().save_to_disk(dir);

    reset_settings(); // wipe in-memory state
    tesseract::Settings::instance().load_from_disk(dir);

    REQUIRE(tesseract::Settings::instance().audio_input_device_id  == "hw:0,0");
    REQUIRE(tesseract::Settings::instance().audio_output_device_id == "hw:0,1");
    REQUIRE(tesseract::Settings::instance().camera_device_id       == "/dev/video0");
}

TEST_CASE("missing device ID keys load as empty string", "[settings][audio_video]")
{
    reset_settings();
    auto dir = make_tmp_dir("device_ids_missing");
    write_file(dir / "app_settings.json", R"({"theme":"light"})");
    tesseract::Settings::instance().load_from_disk(dir);
    REQUIRE(tesseract::Settings::instance().audio_input_device_id.empty());
    REQUIRE(tesseract::Settings::instance().audio_output_device_id.empty());
    REQUIRE(tesseract::Settings::instance().camera_device_id.empty());
}

TEST_CASE("Settings show_sender_status_in_timeline round-trip", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("sender_status_roundtrip");

    auto& s = tesseract::Settings::instance();
    s.show_sender_status_in_timeline = true;
    s.save_to_disk(dir);

    s.show_sender_status_in_timeline = false;
    s.load_from_disk(dir);
    CHECK(s.show_sender_status_in_timeline == true);

    fs::remove_all(dir);
}

TEST_CASE("Settings show_sender_status_in_timeline missing key defaults to false",
          "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("sender_status_missing_key");
    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");

    auto& s = tesseract::Settings::instance();
    s.show_sender_status_in_timeline = true;
    s.load_from_disk(dir);
    CHECK(s.show_sender_status_in_timeline == false);

    fs::remove_all(dir);
}

TEST_CASE("Settings fonts default to empty and round-trip by name")
{
    reset_settings();
    auto dir = make_tmp_dir("fonts");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir); // no file → defaults
    CHECK(s.fonts == tesseract::Settings::FontPrefs{});

    s.fonts.base_pt = 14;
    s.fonts.ui_family = "Inter";
    s.fonts.mono_family = "Fira Code";
    s.fonts.categories[3].delta_pt = 2;
    s.fonts.categories[3].family = "Georgia";
    s.save_to_disk(dir);

    const auto saved = s.fonts;
    s.fonts = {};
    s.load_from_disk(dir);
    CHECK(s.fonts == saved);

    // A file predating the key loads as defaults, not stale values.
    write_file(dir / "app_settings.json", "{}");
    s.load_from_disk(dir);
    CHECK(s.fonts == tesseract::Settings::FontPrefs{});

    // Out-of-range values are clamped on load.
    write_file(dir / "app_settings.json",
               R"({"fonts":{"base_pt":999,"categories":{"headings":{"delta":-50}}}})");
    s.load_from_disk(dir);
    CHECK(s.fonts.base_pt == 36);
    CHECK(s.fonts.categories[3].delta_pt == -6);

    s.fonts = {};
    fs::remove_all(dir);
}

TEST_CASE("Settings: a corrupt file is moved aside and the last good copy is used")
{
    reset_settings();
    auto dir = make_tmp_dir("corrupt_recovery");
    auto& s = tesseract::Settings::instance();

    s.theme_pref = tesseract::Settings::ThemePreference::Dark;
    s.save_to_disk(dir);
    s.theme_pref = tesseract::Settings::ThemePreference::Light;
    s.save_to_disk(dir); // keeps the Dark version as the last good copy
    CHECK(fs::exists(dir / "app_settings.json.last-good"));
    CHECK_FALSE(fs::exists(dir / "app_settings.json.tmp")); // atomic write leaves no temp

    // A crash / disk-full leaves a truncated file.
    write_file(dir / "app_settings.json", "{\"theme\": \"li");

    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Dark); // last good
    CHECK(fs::exists(dir / "app_settings.json.corrupt"));              // kept for inspection
    CHECK_FALSE(fs::exists(dir / "app_settings.json"));                // not left to be overwritten

    fs::remove_all(dir);
}

TEST_CASE("Settings: a corrupt file without a last good copy falls back to defaults")
{
    reset_settings();
    auto dir = make_tmp_dir("corrupt_no_backup");
    write_file(dir / "app_settings.json", "not json at all");

    auto& s = tesseract::Settings::instance();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::System);
    CHECK(fs::exists(dir / "app_settings.json.corrupt"));

    // The next ordinary save writes a fresh, valid file.
    s.theme_pref = tesseract::Settings::ThemePreference::Dark;
    s.save_to_disk(dir);
    reset_settings();
    s.load_from_disk(dir);
    CHECK(s.theme_pref == tesseract::Settings::ThemePreference::Dark);

    fs::remove_all(dir);
}

TEST_CASE("Settings: an empty or non-object file is treated as corrupt")
{
    for (const char* body : {"", "42", "[1,2]"})
    {
        reset_settings();
        auto dir = make_tmp_dir("corrupt_shapes");
        write_file(dir / "app_settings.json", body);
        tesseract::Settings::instance().load_from_disk(dir);
        CHECK(fs::exists(dir / "app_settings.json.corrupt"));
        fs::remove_all(dir);
    }
}

TEST_CASE("split_proxy_credentials separates the userinfo", "[settings][proxy]")
{
    using namespace tesseract;
    auto p = split_proxy_credentials("http://bob:s3cret@proxy.example:3128");
    CHECK(p.url == "http://proxy.example:3128");
    CHECK(p.userinfo == "bob:s3cret");
    CHECK(join_proxy_credentials(p.url, p.userinfo) == "http://bob:s3cret@proxy.example:3128");

    // An @ after the authority belongs to the path, not to the credentials.
    p = split_proxy_credentials("http://proxy.example:3128/a@b");
    CHECK(p.url == "http://proxy.example:3128/a@b");
    CHECK(p.userinfo.empty());

    // A password containing @ (last @ wins).
    p = split_proxy_credentials("https://u:p@ss@host:1");
    CHECK(p.url == "https://host:1");
    CHECK(p.userinfo == "u:p@ss");

    CHECK(split_proxy_credentials("garbage").userinfo.empty());
    CHECK(join_proxy_credentials("http://h:1", "").empty() == false);
    CHECK(join_proxy_credentials("garbage", "u:p") == "garbage");
}

TEST_CASE("Settings timeline column defaults to the full width, left-anchored")
{
    reset_settings();
    auto dir = make_tmp_dir("column_defaults");
    auto& s = tesseract::Settings::instance();
    s.timeline_column_pct = 77;
    s.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Center;
    write_file(dir / "app_settings.json", R"({"theme":"dark"})");
    s.load_from_disk(dir);
    CHECK(s.timeline_column_pct == 100);
    CHECK(s.timeline_column_anchor == tesseract::Settings::TimelineColumnAnchor::Left);
    fs::remove_all(dir);
}

TEST_CASE("Settings timeline column round-trips")
{
    reset_settings();
    auto dir = make_tmp_dir("column_roundtrip");
    auto& s = tesseract::Settings::instance();
    s.timeline_column_pct = 65;
    s.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Center;
    s.save_to_disk(dir);
    s.timeline_column_pct = 100;
    s.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Left;
    s.load_from_disk(dir);
    CHECK(s.timeline_column_pct == 65);
    CHECK(s.timeline_column_anchor == tesseract::Settings::TimelineColumnAnchor::Center);
    s.timeline_column_pct = 100;
    s.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Left;
    fs::remove_all(dir);
}

TEST_CASE("Settings timeline column clamps and sanitizes bad values")
{
    reset_settings();
    auto dir = make_tmp_dir("column_bad");
    auto& s = tesseract::Settings::instance();
    const struct { const char* json; int pct; } cases[] = {
        {R"({"timeline_column_pct":0})", 30},
        {R"({"timeline_column_pct":500})", 100},
        {R"({"timeline_column_pct":"x"})", 100},
        {R"({"timeline_column_pct":42.7})", 100},
    };
    for (const auto& c : cases)
    {
        write_file(dir / "app_settings.json", c.json);
        s.timeline_column_pct = 99;
        s.load_from_disk(dir);
        INFO(c.json);
        CHECK(s.timeline_column_pct == c.pct);
    }
    write_file(dir / "app_settings.json", R"({"timeline_column_anchor":"weird"})");
    s.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Center;
    s.load_from_disk(dir);
    CHECK(s.timeline_column_anchor == tesseract::Settings::TimelineColumnAnchor::Left);
    s.timeline_column_pct = 100;
    fs::remove_all(dir);
}

TEST_CASE("Settings timezone flags round-trip and default to false", "[settings]")
{
    reset_settings();
    auto dir = make_tmp_dir("timezone_flags");
    auto& s = tesseract::Settings::instance();

    write_file(dir / "app_settings.json", "{\"theme\":\"dark\"}");
    s.show_sender_timezone_in_timeline = true;
    s.auto_detect_timezone = true;
    s.load_from_disk(dir);
    CHECK_FALSE(s.show_sender_timezone_in_timeline);
    CHECK_FALSE(s.auto_detect_timezone);

    s.show_sender_timezone_in_timeline = true;
    s.auto_detect_timezone = true;
    s.save_to_disk(dir);
    s.show_sender_timezone_in_timeline = false;
    s.auto_detect_timezone = false;
    s.load_from_disk(dir);
    CHECK(s.show_sender_timezone_in_timeline);
    CHECK(s.auto_detect_timezone);

    fs::remove_all(dir);
}
