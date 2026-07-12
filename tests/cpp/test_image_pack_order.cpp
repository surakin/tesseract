#include <catch2/catch_test_macros.hpp>

#include "views/image_pack_order.h"

using tesseract::views::is_pack_picker_visible;
using tesseract::views::order_picker_packs;

namespace
{

tesseract::ImagePack make_personal()
{
    tesseract::ImagePack p;
    p.id = "user";
    p.display_name = "Saved Stickers";
    p.source_kind = tesseract::PackSourceKind::User;
    return p;
}

tesseract::ImagePack make_room_pack(std::string room_id, std::string display_name,
                                    bool is_subscribed = false)
{
    tesseract::ImagePack p;
    p.id = "room:" + std::to_string(room_id.size()) + ":" + room_id + "/";
    p.display_name = std::move(display_name);
    p.source_kind = tesseract::PackSourceKind::Room;
    p.source_room = std::move(room_id);
    p.is_subscribed = is_subscribed;
    return p;
}

const std::vector<std::string> kNoSpaces;

} // namespace

TEST_CASE("is_pack_picker_visible: personal pack is always visible",
         "[image_pack][order]")
{
    CHECK(is_pack_picker_visible(make_personal(), "", kNoSpaces));
    CHECK(is_pack_picker_visible(make_personal(), "!current:h", kNoSpaces));
}

TEST_CASE("is_pack_picker_visible: current room's pack is visible even "
         "unsubscribed",
         "[image_pack][order]")
{
    auto pack = make_room_pack("!current:h", "Current", /*is_subscribed=*/false);
    CHECK(is_pack_picker_visible(pack, "!current:h", kNoSpaces));
}

TEST_CASE("is_pack_picker_visible: subscribed room pack is visible "
         "regardless of current room",
         "[image_pack][order]")
{
    auto pack = make_room_pack("!other:h", "Other", /*is_subscribed=*/true);
    CHECK(is_pack_picker_visible(pack, "!current:h", kNoSpaces));
    CHECK(is_pack_picker_visible(pack, "", kNoSpaces));
}

TEST_CASE("is_pack_picker_visible: unsubscribed, non-current room pack is "
         "not visible",
         "[image_pack][order]")
{
    auto pack = make_room_pack("!other:h", "Other", /*is_subscribed=*/false);
    CHECK_FALSE(is_pack_picker_visible(pack, "!current:h", kNoSpaces));
    CHECK_FALSE(is_pack_picker_visible(pack, "", kNoSpaces));
}

TEST_CASE("is_pack_picker_visible: pack sourced from a parent space is "
         "visible even unsubscribed",
         "[image_pack][order]")
{
    auto pack = make_room_pack("!space:h", "Space Pack", /*is_subscribed=*/false);
    CHECK(is_pack_picker_visible(pack, "!current:h", {"!space:h"}));
}

TEST_CASE("is_pack_picker_visible: pack from an unrelated room is still "
         "hidden even when some parent spaces are known",
         "[image_pack][order]")
{
    auto pack = make_room_pack("!other:h", "Other", /*is_subscribed=*/false);
    CHECK_FALSE(is_pack_picker_visible(pack, "!current:h", {"!space:h"}));
}

TEST_CASE("order_picker_packs: personal, then current room, then subscribed",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_room_pack("!sub1:h", "Sub 1", /*is_subscribed=*/true),
            make_room_pack("!current:h", "Current"),
            make_personal(),
            make_room_pack("!sub2:h", "Sub 2", /*is_subscribed=*/true),
        },
        "!current:h", kNoSpaces);

    REQUIRE(packs.size() == 4);
    CHECK(packs[0].id == "user");
    CHECK(packs[1].source_room == "!current:h");
    CHECK(packs[2].source_room == "!sub1:h");
    CHECK(packs[3].source_room == "!sub2:h");
}

TEST_CASE("order_picker_packs: unsubscribed, non-current room packs are "
         "dropped entirely",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_personal(),
            make_room_pack("!current:h", "Current"),
            make_room_pack("!visited:h", "Visited but not subscribed",
                          /*is_subscribed=*/false),
        },
        "!current:h", kNoSpaces);

    REQUIRE(packs.size() == 2);
    CHECK(packs[0].id == "user");
    CHECK(packs[1].source_room == "!current:h");
}

TEST_CASE("order_picker_packs: no personal pack -> current room leads",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_room_pack("!sub:h", "Sub", /*is_subscribed=*/true),
            make_room_pack("!current:h", "Current"),
        },
        "!current:h", kNoSpaces);

    REQUIRE(packs.size() == 2);
    CHECK(packs[0].source_room == "!current:h");
    CHECK(packs[1].source_room == "!sub:h");
}

TEST_CASE("order_picker_packs: no pack matches current_room_id -> all "
         "subscribed room packs land in the trailing bucket, order "
         "preserved",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_personal(),
            make_room_pack("!a:h", "A", /*is_subscribed=*/true),
            make_room_pack("!b:h", "B", /*is_subscribed=*/true),
        },
        "!not-in-list:h", kNoSpaces);

    REQUIRE(packs.size() == 3);
    CHECK(packs[0].id == "user");
    CHECK(packs[1].source_room == "!a:h");
    CHECK(packs[2].source_room == "!b:h");
}

TEST_CASE("order_picker_packs: empty current_room_id -> no room pack "
         "treated as current, but subscribed packs still show",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_room_pack("!a:h", "A", /*is_subscribed=*/true),
            make_personal(),
        },
        "", kNoSpaces);

    REQUIRE(packs.size() == 2);
    CHECK(packs[0].id == "user");
    CHECK(packs[1].source_room == "!a:h");
}

TEST_CASE("order_picker_packs: empty input yields empty output",
         "[image_pack][order]")
{
    auto packs = order_picker_packs({}, "!current:h", kNoSpaces);
    CHECK(packs.empty());
}

TEST_CASE("order_picker_packs: personal, then current room, then parent "
         "space, then subscribed",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_room_pack("!sub:h", "Sub", /*is_subscribed=*/true),
            make_room_pack("!space:h", "Space Pack"),
            make_room_pack("!current:h", "Current"),
            make_personal(),
        },
        "!current:h", {"!space:h"});

    REQUIRE(packs.size() == 4);
    CHECK(packs[0].id == "user");
    CHECK(packs[1].source_room == "!current:h");
    CHECK(packs[2].source_room == "!space:h");
    CHECK(packs[3].source_room == "!sub:h");
}

TEST_CASE("order_picker_packs: pack from a room that is neither current, "
         "a parent space, nor subscribed is dropped even with parent "
         "spaces present",
         "[image_pack][order]")
{
    auto packs = order_picker_packs(
        {
            make_room_pack("!space:h", "Space Pack"),
            make_room_pack("!unrelated:h", "Unrelated"),
        },
        "!current:h", {"!space:h"});

    REQUIRE(packs.size() == 1);
    CHECK(packs[0].source_room == "!space:h");
}
