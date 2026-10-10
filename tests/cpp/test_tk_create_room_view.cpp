#include <catch2/catch_test_macros.hpp>

#include "tk/combo_button.h"
#include "view_test_util.h"
#include "views/CreateRoomView.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::views::CreateRoomView;

namespace
{

struct CrStage : vt::Stage
{
    std::unique_ptr<CreateRoomView> view =
        tk::create_root_widget<CreateRoomView>(&host);
    CrStage()
    {
        mount(*view, {0, 0, CreateRoomView::kPreferredW,
                      CreateRoomView::kPreferredH});
    }
    // fields_created order: name, alias, reason; areas: topic, invite.
    StubTextField& name() { return *host.fields_created.at(0); }
    StubTextField& alias() { return *host.fields_created.at(1); }
    StubTextField& reason() { return *host.fields_created.at(2); }
    StubTextArea& topic() { return *host.areas_created.at(0); }
    StubTextArea& invite() { return *host.areas_created.at(1); }
    void relayout()
    {
        relayout_paint(*view, {0, 0, CreateRoomView::kPreferredW,
                               CreateRoomView::kPreferredH});
    }
};

} // namespace

TEST_CASE("CreateRoomView measures to a positive size", "[tk][view][create_room]")
{
    CrStage s;
    auto lc = s.lc();
    tk::Size sz = s.view->measure(lc, {CreateRoomView::kPreferredW, 800});
    CHECK(sz.w > 0);
    CHECK(sz.h > 0);
    s.view->set_title_visible(false);
    tk::Size sz2 = s.view->measure(lc, {CreateRoomView::kPreferredW, 800});
    CHECK(sz2.h <= sz.h);
}

TEST_CASE("CreateRoomView create button builds options from the form",
          "[tk][view][create_room]")
{
    CrStage s;
    s.name().set_text("My Room");
    s.alias().set_text("my-room");
    s.reason().set_text("join us");
    s.topic().set_text("topic text");
    s.invite().set_text(" @a:x , @b:y\n\n@c:z ");

    tesseract::RoomCreateOptions got;
    int calls = 0;
    s.view->on_create_requested = [&](tesseract::RoomCreateOptions o)
    {
        got = std::move(o);
        ++calls;
    };

    REQUIRE(vt::press(*s.view, "Create Room"));
    CHECK(calls == 1);
    CHECK(got.name == "My Room");
    CHECK(got.room_alias_local_part == "my-room");
    CHECK(got.invite_reason == "join us");
    CHECK(got.topic == "topic text");
    CHECK(got.invite == std::vector<std::string>{"@a:x", "@b:y", "@c:z"});
    CHECK(got.visibility == "private");
    CHECK_FALSE(got.encrypted);
    CHECK_FALSE(got.is_space);
    CHECK_FALSE(got.is_call_room);
}

TEST_CASE("CreateRoomView encryption checkbox and visibility feed the options",
          "[tk][view][create_room]")
{
    CrStage s;
    tesseract::RoomCreateOptions got;
    s.view->on_create_requested = [&](tesseract::RoomCreateOptions o)
    { got = std::move(o); };

    auto* check = vt::find<tk::CheckButton>(*s.view);
    REQUIRE(check != nullptr);
    check->set_checked(true);
    auto* vis = vt::find<tk::ComboBox>(*s.view);
    REQUIRE(vis != nullptr);
    vis->set_selected_value("public");

    vt::press(*s.view, "Create Room");
    CHECK(got.encrypted);
    CHECK(got.visibility == "public");
}

TEST_CASE("CreateRoomView switching to Create Space disables encryption and "
          "retitles placeholders; reset restores",
          "[tk][view][create_room]")
{
    CrStage s;
    auto* check = vt::find<tk::CheckButton>(*s.view);
    auto* combo = vt::find<tk::ComboButton>(*s.view);
    REQUIRE(check != nullptr);
    REQUIRE(combo != nullptr);
    check->set_checked(true);

    // Drive the dropdown for real: open via chevron, pick "Create Space".
    combo->on_selection_changed("space");
    combo->set_selected_value("space");
    CHECK_FALSE(check->checked());
    CHECK_FALSE(check->enabled());

    tesseract::RoomCreateOptions got;
    s.view->on_create_requested = [&](tesseract::RoomCreateOptions o)
    { got = std::move(o); };
    vt::press(*s.view, "Create Space");
    CHECK(got.is_space);
    CHECK_FALSE(got.encrypted);

    combo->on_selection_changed("call");
    combo->set_selected_value("call");
    CHECK(check->enabled());
    vt::press(*s.view, "Create Call Room");
    CHECK(got.is_call_room);

    s.name().set_text("keep?");
    s.view->reset();
    CHECK(s.name().text().empty());
    CHECK(combo->selected_value() == "room");
    CHECK(check->enabled());
    CHECK(s.view->state() == CreateRoomView::State::Idle);
}

TEST_CASE("CreateRoomView cancel button fires on_cancel", "[tk][view][create_room]")
{
    CrStage s;
    int cancelled = 0;
    s.view->on_cancel = [&] { ++cancelled; };
    REQUIRE(vt::press(*s.view, "Cancel"));
    CHECK(cancelled == 1);
}

TEST_CASE("CreateRoomView state transitions: creating hides the form, error "
          "shows a message",
          "[tk][view][create_room]")
{
    CrStage s;
    s.view->set_state(CreateRoomView::State::Creating);
    CHECK(s.view->state() == CreateRoomView::State::Creating);
    s.relayout();
    CHECK_FALSE(vt::has_name(*s.view, "Cancel"));
    CHECK_FALSE(vt::press(*s.view, "Create Room"));

    s.view->set_error("Server said no");
    CHECK(s.view->state() == CreateRoomView::State::Error);
    s.relayout();
    CHECK(vt::has_name(*s.view, "Server said no"));
    CHECK(vt::has_name(*s.view, "Cancel"));

    s.view->set_state(CreateRoomView::State::Idle);
    s.relayout();
    CHECK_FALSE(vt::has_name(*s.view, "Server said no"));

    // Empty error message falls back to a generic per-kind string.
    s.view->set_error("");
    CHECK(vt::has_name(*s.view, "Couldn't create room."));
}

TEST_CASE("CreateRoomView visibility toggling, theme change and focus are safe",
          "[tk][view][create_room]")
{
    CrStage s;
    s.view->set_visible(false);
    s.view->set_visible(true);
    s.view->on_theme_changed(tk::Theme::dark());
    s.view->focus_name_field();
    CHECK(s.name().focused_);
    s.view->set_title_visible(false);
    s.relayout();
}
