#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "views/JoinRoomView.h"

#include <tesseract/types.h>

using tesseract::views::JoinRoomView;

namespace
{
tesseract::RoomSummary make_join_summary(const std::string& rule = "public")
{
    tesseract::RoomSummary s;
    s.room_id            = "!room:s";
    s.name               = "Test Room";
    s.topic              = "A test topic";
    s.num_joined_members = 42;
    s.join_rule          = rule;
    return s;
}
} // namespace

TEST_CASE("JoinRoomView: join button enabled after preview", "[joinroom]")
{
    auto v = tk::create_root_widget<JoinRoomView>(nullptr);
    v->set_preview(make_join_summary());
    CHECK(v->state() == JoinRoomView::State::Preview);
    CHECK(v->join_button_enabled());
}

TEST_CASE("JoinRoomView: join disables the button until the outcome arrives",
          "[joinroom]")
{
    auto v = tk::create_root_widget<JoinRoomView>(nullptr);
    v->set_preview(make_join_summary());

    int joins = 0;
    std::string joined_id;
    v->on_join_requested = [&](const std::string& id) { ++joins; joined_id = id; };

    v->trigger_join_for_test();
    CHECK(joins == 1);
    CHECK(joined_id == "!room:s");
    CHECK(v->state() == JoinRoomView::State::Joining);
    CHECK_FALSE(v->join_button_enabled());

    // A second click while the request is in flight must be a no-op.
    v->trigger_join_for_test();
    CHECK(joins == 1);

    // Failure path: ShellBase calls set_error(), which leaves Joining; the
    // flow is usable again once a fresh preview is shown.
    v->set_error("nope");
    CHECK(v->state() == JoinRoomView::State::Error);
    v->set_preview(make_join_summary());
    CHECK(v->join_button_enabled());
}

TEST_CASE("JoinRoomView: knock room routes to on_knock_requested once",
          "[joinroom]")
{
    auto v = tk::create_root_widget<JoinRoomView>(nullptr);
    v->set_preview(make_join_summary("knock"));

    int knocks = 0;
    v->on_knock_requested = [&](const std::string&, const std::string&) { ++knocks; };

    v->trigger_join_for_test();
    CHECK(knocks == 1);
    CHECK(v->state() == JoinRoomView::State::Joining);
    CHECK_FALSE(v->join_button_enabled());

    v->trigger_join_for_test();
    CHECK(knocks == 1);
}
