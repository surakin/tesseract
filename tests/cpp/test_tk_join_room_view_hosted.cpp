#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/JoinRoomView.h"

#include <tesseract/types.h>

#include <optional>
#include <string>
#include <vector>

using tesseract::views::JoinRoomView;

namespace
{

tesseract::RoomSummary jrh_summary(const std::string& rule = "public",
                                   const std::string& topic = "")
{
    tesseract::RoomSummary s;
    s.room_id = "!room:s";
    s.name = "Test Room";
    s.topic = topic;
    s.num_joined_members = 3;
    s.join_rule = rule;
    s.avatar_url = "mxc://s/avatar";
    return s;
}

struct JrhStage : vt::Stage
{
    std::unique_ptr<JoinRoomView> view = tk::create_root_widget<JoinRoomView>(&host);
    JrhStage()
    {
        view->open();
        run();
    }
    void run()
    {
        relayout_paint(*view, {0, 0, JoinRoomView::kPreferredW, JoinRoomView::kPreferredH});
        host.set_root(view.get());
    }
    StubTextField& alias() { return *host.fields_created.at(0); }
    StubTextField& reason() { return *host.fields_created.at(1); }
};

} // namespace

TEST_CASE("JoinRoomView hosted: typing an alias and submitting requests a "
          "lookup; the Look up button does the same",
          "[tk][view][join_room]")
{
    JrhStage s;
    CHECK(s.view->is_open());
    std::vector<std::string> looked;
    s.view->on_lookup_requested = [&](const std::string& a) { looked.push_back(a); };

    s.alias().on_submit(); // empty alias: ignored
    CHECK(looked.empty());
    s.alias().on_changed("#room:s.org");
    CHECK(s.view->alias_text() == "#room:s.org");
    s.alias().on_submit();
    REQUIRE(vt::press(*s.view, "Look up"));
    CHECK(looked == std::vector<std::string>{"#room:s.org", "#room:s.org"});

    s.view->open("#prefill:s");
    CHECK(s.view->alias_text() == "#prefill:s");
    CHECK(s.alias().text() == "#prefill:s");
    CHECK(s.view->state() == JoinRoomView::State::Idle);
}

TEST_CASE("JoinRoomView hosted: state banners and close", "[tk][view][join_room]")
{
    JrhStage s;
    s.view->set_state(JoinRoomView::State::Loading);
    s.run();
    CHECK(vt::has_name(*s.view, "Looking up room\xe2\x80\xa6"));
    s.view->set_error("");
    s.run();
    CHECK(vt::has_name(*s.view, "Room not found."));
    s.view->set_error("Boom");
    s.run();
    CHECK(vt::has_name(*s.view, "Boom"));

    int closed = 0;
    s.view->on_close = [&] { ++closed; };
    s.view->close();
    CHECK_FALSE(s.view->is_open());
    CHECK_FALSE(s.view->visible());
    CHECK(closed == 1);
    s.view->close(); // second close is a no-op
    CHECK(closed == 1);
}

TEST_CASE("JoinRoomView hosted: public room joins with its room id and "
          "Cancel fires on_cancel",
          "[tk][view][join_room]")
{
    JrhStage s;
    std::vector<std::string> joined;
    int cancelled = 0;
    s.view->on_join_requested = [&](const std::string& id) { joined.push_back(id); };
    s.view->on_cancel = [&] { ++cancelled; };
    s.view->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.view->set_preview(jrh_summary());
    s.run();
    CHECK(s.view->preview_room_id() == "!room:s");
    REQUIRE(vt::press(*s.view, "Join"));
    CHECK(joined == std::vector<std::string>{"!room:s"});
    CHECK(s.view->state() == JoinRoomView::State::Joining);
    s.run();
    CHECK(vt::has_name(*s.view, "Joining room\xe2\x80\xa6"));
    CHECK_FALSE(s.view->alias_field_visible());
    REQUIRE(vt::press(*s.view, "Cancel"));
    CHECK(cancelled == 1);
}

TEST_CASE("JoinRoomView hosted: knock rooms show a reason field and send it",
          "[tk][view][join_room]")
{
    JrhStage s;
    std::string knock_id, knock_reason;
    s.view->on_knock_requested = [&](const std::string& id, const std::string& r)
    {
        knock_id = id;
        knock_reason = r;
    };
    s.view->set_preview(jrh_summary("knock"));
    s.run();
    REQUIRE(vt::has_name(*s.view, "Request to Join"));
    s.reason().on_changed("pretty please");
    REQUIRE(vt::press(*s.view, "Request to Join"));
    CHECK(knock_id == "!room:s");
    CHECK(knock_reason == "pretty please");
    s.run();
    CHECK(vt::has_name(*s.view, "Sending request\xe2\x80\xa6"));

    // Already a member: a knock room is joined directly, not knocked on.
    auto member = jrh_summary("knock_restricted");
    member.membership = "join";
    s.view->set_preview(member);
    s.run();
    CHECK(vt::has_name(*s.view, "Join"));
    CHECK(s.reason().text().empty());
}

TEST_CASE("JoinRoomView hosted: topic links report hover and click",
          "[tk][view][join_room]")
{
    JrhStage s;
    std::vector<std::string> hovered, clicked;
    s.view->on_link_hovered = [&](std::string u) { hovered.push_back(u); };
    s.view->on_link_clicked = [&](std::string u) { clicked.push_back(u); };
    s.view->set_preview(jrh_summary("public", "Docs at https://example.org/docs please"));
    s.run();

    std::optional<tk::Point> hit;
    for (float y = 40; y < 400 && !hit; y += 3)
        for (float x = 10; x < 430; x += 3)
        {
            hovered.clear();
            s.view->on_pointer_move({x, y});
            if (!hovered.empty() && !hovered.back().empty())
            {
                hit = tk::Point{x, y};
                break;
            }
        }
    REQUIRE(hit.has_value());
    CHECK(hovered.back() == "https://example.org/docs");
    CHECK_FALSE(s.view->on_pointer_move(*hit)); // unchanged
    CHECK(s.view->on_pointer_down(*hit));
    s.view->on_pointer_up(*hit, true);
    CHECK(clicked == std::vector<std::string>{"https://example.org/docs"});

    // Press on the link, release outside: no click.
    s.view->on_pointer_down(*hit);
    s.view->on_pointer_up(*hit, false);
    CHECK(clicked.size() == 1);
    // Press elsewhere: not consumed.
    CHECK_FALSE(s.view->on_pointer_down({2, 2}));

    s.view->on_pointer_leave();
    CHECK(hovered.back().empty());
    s.view->on_pointer_leave(); // already clear
}

TEST_CASE("JoinRoomView hosted: theme, focus, title toggle and a11y",
          "[tk][view][join_room]")
{
    JrhStage s;
    s.view->on_theme_changed(tk::Theme::dark());
    s.view->focus_alias_field();
    CHECK(s.alias().focused_);
    s.view->set_title_visible(false);
    s.run();
    s.view->set_visible(false);
    s.view->set_visible(true);
    CHECK(s.view->access_name().empty()); // no preview card yet
    CHECK(s.view->access_role() == tk::Role::None);
    s.view->set_preview(jrh_summary("invite", "hello"));
    CHECK(s.view->access_role() == tk::Role::Group);
    const std::string name = s.view->access_name();
    CHECK(name.find("Test Room") != std::string::npos);
    CHECK(name.find("hello") != std::string::npos);
}
