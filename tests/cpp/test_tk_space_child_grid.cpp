#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/SpaceChildRoomGrid.h"

#include <tesseract/types.h>

#include <algorithm>
#include <string>
#include <vector>

using tesseract::views::SpaceChildRoomGrid;

namespace
{

SpaceChildRoomGrid::ChildRoomEntry scg_entry(const std::string& id,
                                             const std::string& name,
                                             bool joined = true,
                                             const std::string& topic = "")
{
    SpaceChildRoomGrid::ChildRoomEntry e;
    e.info.id = id;
    e.info.name = name;
    e.info.topic = topic;
    e.joined = joined;
    return e;
}

struct ScgStage : vt::Stage
{
    std::unique_ptr<SpaceChildRoomGrid> grid =
        tk::create_root_widget<SpaceChildRoomGrid>(&host);
    ScgStage()
    {
        grid->set_children({scg_entry("!a:x", "Alpha", true, "topic a"),
                            scg_entry("!b:x", "Beta"),
                            scg_entry("!u:x", "", false)});
        run();
    }
    void run() { mount(*grid, {0, 0, 520, 300}); }
};

} // namespace

TEST_CASE("SpaceChildRoomGrid exposes one GridCell per child",
          "[tk][view][space_grid][accessibility]")
{
    ScgStage s;
    auto names = vt::all_names(*s.grid);
    auto has = [&](const std::string& n)
    { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("Alpha"));
    CHECK(has("Beta"));
    CHECK(has("Loading\xe2\x80\xa6")); // un-joined child awaiting its summary
}

TEST_CASE("SpaceChildRoomGrid asks for an unjoined child's summary once",
          "[tk][view][space_grid]")
{
    ScgStage s;
    std::vector<std::string> asked;
    s.grid->on_unjoined_summary_needed = [&](std::string id) { asked.push_back(id); };
    s.grid->set_children({scg_entry("!u:x", "", false)});
    s.run();
    s.run(); // repaint: not asked again
    CHECK(asked == std::vector<std::string>{"!u:x"});
    s.grid->set_children({scg_entry("!u:x", "", false)}); // reset -> asks again
    s.run();
    CHECK(asked.size() == 2);
}

TEST_CASE("SpaceChildRoomGrid requests avatars missing from the provider",
          "[tk][view][space_grid]")
{
    ScgStage s;
    std::vector<std::string> needed;
    s.grid->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.grid->on_room_avatar_needed = [&](const tesseract::RoomInfo& r) { needed.push_back(r.id); };
    auto e = scg_entry("!a:x", "Alpha");
    e.info.avatar_url = "mxc://x/a";
    s.grid->set_children({e});
    s.run();
    CHECK(needed == std::vector<std::string>{"!a:x"});
}

TEST_CASE("SpaceChildRoomGrid Delete removes the selected child when "
          "permitted",
          "[tk][view][space_grid]")
{
    ScgStage s;
    std::vector<std::string> removed;
    s.grid->on_remove_requested = [&](std::string id) { removed.push_back(id); };

    // Click the first cell (8px padding, 220x48 cells) to select + focus it.
    s.host.dispatch_pointer_down({60, 30});
    s.host.dispatch_pointer_up({60, 30});
    CHECK(s.host.dispatch_key_down(vt::key(tk::Key::Delete)));
    CHECK(removed == std::vector<std::string>{"!a:x"});

    s.grid->set_can_manage(false);
    CHECK_FALSE(s.grid->can_manage());
    s.host.dispatch_key_down(vt::key(tk::Key::Delete));
    CHECK(removed.size() == 1);
    s.run(); // paints the no-permission notice
}

TEST_CASE("SpaceChildRoomGrid description hints at Delete only when removable",
          "[tk][view][space_grid][accessibility]")
{
    ScgStage s;
    s.grid->on_remove_requested = [](std::string) {};
    auto tree = tk::build_access_tree(s.grid.get());
    auto* alpha = vt::node_named(tree, "Alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->description.find("topic a") != std::string::npos);
    CHECK(alpha->description.find("Delete") != std::string::npos);
    auto* beta = vt::node_named(tree, "Beta");
    REQUIRE(beta != nullptr);
    CHECK(beta->description.find("Delete") != std::string::npos);
}

TEST_CASE("SpaceChildRoomGrid drops add rooms only with the right payload",
          "[tk][view][space_grid]")
{
    ScgStage s;
    std::string added;
    s.grid->on_room_dropped_for_add = [&](std::string id) { added = id; };
    CHECK_FALSE(s.grid->on_drag_enter({1, 1}, tk::DragPayload("nope", std::string("x"))));
    CHECK(s.grid->on_drag_enter({1, 1}, tk::DragPayload("space_room_id", std::string("!q:x"))));
    s.grid->on_drag_over({2, 2}, tk::DragPayload("space_room_id", std::string("!q:x")));
    s.run();
    s.grid->on_drag_leave_target();
    CHECK(s.grid->on_drop({1, 1}, tk::DragPayload("space_room_id", std::string("!q:x"))));
    CHECK(added == "!q:x");
    CHECK_FALSE(s.grid->on_drop({1, 1}, tk::DragPayload("space_room_id", 7)));
    CHECK_FALSE(s.grid->on_drop({1, 1}, tk::DragPayload("x", std::string("y"))));
    s.grid->set_can_manage(false);
    CHECK_FALSE(s.grid->on_drag_enter({1, 1}, tk::DragPayload("space_room_id", std::string("!q:x"))));
}

TEST_CASE("SpaceChildRoomGrid dragging a cell begins a host drag; hover "
          "shows a tooltip; empty grid paints a placeholder",
          "[tk][view][space_grid]")
{
    ScgStage s;
    s.host.dispatch_pointer_move({60, 30});
    CHECK(s.host.tooltip_owner_ != nullptr);
    s.host.dispatch_pointer_down({60, 30});
    s.host.dispatch_pointer_move({60, 32});
    s.host.dispatch_pointer_move({300, 150});
    CHECK(s.host.is_dragging());
    s.host.dispatch_pointer_up({300, 150});
    s.host.dispatch_pointer_leave();

    s.grid->set_children({});
    s.run();
}
