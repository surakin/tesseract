#include <catch2/catch_test_macros.hpp>
#include "views/MessageListView.h"
#include "views/map_tiles.h"
#include <tesseract/types.h>

using tesseract::views::LiveLocationPhase;
using tesseract::views::live_location_phase;
using tesseract::views::live_location_status_text;
using tesseract::views::live_location_has_map;
using tesseract::views::next_map_viewport;
using tesseract::views::make_row_data;
using tesseract::views::MapViewport;
using tesseract::views::MessageRowData;

static tesseract::LocationEvent make_loc_event(double lat, double lon,
                                               const std::string& desc = "")
{
    tesseract::LocationEvent ev;
    ev.event_id = "!loc:example.org";
    ev.sender = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body = "Alice shared her location";
    ev.timestamp = 1000;
    ev.type = tesseract::EventType::Location;
    ev.lat = lat;
    ev.lon = lon;
    ev.description = desc;
    return ev;
}

TEST_CASE("make_row_data maps LocationEvent to Kind::Location", "[location]")
{
    auto ev = make_loc_event(51.5074, -0.1278);
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.kind == MessageRowData::Kind::Location);
    CHECK(row.location_lat == 51.5074);
    CHECK(row.location_lon == -0.1278);
    CHECK(row.location_description.empty());
}

TEST_CASE("make_row_data: description is carried through", "[location]")
{
    auto ev = make_loc_event(51.5074, -0.1278, "Parliament Square");
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.location_description == "Parliament Square");
}

TEST_CASE("make_row_data: map_viewport initialised to event coords at zoom 15",
          "[location]")
{
    auto ev = make_loc_event(48.8566, 2.3522);
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.map_viewport.lat == 48.8566);
    CHECK(row.map_viewport.lon == 2.3522);
    CHECK(row.map_viewport.zoom == 15);
}

static MessageRowData live_row(bool live, bool awaiting, std::uint64_t expires)
{
    auto ev = make_loc_event(51.5, -0.1);
    ev.live_share = true;
    ev.live = live;
    ev.awaiting_fix = awaiting;
    ev.live_expires_ms = expires;
    return make_row_data(ev, "@other:example.org");
}

TEST_CASE("live location phase", "[location]")
{
    CHECK(live_location_phase(live_row(true, false, 2000), 1000) == LiveLocationPhase::Live);
    CHECK(live_location_phase(live_row(true, false, 2000), 2000) == LiveLocationPhase::Ended);
    CHECK(live_location_phase(live_row(false, false, 2000), 1000) == LiveLocationPhase::Ended);
    CHECK(live_location_phase(live_row(true, true, 2000), 1000) == LiveLocationPhase::Waiting);
    CHECK(live_location_phase(live_row(true, true, 2000), 3000) == LiveLocationPhase::Ended);
    auto stat = make_row_data(make_loc_event(1, 2), "@o:x");
    CHECK(live_location_phase(stat, 1) == LiveLocationPhase::NotLive);
}

TEST_CASE("live location status text", "[location]")
{
    CHECK(live_location_status_text(make_row_data(make_loc_event(1, 2), "@o:x"), 1).empty());
    CHECK_FALSE(live_location_status_text(live_row(true, false, 2000), 1000).empty());
    CHECK(live_location_status_text(live_row(true, true, 2000), 1000) != "");
    CHECK(live_location_status_text(live_row(true, true, 2000), 1000) !=
          live_location_status_text(live_row(true, true, 2000), 3000));
    CHECK(live_location_status_text(live_row(false, false, 2000), 1000) ==
          live_location_status_text(live_row(true, false, 2000), 3000));
}

TEST_CASE("next_map_viewport", "[location]")
{
    auto oldr = live_row(true, false, 9999);
    oldr.map_viewport = {10, 10, 12};
    auto newr = live_row(true, false, 9999);
    newr.location_lat = 20;
    newr.location_lon = 20;
    newr.map_viewport = {20, 20, 15};
    CHECK(next_map_viewport(oldr, newr).lat == 20);
    oldr.map_viewport_touched = true;
    auto vp = next_map_viewport(oldr, newr);
    CHECK(vp.lat == 10);
    CHECK(vp.zoom == 12);
    newr.event_id = "other";
    CHECK(next_map_viewport(oldr, newr).lat == 20);
}

TEST_CASE("live location has_map: no fix means no map in any phase", "[location]")
{
    auto ended = live_row(false, true, 2000);
    CHECK_FALSE(live_location_has_map(ended));
    CHECK(live_location_status_text(ended, 1000) == "Live location ended");
    CHECK_FALSE(live_location_has_map(live_row(true, true, 2000)));
    CHECK(live_location_has_map(live_row(true, false, 2000)));
    CHECK(live_location_has_map(live_row(false, false, 2000)));
    CHECK(live_location_has_map(make_row_data(make_loc_event(1, 2), "@o:x")));
}
