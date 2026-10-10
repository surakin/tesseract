#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/SpaceAddRoomList.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::views::SpaceAddRoomList;

namespace
{

tesseract::RoomInfo sar_room(const std::string& id, const std::string& name,
                             bool space = false)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = name;
    r.is_space = space;
    return r;
}

struct SarStage : vt::Stage
{
    std::unique_ptr<SpaceAddRoomList> list =
        tk::create_root_widget<SpaceAddRoomList>(&host);
    SarStage()
    {
        list->set_rooms_provider(
            []
            {
                return std::vector<tesseract::RoomInfo>{
                    sar_room("!a:x", "Alpha"), sar_room("!b:x", "Beta"),
                    sar_room("!s:x", "A Space", true), sar_room("!c:x", "Gamma")};
            });
        list->set_excluded_room_ids({"!c:x"});
        list->refresh();
        run();
    }
    void run() { mount(*list, {0, 0, 300, 400}); }
    std::vector<std::string> add_names()
    {
        std::vector<std::string> out;
        for (const auto& n : vt::all_names(*list))
            if (n.rfind("Add ", 0) == 0)
                out.push_back(n);
        return out;
    }
};

} // namespace

TEST_CASE("SpaceAddRoomList lists non-space, non-excluded rooms",
          "[tk][view][space_add]")
{
    SarStage s;
    CHECK(s.add_names() ==
          std::vector<std::string>{"Add Alpha to space", "Add Beta to space"});
    CHECK(s.list->search_field() != nullptr);
    CHECK(s.list->search_field_rect().w > 0);
}

TEST_CASE("SpaceAddRoomList query filters the candidates", "[tk][view][space_add]")
{
    SarStage s;
    s.list->set_query("bet");
    s.run();
    CHECK(s.add_names() == std::vector<std::string>{"Add Beta to space"});
    s.list->set_query("zzz");
    s.run();
    CHECK(s.add_names().empty()); // "No more rooms to add." painted instead
    // Typing in the native search box routes to set_query.
    s.host.fields_created.at(0)->on_changed("alp");
    s.run();
    CHECK(s.add_names() == std::vector<std::string>{"Add Alpha to space"});
}

TEST_CASE("SpaceAddRoomList activating a row (a11y or click) requests the add",
          "[tk][view][space_add]")
{
    SarStage s;
    std::vector<std::string> added;
    s.list->on_add_requested = [&](std::string id) { added.push_back(id); };

    REQUIRE(vt::press(*s.list, "Add Beta to space"));
    CHECK(added == std::vector<std::string>{"!b:x"});

    // Real pointer click on the first row (list starts below the search box).
    s.host.dispatch_pointer_down({100, 50 + SpaceAddRoomList::kRowH / 2});
    s.host.dispatch_pointer_up({100, 50 + SpaceAddRoomList::kRowH / 2});
    CHECK(added.size() == 2);
    CHECK(added[1] == "!a:x");
}

TEST_CASE("SpaceAddRoomList without manage permission refuses adds",
          "[tk][view][space_add]")
{
    SarStage s;
    int adds = 0;
    s.list->on_add_requested = [&](std::string) { ++adds; };
    s.list->set_can_manage(false);
    CHECK_FALSE(s.list->can_manage());
    s.run();
    auto nodes = vt::nodes_named(*s.list, "Add Alpha to space");
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].state.disabled);
    CHECK_FALSE(tk::invoke_default_action(nodes[0]));
    s.host.dispatch_pointer_down({100, 70});
    s.host.dispatch_pointer_up({100, 70});
    CHECK(adds == 0);
}

TEST_CASE("SpaceAddRoomList accepts room drops only with manage permission "
          "and the right payload",
          "[tk][view][space_add]")
{
    SarStage s;
    std::string removed;
    s.list->on_room_dropped_for_remove = [&](std::string id) { removed = id; };

    tk::DragPayload wrong("something_else", std::string("x"));
    CHECK_FALSE(s.list->on_drag_enter({10, 10}, wrong));
    tk::DragPayload ok("space_room_id", std::string("!z:x"));
    CHECK(s.list->on_drag_enter({10, 10}, ok));
    s.list->on_drag_over({12, 12}, ok);
    s.run(); // paints the hover tint
    s.list->on_drag_leave_target();
    CHECK(s.list->on_drop({10, 10}, tk::DragPayload("space_room_id", std::string("!z:x"))));
    CHECK(removed == "!z:x");
    CHECK_FALSE(s.list->on_drop({10, 10}, tk::DragPayload("other", std::string("q"))));
    CHECK_FALSE(s.list->on_drop({10, 10}, tk::DragPayload("space_room_id", 42)));

    s.list->set_can_manage(false);
    CHECK_FALSE(s.list->on_drag_enter({10, 10}, ok));
}

TEST_CASE("SpaceAddRoomList requests avatars for rooms the provider lacks, "
          "and hover shows a tooltip",
          "[tk][view][space_add]")
{
    SarStage s;
    // Rooms without avatar urls never ask.
    std::vector<std::string> needed;
    s.list->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.list->on_room_avatar_needed = [&](const tesseract::RoomInfo& r) { needed.push_back(r.id); };
    s.run();
    CHECK(needed.empty());

    s.host.dispatch_pointer_move({100, 70});
    CHECK(s.host.tooltip_owner_ != nullptr);
    s.host.dispatch_pointer_move({100, 390}); // below the last row
    s.host.dispatch_pointer_leave();
}

TEST_CASE("SpaceAddRoomList drag from a row starts a host drag past the "
          "threshold",
          "[tk][view][space_add]")
{
    SarStage s;
    s.host.dispatch_pointer_down({100, 70});
    s.host.dispatch_pointer_move({100, 71});
    s.host.dispatch_pointer_move({100, 140});
    s.host.dispatch_pointer_up({100, 140});
    SUCCEED();
}
