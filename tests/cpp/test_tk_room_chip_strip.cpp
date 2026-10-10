#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/LocationMapPanner.h"
#include "views/MessageListView.h"
#include "views/room_chip_strip.h"

#include <tesseract/types.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace tesseract::views;

namespace
{

tesseract::RoomInfo chip_room(const std::string& id, const std::string& name,
                              const std::string& avatar = "")
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = name;
    r.avatar_url = avatar;
    return r;
}

struct ChipStage : vt::Stage
{
    ChipStage() : vt::Stage(600, 300) {}
};

} // namespace

TEST_CASE("paint_room_chips lays out as many chips as fit, with hit rects",
          "[tk][view][room_chips]")
{
    ChipStage s;
    auto pc = s.pc();
    std::vector<tesseract::RoomInfo> rooms;
    for (int i = 0; i < 20; ++i)
        rooms.push_back(chip_room("!r" + std::to_string(i), "Room " + std::to_string(i)));
    RoomChipStripStyle style;
    std::vector<std::pair<tk::Rect, std::string>> chips;
    paint_room_chips(pc, {0, 0, 300, 100}, rooms, nullptr, nullptr, style, chips);
    // avail = 300 - 24 = 276; (276 + 8) / (64 + 8) = 3 chips.
    REQUIRE(chips.size() == 3);
    CHECK(chips[0].second == "!r0");
    CHECK(chips[1].first.x == chips[0].first.x + 64 + 8);
    CHECK(chips[0].first.y == style.top_pad);
}

TEST_CASE("paint_room_chips caption shifts the chips down; zero width yields "
          "none",
          "[tk][view][room_chips]")
{
    ChipStage s;
    auto pc = s.pc();
    std::vector<tesseract::RoomInfo> rooms = {chip_room("!a", "A")};
    RoomChipStripStyle style;
    std::vector<std::pair<tk::Rect, std::string>> plain, captioned, narrow;
    paint_room_chips(pc, {0, 0, 300, 100}, rooms, nullptr, nullptr, style, plain);
    style.caption = "Recent";
    paint_room_chips(pc, {0, 0, 300, 100}, rooms, nullptr, nullptr, style, captioned);
    REQUIRE(plain.size() == 1);
    REQUIRE(captioned.size() == 1);
    CHECK(captioned[0].first.y > plain[0].first.y);
    paint_room_chips(pc, {0, 0, 20, 100}, rooms, nullptr, nullptr, style, narrow);
    CHECK(narrow.empty());
}

TEST_CASE("paint_room_chips highlights one chip and requests missing avatars",
          "[tk][view][room_chips]")
{
    ChipStage s;
    auto pc = s.pc();
    std::vector<tesseract::RoomInfo> rooms = {chip_room("!a", "", "mxc://x/a"),
                                              chip_room("!b", "B")};
    RoomChipStripStyle style;
    style.highlight_index = 0;
    style.highlight_fill = tk::Color{10, 20, 30, 255};
    style.highlight_border = true;
    style.highlight_border_color = tk::Color{255, 0, 0, 255};
    std::vector<std::string> asked, provided;
    std::vector<std::pair<tk::Rect, std::string>> chips;
    paint_room_chips(
        pc, {0, 0, 300, 100}, rooms,
        [&](const std::string& mxc) -> const tk::Image* { provided.push_back(mxc); return nullptr; },
        [&](const tesseract::RoomInfo& r) { asked.push_back(r.id); }, style, chips);
    CHECK(chips.size() == 2);
    CHECK(provided == std::vector<std::string>{"mxc://x/a"});
    CHECK(asked == std::vector<std::string>{"!a"});
}

// ── LocationMapPanner ─────────────────────────────────────────────────────

TEST_CASE("LocationMapPanner pan converts drag to a viewport shift and "
          "classifies clicks",
          "[tk][view][map_panner]")
{
    LocationMapPanner p;
    MapViewport vp;
    vp.lat = 10.0;
    vp.lon = 20.0;
    vp.zoom = 10;
    CHECK_FALSE(p.panning());
    p.begin_pan(3, {100, 100}, vp);
    CHECK(p.panning());
    CHECK(p.active_row() == 3);
    p.drag_pan({160, 100}, vp); // dragging right moves the map east-to-west
    CHECK(vp.lon < 20.0);
    CHECK(vp.lat == Catch::Approx(10.0).margin(0.01));
    CHECK_FALSE(p.end_pan({160, 100})); // moved >8px: not a click
    CHECK_FALSE(p.panning());

    p.begin_pan(1, {50, 50}, vp);
    CHECK(p.end_pan({52, 51})); // tiny move: a click
}

TEST_CASE("LocationMapPanner wheel accumulates and clamps zoom",
          "[tk][view][map_panner]")
{
    LocationMapPanner p;
    MapViewport vp;
    vp.zoom = 18;
    CHECK_FALSE(p.zoom(30, vp));
    CHECK(p.zoom(30, vp));
    CHECK(vp.zoom == 19);
    CHECK(p.zoom(100, vp));
    CHECK(vp.zoom == 19); // capped
    vp.zoom = 2;
    CHECK(p.zoom(-100, vp));
    CHECK(vp.zoom == 1);
    CHECK(p.zoom(-100, vp));
    CHECK(vp.zoom == 1); // floor
}

TEST_CASE("LocationMapPanner tooltip show/hide is edge-triggered and geometry "
          "is recorded",
          "[tk][view][map_panner]")
{
    LocationMapPanner p;
    int shows = 0, hides = 0;
    std::string text;
    p.set_tooltip_callbacks([&](std::string t, tk::Rect) { ++shows; text = t; },
                            [&] { ++hides; });
    p.hide_tooltip(); // not showing: no-op
    CHECK(hides == 0);
    p.show_tooltip("here", {0, 0, 1, 1});
    p.show_tooltip("again", {0, 0, 1, 1});
    CHECK(shows == 1);
    CHECK(text == "here");
    CHECK(p.tooltip_showing());
    p.hide_tooltip();
    p.hide_tooltip();
    CHECK(hides == 1);

    CHECK(p.geom_at("$e") == nullptr);
    p.record_geom("$e", {1, 2, 3, 4});
    REQUIRE(p.geom_at("$e") != nullptr);
    CHECK(p.geom_at("$e")->w == 3);
    p.clear_geometry();
    CHECK(p.geom_at("$e") == nullptr);
}

TEST_CASE("LocationMapPanner paint requests tiles, falls back to parent/child "
          "tiles and draws a pin",
          "[tk][view][map_panner]")
{
    ChipStage s;
    auto pc = s.pc();
    MessageRowData m;
    m.event_id = "$loc";
    m.location_lat = 48.85;
    m.location_lon = 2.35;
    m.map_viewport.lat = 48.85;
    m.map_viewport.lon = 2.35;
    m.map_viewport.zoom = 12;

    LocationMapPanner p;
    std::vector<std::string> requested;
    // No provider: tiles requested only when a request callback is set.
    p.set_tile_request([&](int z, int x, int y)
                       { requested.push_back(std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y)); });
    p.paint(m, pc, {10, 10, 300, 200});
    CHECK(requested.size() >= 1);
    REQUIRE(p.geom_at("$loc") != nullptr);

    // Provider returns nothing: "Loading map" placeholder path + fallbacks.
    int provider_calls = 0;
    p.set_tile_image_provider([&](const std::string&) -> const tk::Image* { ++provider_calls; return nullptr; });
    requested.clear();
    p.paint(m, pc, {10, 10, 300, 200});
    CHECK(provider_calls > 0);
    CHECK_FALSE(requested.empty());

    // Provider returns an image for everything: tiles drawn directly.
    std::vector<std::uint8_t> px(4 * 4 * 4, 200);
    auto img = s.surface->factory().create_image_rgba(px.data(), 4, 4);
    REQUIRE(img != nullptr);
    p.set_tile_image_provider([&](const std::string&) -> const tk::Image* { return img.get(); });
    requested.clear();
    p.paint(m, pc, {10, 10, 300, 200});
    CHECK(requested.empty());

    // Zoom 0 / 19 edges exercise the no-parent / no-child branches.
    m.map_viewport.zoom = 1;
    p.set_tile_image_provider([&](const std::string&) -> const tk::Image* { return nullptr; });
    p.paint(m, pc, {10, 10, 300, 200});
    m.map_viewport.zoom = 19;
    p.paint(m, pc, {10, 10, 300, 200});
}
