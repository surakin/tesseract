#include <catch2/catch_test_macros.hpp>
#include "views/RoomPreviewView.h"
#include "tesseract/types.h"

using namespace tesseract::views;

namespace
{
tesseract::RoomSummary make_summary(const std::string& id = "!room:s",
                                    const std::string& name = "Test Room")
{
    tesseract::RoomSummary s;
    s.room_id            = id;
    s.name               = name;
    s.topic              = "A test topic";
    s.num_joined_members = 42;
    s.join_rule          = "public";
    return s;
}
} // namespace

TEST_CASE("RoomPreviewView: starts invisible", "[roompreview]")
{
    RoomPreviewView v;
    CHECK(!v.visible());
}

TEST_CASE("RoomPreviewView: visible after set_summary", "[roompreview]")
{
    RoomPreviewView v;
    v.set_summary(make_summary());
    CHECK(v.visible());
}

TEST_CASE("RoomPreviewView: on_join fires with room_id", "[roompreview]")
{
    RoomPreviewView v;
    v.set_summary(make_summary("!abc:s", "Alpha"));

    std::string fired_id;
    v.on_join = [&](const std::string& id) { fired_id = id; };
    v.trigger_join_for_test();

    CHECK(fired_id == "!abc:s");
}

TEST_CASE("RoomPreviewView: clear hides widget", "[roompreview]")
{
    RoomPreviewView v;
    v.set_summary(make_summary());
    v.clear();
    CHECK(!v.visible());
}

TEST_CASE("RoomPreviewView: set_state Joining disables join button",
          "[roompreview]")
{
    RoomPreviewView v;
    v.set_summary(make_summary());
    CHECK(v.join_button_enabled());
    v.set_state(RoomPreviewView::State::Joining);
    CHECK(!v.join_button_enabled());
    v.set_state(RoomPreviewView::State::Idle);
    CHECK(v.join_button_enabled());
}
