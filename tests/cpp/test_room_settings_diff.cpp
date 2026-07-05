#include <catch2/catch_test_macros.hpp>

#include "views/RoomSettingsView.h"

using tesseract::views::compute_room_settings_changes;
using tesseract::views::RoomSettingsFieldValues;
using Mode = tesseract::MediaPreviewConfig::Mode;

namespace
{
RoomSettingsFieldValues make_values(
    std::string name = "My Room", std::string topic = "Topic",
    std::string avatar_mxc = "mxc://a", bool is_encrypted = false,
    std::string join_rule = "invite", bool guest_access = false,
    std::string history_visibility = "shared",
    bool has_media_override = false, Mode media_override_mode = Mode::On,
    tesseract::RoomPermissions permissions = {})
{
    return RoomSettingsFieldValues{
        std::move(name), std::move(topic), std::move(avatar_mxc),
        is_encrypted, std::move(join_rule), guest_access,
        std::move(history_visibility), has_media_override, media_override_mode,
        permissions};
}
} // namespace

TEST_CASE("compute_room_settings_changes: no fields changed",
          "[room_settings][diff]")
{
    auto values = make_values();
    auto changes = compute_room_settings_changes(values, values);
    CHECK_FALSE(changes.name.has_value());
    CHECK_FALSE(changes.topic.has_value());
    CHECK_FALSE(changes.avatar_mxc.has_value());
    CHECK_FALSE(changes.is_encrypted.has_value());
    CHECK_FALSE(changes.join_rule.has_value());
    CHECK_FALSE(changes.guest_access.has_value());
    CHECK_FALSE(changes.history_visibility.has_value());
    CHECK_FALSE(changes.media_override.has_value());
    CHECK_FALSE(changes.permissions.has_value());
}

TEST_CASE("compute_room_settings_changes: name-only changed",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Old Name"), make_values("New Name"));
    REQUIRE(changes.name.has_value());
    CHECK(*changes.name == "New Name");
    CHECK_FALSE(changes.topic.has_value());
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: topic-only changed",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Name", "Old topic"), make_values("Name", "New topic"));
    CHECK_FALSE(changes.name.has_value());
    REQUIRE(changes.topic.has_value());
    CHECK(*changes.topic == "New topic");
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: avatar-only changed to a new mxc",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Name", "Topic", "mxc://old"),
        make_values("Name", "Topic", "mxc://new"));
    CHECK_FALSE(changes.name.has_value());
    CHECK_FALSE(changes.topic.has_value());
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(*changes.avatar_mxc == "mxc://new");
}

TEST_CASE("compute_room_settings_changes: avatar cleared (non-empty to empty)",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Name", "Topic", "mxc://old"),
        make_values("Name", "Topic", ""));
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(changes.avatar_mxc->empty());
}

TEST_CASE("compute_room_settings_changes: avatar unchanged when both empty",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Name", "Topic", ""), make_values("Name", "Topic", ""));
    CHECK_FALSE(changes.avatar_mxc.has_value());
}

TEST_CASE("compute_room_settings_changes: all three name/topic/avatar changed simultaneously",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("Old Name", "Old topic", "mxc://old"),
        make_values("New Name", "New topic", "mxc://new"));
    REQUIRE(changes.name.has_value());
    REQUIRE(changes.topic.has_value());
    REQUIRE(changes.avatar_mxc.has_value());
    CHECK(*changes.name == "New Name");
    CHECK(*changes.topic == "New topic");
    CHECK(*changes.avatar_mxc == "mxc://new");
}

// ── encryption: asymmetric diff rule ────────────────────────────────────────

TEST_CASE("compute_room_settings_changes: encryption turned on (false->true) emits a change",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false), make_values("N", "T", "A", true));
    REQUIRE(changes.is_encrypted.has_value());
    CHECK(*changes.is_encrypted == true);
}

TEST_CASE("compute_room_settings_changes: encryption already on (true->true) emits nothing",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", true), make_values("N", "T", "A", true));
    CHECK_FALSE(changes.is_encrypted.has_value());
}

TEST_CASE("compute_room_settings_changes: encryption off in both (false->false) emits nothing",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false), make_values("N", "T", "A", false));
    CHECK_FALSE(changes.is_encrypted.has_value());
}

TEST_CASE("compute_room_settings_changes: defensive impossible state "
          "(true->false) never emits a disable and does not crash",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", true), make_values("N", "T", "A", false));
    CHECK_FALSE(changes.is_encrypted.has_value());
}

// ── join_rule / guest_access / history_visibility: independent diffs ───────

TEST_CASE("compute_room_settings_changes: join_rule changed independently",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite"),
        make_values("N", "T", "A", false, "public"));
    REQUIRE(changes.join_rule.has_value());
    CHECK(*changes.join_rule == "public");
    CHECK_FALSE(changes.guest_access.has_value());
    CHECK_FALSE(changes.history_visibility.has_value());
}

TEST_CASE("compute_room_settings_changes: guest_access changed independently",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false),
        make_values("N", "T", "A", false, "invite", true));
    REQUIRE(changes.guest_access.has_value());
    CHECK(*changes.guest_access == true);
    CHECK_FALSE(changes.join_rule.has_value());
    CHECK_FALSE(changes.history_visibility.has_value());
}

TEST_CASE("compute_room_settings_changes: history_visibility changed independently",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared"),
        make_values("N", "T", "A", false, "invite", false, "invited"));
    REQUIRE(changes.history_visibility.has_value());
    CHECK(*changes.history_visibility == "invited");
    CHECK_FALSE(changes.join_rule.has_value());
    CHECK_FALSE(changes.guest_access.has_value());
}

// ── media_override: has_override/mode pair diff ─────────────────────────────

TEST_CASE("compute_room_settings_changes: media_override false->true with a mode emits a change",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", false, Mode::On),
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::Off));
    REQUIRE(changes.media_override.has_value());
    CHECK(changes.media_override->has_override == true);
    CHECK(changes.media_override->mode == Mode::Off);
}

TEST_CASE("compute_room_settings_changes: media_override true->true with same mode emits nothing",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::Private),
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::Private));
    CHECK_FALSE(changes.media_override.has_value());
}

TEST_CASE("compute_room_settings_changes: media_override true->true with a different mode emits a change",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::On),
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::Off));
    REQUIRE(changes.media_override.has_value());
    CHECK(changes.media_override->has_override == true);
    CHECK(changes.media_override->mode == Mode::Off);
}

TEST_CASE("compute_room_settings_changes: media_override true->false (clearing) emits a change ignoring mode",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", true, Mode::On),
        make_values("N", "T", "A", false, "invite", false, "shared", false, Mode::Private));
    REQUIRE(changes.media_override.has_value());
    CHECK(changes.media_override->has_override == false);
}

TEST_CASE("compute_room_settings_changes: media_override false->false regardless "
          "of mode-field noise emits nothing",
          "[room_settings][diff]")
{
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", false, Mode::On),
        make_values("N", "T", "A", false, "invite", false, "shared", false, Mode::Off));
    CHECK_FALSE(changes.media_override.has_value());
}

// ── permissions: full-struct diff ───────────────────────────────────────────

TEST_CASE("compute_room_settings_changes: permissions unchanged emits nothing",
          "[room_settings][diff]")
{
    tesseract::RoomPermissions p;
    auto changes = compute_room_settings_changes(make_values("N", "T", "A", false,
                                                             "invite", false, "shared",
                                                             false, Mode::On, p),
                                                  make_values("N", "T", "A", false,
                                                             "invite", false, "shared",
                                                             false, Mode::On, p));
    CHECK_FALSE(changes.permissions.has_value());
}

TEST_CASE("compute_room_settings_changes: a single permissions field changing "
          "emits the full updated struct",
          "[room_settings][diff]")
{
    tesseract::RoomPermissions original;
    tesseract::RoomPermissions staged;
    staged.kick_users = 100;
    auto changes = compute_room_settings_changes(
        make_values("N", "T", "A", false, "invite", false, "shared", false,
                    Mode::On, original),
        make_values("N", "T", "A", false, "invite", false, "shared", false,
                    Mode::On, staged));
    REQUIRE(changes.permissions.has_value());
    CHECK(changes.permissions->kick_users == 100);
    CHECK(changes.permissions->ban_users == original.ban_users);
}

TEST_CASE("compute_room_settings_changes: full no-op case across all 9 fields",
          "[room_settings][diff]")
{
    tesseract::RoomPermissions permissions;
    permissions.kick_users = 100;
    auto values = make_values("Room", "Topic", "mxc://a", true, "restricted",
                              true, "invited", true, Mode::Private, permissions);
    auto changes = compute_room_settings_changes(values, values);
    CHECK_FALSE(changes.name.has_value());
    CHECK_FALSE(changes.topic.has_value());
    CHECK_FALSE(changes.avatar_mxc.has_value());
    CHECK_FALSE(changes.is_encrypted.has_value());
    CHECK_FALSE(changes.join_rule.has_value());
    CHECK_FALSE(changes.guest_access.has_value());
    CHECK_FALSE(changes.history_visibility.has_value());
    CHECK_FALSE(changes.media_override.has_value());
    CHECK_FALSE(changes.permissions.has_value());
}
