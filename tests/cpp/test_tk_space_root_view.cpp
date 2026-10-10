#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/SpaceRootView.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::views::SpaceChildRoomGrid;
using tesseract::views::SpaceRootView;

namespace
{

tesseract::RoomInfo srv_room(const std::string& id, const std::string& name)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = name;
    return r;
}

struct SrvStage : vt::Stage
{
    std::unique_ptr<SpaceRootView> view =
        tk::create_root_widget<SpaceRootView>(&host);
    tesseract::RoomInfo space = srv_room("!space:x", "My Space");
    SrvStage()
    {
        space.topic = "Welcome, see https://example.org for more";
        space.canonical_alias = "#space:x";
        space.avatar_url = "mxc://x/avatar";
        view->set_candidate_rooms_provider(
            [] {
                return std::vector<tesseract::RoomInfo>{srv_room("!a:x", "Alpha"),
                                                        srv_room("!b:x", "Beta")};
            });
    }
    void run(float w = 900, float h = 700) { mount(*view, {0, 0, w, h}); }
};

} // namespace

TEST_CASE("SpaceRootView is hidden until a space is set and after clear",
          "[tk][view][space_root]")
{
    SrvStage s;
    CHECK_FALSE(s.view->visible());
    s.view->set_space(s.space, 2, 1);
    CHECK(s.view->visible());
    s.run();
    s.view->clear();
    CHECK_FALSE(s.view->visible());
    s.run(); // painting with no space is a no-op
}

TEST_CASE("SpaceRootView requests the missing space avatar when painting",
          "[tk][view][space_root]")
{
    SrvStage s;
    std::vector<std::string> needed;
    s.view->on_avatar_needed = [&](const std::string& m) { needed.push_back(m); };
    s.view->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.view->set_space(s.space, 0, 0);
    s.run();
    REQUIRE_FALSE(needed.empty());
    CHECK(needed[0] == "mxc://x/avatar");
}

TEST_CASE("SpaceRootView leave button reports the space id",
          "[tk][view][space_root]")
{
    SrvStage s;
    s.view->set_space(s.space, 1, 0);
    s.run();
    std::string left;
    s.view->on_leave_space = [&](std::string id) { left = id; };
    REQUIRE(vt::press(*s.view, "Leave Space"));
    CHECK(left == "!space:x");
}

TEST_CASE("SpaceRootView settings button opens the settings view and hides "
          "the chrome",
          "[tk][view][space_root]")
{
    SrvStage s;
    s.view->set_space(s.space, 1, 0);
    s.run();
    std::string opened;
    int layout_changes = 0;
    s.view->on_settings_opened = [&](std::string id) { opened = id; };
    s.view->on_layout_changed = [&] { ++layout_changes; };

    REQUIRE(s.view->settings_view() != nullptr);
    CHECK_FALSE(s.view->settings_view()->is_open());
    REQUIRE(vt::press(*s.view, "Space settings"));
    CHECK(opened == "!space:x");
    CHECK(s.view->settings_view()->is_open());
    CHECK(layout_changes >= 1);
    s.run(); // arranges/paints the settings view instead
    CHECK_FALSE(vt::has_name(*s.view, "Leave Space"));

    // Switching to a different space closes an open settings view.
    s.view->set_space(srv_room("!other:x", "Other"), 0, 0);
    CHECK_FALSE(s.view->settings_view()->is_open());
    s.run();
    CHECK(vt::has_name(*s.view, "Leave Space"));
}

TEST_CASE("SpaceRootView add/remove flows route through the space id",
          "[tk][view][space_root]")
{
    SrvStage s;
    s.view->set_space(s.space, 1, 0);
    s.view->set_can_manage_children(true);
    SpaceChildRoomGrid::ChildRoomEntry child;
    child.info = srv_room("!b:x", "Beta");
    s.view->set_children({child}); // Beta is a child, so it's not a candidate
    s.run();

    std::vector<std::string> adds, removes;
    s.view->on_add_room_to_space = [&](std::string sp, std::string r)
    { adds.push_back(sp + ">" + r); };
    s.view->on_remove_room_from_space = [&](std::string sp, std::string r)
    { removes.push_back(sp + ">" + r); };

    CHECK_FALSE(vt::has_name(*s.view, "Add Beta to space"));
    REQUIRE(vt::press(*s.view, "Add Alpha to space"));
    CHECK(adds == std::vector<std::string>{"!space:x>!a:x"});

    auto* grid = vt::find<SpaceChildRoomGrid>(*s.view);
    auto* list = vt::find<tesseract::views::SpaceAddRoomList>(*s.view);
    REQUIRE(grid != nullptr);
    REQUIRE(list != nullptr);
    CHECK(grid->on_drop({0, 0}, tk::DragPayload("space_room_id", std::string("!z:x"))));
    CHECK(adds.back() == "!space:x>!z:x");
    CHECK(list->on_drop({0, 0}, tk::DragPayload("space_room_id", std::string("!b:x"))));
    CHECK(removes == std::vector<std::string>{"!space:x>!b:x"});
    grid->on_remove_requested("!b:x");
    CHECK(removes.size() == 2);
    grid->on_unjoined_summary_needed("!u:x");
    grid->on_room_avatar_needed(srv_room("!u:x", ""));
    list->on_room_avatar_needed(srv_room("!a:x", ""));

    std::vector<std::string> summaries;
    s.view->on_child_summary_needed = [&](std::string id) { summaries.push_back(id); };
    grid->on_unjoined_summary_needed("!u:x");
    CHECK(summaries == std::vector<std::string>{"!u:x"});
    int avatars = 0;
    s.view->on_room_avatar_needed = [&](const tesseract::RoomInfo&) { ++avatars; };
    grid->on_room_avatar_needed(srv_room("!u:x", ""));
    list->on_room_avatar_needed(srv_room("!a:x", ""));
    CHECK(avatars == 2);
}

TEST_CASE("SpaceRootView hides the management lists without permission and "
          "lays out narrow and wide",
          "[tk][view][space_root]")
{
    SrvStage s;
    s.view->set_space(s.space, 3, 2);
    s.view->set_can_manage_children(false);
    s.run();
    CHECK_FALSE(vt::has_name(*s.view, "Add Alpha to space"));
    s.view->set_can_manage_children(true);
    s.run(900, 700);  // side-by-side
    s.run(400, 800);  // stacked
    s.run(900, 700);
    s.view->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.run();
    CHECK(s.view->on_pointer_down({1, 1}));
}

TEST_CASE("SpaceRootView settings callbacks are forwarded",
          "[tk][view][space_root]")
{
    SrvStage s;
    s.view->set_space(s.space, 0, 0);
    s.run();
    std::string upload, copied, left;
    s.view->on_settings_avatar_upload_requested = [&](std::string id) { upload = id; };
    s.view->on_copy_to_clipboard = [&](std::string t) { copied = t; };
    s.view->on_leave_space = [&](std::string id) { left = id; };
    auto* sv = s.view->settings_view();
    REQUIRE(sv != nullptr);
    sv->on_avatar_upload_clicked();
    sv->on_copy_to_clipboard("hello");
    sv->on_leave_room("!space:x");
    sv->on_avatar_remove_clicked();
    sv->on_cancel();
    CHECK(upload == "!space:x");
    CHECK(copied == "hello");
    CHECK(left == "!space:x");
}
