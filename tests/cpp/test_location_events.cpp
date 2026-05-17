#include <catch2/catch_test_macros.hpp>
#include "views/MessageListView.h"
#include "views/map_tiles.h"
#include <tesseract/types.h>

using tesseract::views::MessageRowData;
using tesseract::views::make_row_data;
using tesseract::views::MapViewport;

static tesseract::LocationEvent make_loc_event(double lat, double lon,
                                                const std::string& desc = "") {
    tesseract::LocationEvent ev;
    ev.event_id    = "!loc:example.org";
    ev.sender      = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body        = "Alice shared her location";
    ev.timestamp   = 1000;
    ev.type        = tesseract::EventType::Location;
    ev.lat         = lat;
    ev.lon         = lon;
    ev.description = desc;
    return ev;
}

TEST_CASE("make_row_data maps LocationEvent to Kind::Location", "[location]") {
    auto ev  = make_loc_event(51.5074, -0.1278);
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.kind == MessageRowData::Kind::Location);
    CHECK(row.location_lat == 51.5074);
    CHECK(row.location_lon == -0.1278);
    CHECK(row.location_description.empty());
}

TEST_CASE("make_row_data: description is carried through", "[location]") {
    auto ev  = make_loc_event(51.5074, -0.1278, "Parliament Square");
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.location_description == "Parliament Square");
}

TEST_CASE("make_row_data: map_viewport initialised to event coords at zoom 15",
          "[location]") {
    auto ev  = make_loc_event(48.8566, 2.3522);
    auto row = make_row_data(ev, "@other:example.org");
    CHECK(row.map_viewport.lat  == 48.8566);
    CHECK(row.map_viewport.lon  == 2.3522);
    CHECK(row.map_viewport.zoom == 15);
}
