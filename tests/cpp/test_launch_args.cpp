#include <catch2/catch_test_macros.hpp>
#include <tesseract/launch_args.h>

TEST_CASE("parse_launch_args: no args returns defaults")
{
    auto args = tesseract::parse_launch_args({});
    CHECK(args.autostart == false);
    CHECK(!args.matrix_uri.has_value());
    CHECK(args.action == tesseract::LaunchAction::None);
    CHECK_FALSE(args.room_id);
}

TEST_CASE("parse_launch_args: --autostart alone")
{
    auto args = tesseract::parse_launch_args({"--autostart"});
    CHECK(args.autostart == true);
    CHECK(!args.matrix_uri.has_value());
}

TEST_CASE("parse_launch_args: matrix URI alone")
{
    auto args = tesseract::parse_launch_args(
        {"https://matrix.to/#/@alice:example.org"});
    CHECK(args.autostart == false);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: --autostart then matrix URI")
{
    auto args = tesseract::parse_launch_args(
        {"--autostart", "https://matrix.to/#/@alice:example.org"});
    CHECK(args.autostart == true);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: matrix URI then --autostart")
{
    auto args = tesseract::parse_launch_args(
        {"https://matrix.to/#/@alice:example.org", "--autostart"});
    CHECK(args.autostart == true);
    REQUIRE(args.matrix_uri.has_value());
    CHECK(*args.matrix_uri == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("parse_launch_args: unrecognised args are ignored")
{
    auto args = tesseract::parse_launch_args({"--bogus-flag", "not-a-uri"});
    CHECK(args.autostart == false);
    CHECK(!args.matrix_uri.has_value());
}

TEST_CASE("parse_launch_args: taskbar actions are parsed")
{
    CHECK(tesseract::parse_launch_args({"--open-quick-switcher"}).action ==
          tesseract::LaunchAction::QuickSwitcher);
    CHECK(tesseract::parse_launch_args({"--open-message-search"}).action ==
          tesseract::LaunchAction::MessageSearch);
    CHECK(tesseract::parse_launch_args({"--open-settings"}).action ==
          tesseract::LaunchAction::Settings);
}

TEST_CASE("parse_launch_args: first taskbar action wins")
{
    auto args = tesseract::parse_launch_args(
        {"--open-settings", "--open-message-search", "--autostart"});
    CHECK(args.action == tesseract::LaunchAction::Settings);
    CHECK(args.autostart);
}

TEST_CASE("parse_launch_args: recent room action retains the room ID")
{
    auto args = tesseract::parse_launch_args(
        {"--open-room=!abcdef:example.org", "--open-settings"});
    CHECK(args.action == tesseract::LaunchAction::Room);
    REQUIRE(args.room_id);
    CHECK(*args.room_id == "!abcdef:example.org");
}

TEST_CASE("parse_launch_args: empty recent room action is ignored")
{
    auto args = tesseract::parse_launch_args({"--open-room="});
    CHECK(args.action == tesseract::LaunchAction::None);
    CHECK_FALSE(args.room_id);
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
TEST_CASE("parse_launch_args: screenshot output is available in CI builds")
{
    auto args = tesseract::parse_launch_args(
        {"--screenshot-dir=/tmp/tesseract-shots"});
    REQUIRE(args.screenshot_dir);
    CHECK(*args.screenshot_dir == "/tmp/tesseract-shots");
    CHECK_FALSE(args.autostart);
    CHECK_FALSE(args.matrix_uri);
}
#endif
