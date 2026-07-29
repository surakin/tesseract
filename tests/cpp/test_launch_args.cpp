#include <catch2/catch_test_macros.hpp>
#include <tesseract/launch_args.h>

TEST_CASE("parse_launch_args: no args returns defaults")
{
    auto args = tesseract::parse_launch_args({});
    CHECK(args.autostart == false);
    CHECK(!args.matrix_uri.has_value());
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
