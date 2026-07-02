#include <catch2/catch_test_macros.hpp>

#include "views/RoomSettingsView.h"

using tesseract::views::compute_room_settings_changes;

TEST_CASE("compute_room_settings_changes: no fields changed",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "My Room", "My Room", "Topic", "Topic", "mxc://a", "mxc://a");
    CHECK_FALSE(changes.name.has_value());
    CHECK_FALSE(changes.topic.has_value());
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: name-only changed",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "Old Name", "New Name", "Topic", "Topic", "mxc://a", "mxc://a");
    REQUIRE(changes.name.has_value());
    CHECK(*changes.name == "New Name");
    CHECK_FALSE(changes.topic.has_value());
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: topic-only changed",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "Name", "Name", "Old topic", "New topic", "mxc://a", "mxc://a");
    CHECK_FALSE(changes.name.has_value());
    REQUIRE(changes.topic.has_value());
    CHECK(*changes.topic == "New topic");
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: avatar-only changed to a new mxc",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "Name", "Name", "Topic", "Topic", "mxc://old", "mxc://new");
    CHECK_FALSE(changes.name.has_value());
    CHECK_FALSE(changes.topic.has_value());
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(*changes.avatar_mxc == "mxc://new");
}

TEST_CASE("compute_room_settings_changes: avatar cleared (non-empty to empty)",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "Name", "Name", "Topic", "Topic", "mxc://old", "");
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(changes.avatar_mxc->empty());
}

TEST_CASE("compute_room_settings_changes: avatar unchanged when both empty",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes("Name", "Name", "Topic",
                                                  "Topic", "", "");
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: all three changed simultaneously",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        "Old Name", "New Name", "Old topic", "New topic", "mxc://old",
        "mxc://new");
    REQUIRE(changes.name.has_value());
    REQUIRE(changes.topic.has_value());
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(*changes.name == "New Name");
    CHECK(*changes.topic == "New topic");
    CHECK(*changes.avatar_mxc == "mxc://new");
}
