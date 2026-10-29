#include <catch2/catch_test_macros.hpp>

#include <tesseract/provisioning.h>

using namespace tesseract;

TEST_CASE("normalize_homeserver trims whitespace and trailing slashes", "[provisioning]")
{
    CHECK(normalize_homeserver("example.org") == "example.org");
    CHECK(normalize_homeserver("  example.org \r\n") == "example.org");
    CHECK(normalize_homeserver("https://example.org/") == "https://example.org");
    CHECK(normalize_homeserver("example.org:8448//") == "example.org:8448");
}

TEST_CASE("normalize_homeserver rejects empty input", "[provisioning]")
{
    CHECK_FALSE(normalize_homeserver("").has_value());
    CHECK_FALSE(normalize_homeserver("  \t ").has_value());
    CHECK_FALSE(normalize_homeserver("/").has_value());
}

TEST_CASE("parse_provisioning_json reads default_homeserver", "[provisioning]")
{
    CHECK(parse_provisioning_json(R"({"default_homeserver": " example.org/ "})") == "example.org");
    CHECK_FALSE(parse_provisioning_json("{}").has_value());
    CHECK_FALSE(parse_provisioning_json(R"({"default_homeserver": 5})").has_value());
    CHECK_FALSE(parse_provisioning_json(R"({"default_homeserver": ""})").has_value());
    CHECK_FALSE(parse_provisioning_json("not json").has_value());
    CHECK_FALSE(parse_provisioning_json("[1,2]").has_value());
}

TEST_CASE("normalize_username trims whitespace and a leading @", "[provisioning]")
{
    CHECK(normalize_username("alice") == "alice");
    CHECK(normalize_username("  @alice:example.org \n") == "alice:example.org");
    CHECK_FALSE(normalize_username("").has_value());
    CHECK_FALSE(normalize_username(" @ ").has_value());
}

TEST_CASE("homeserver_server_name drops scheme and path", "[provisioning]")
{
    CHECK(homeserver_server_name("example.org") == "example.org");
    CHECK(homeserver_server_name("https://matrix.example.org:8448/x") == "matrix.example.org:8448");
}

TEST_CASE("make_user_id completes a localpart with the homeserver", "[provisioning]")
{
    CHECK(make_user_id("alice", "https://example.org") == "@alice:example.org");
    CHECK(make_user_id("@alice:example.org", "other.org") == "@alice:example.org");
    CHECK(make_user_id("alice:example.org", "") == "@alice:example.org");
    CHECK_FALSE(make_user_id("alice", "").has_value());
    CHECK_FALSE(make_user_id("", "example.org").has_value());
}

TEST_CASE("parse_provisioning_values reads both keys independently", "[provisioning]")
{
    auto both = parse_provisioning_values(
        R"({"default_homeserver": "example.org", "default_username": " @alice "})");
    CHECK(both.homeserver == "example.org");
    CHECK(both.username == "alice");

    auto user_only = parse_provisioning_values(R"({"default_username": "bob", "default_homeserver": 3})");
    CHECK_FALSE(user_only.homeserver.has_value());
    CHECK(user_only.username == "bob");

    auto bad = parse_provisioning_values("not json");
    CHECK_FALSE(bad.homeserver.has_value());
    CHECK_FALSE(bad.username.has_value());
}

TEST_CASE("merge_provisioned prefers per-field values from the first argument", "[provisioning]")
{
    ProvisionedValues user{std::nullopt, "alice"};
    ProvisionedValues platform{"example.org", "root"};
    auto merged = merge_provisioned(user, platform);
    CHECK(merged.homeserver == "example.org");
    CHECK(merged.username == "alice");
}
