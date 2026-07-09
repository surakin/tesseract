#include <catch2/catch_test_macros.hpp>

#include "views/QuickSwitcher.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <optional>
#include <string>
#include <vector>

using namespace tk;
using tesseract::RoomInfo;
using tesseract::views::QuickSwitcher;

namespace
{

struct TkQuickSwitcherStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 600);
    LayoutCtx lc()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& w, Rect bounds)
    {
        auto l = lc();
        w.measure(l, {bounds.w, bounds.h});
        w.arrange(l, bounds);
        PaintCtx p{surface->canvas(), surface->factory(), Theme::light()};
        w.paint(p);
    }
};

RoomInfo make_room(const char* id, const char* name)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    return r;
}

// A QuickSwitcher pre-wired with two rooms and capture hooks for every
// callback under test. open() is called so the overlay is live.
struct Harness
{
    QuickSwitcher qs;
    TkQuickSwitcherStage stage;

    std::optional<std::string> last_user_query;
    std::optional<std::string> selected_room;
    std::optional<std::string> selected_user;

    Harness()
    {
        qs.set_rooms_provider(
            []() -> std::vector<RoomInfo> {
                return {make_room("!a:srv", "Alpha"),
                        make_room("!b:srv", "Beta")};
            });
        qs.on_user_query_changed =
            [this](const std::string& q) { last_user_query = q; };
        qs.on_room_selected =
            [this](const std::string& id) { selected_room = id; };
        qs.on_user_selected =
            [this](const std::string& id) { selected_user = id; };
        qs.open();
    }

    void layout()
    {
        stage.run(qs, {0, 0, 640, 600});
    }
};

std::vector<QuickSwitcher::UserEntry> two_users()
{
    return {{"@alice:srv", "Alice", ""}, {"@alicia:srv", "Alicia", ""}};
}

} // namespace

TEST_CASE("QuickSwitcher: leading @ enters user mode and emits the query")
{
    Harness h;
    h.qs.set_query("@al");
    REQUIRE(h.last_user_query.has_value());
    CHECK(*h.last_user_query == "@al");
}

TEST_CASE("QuickSwitcher: non-@ query stays in room mode (no user query)")
{
    Harness h;
    h.qs.set_query("alp");
    CHECK_FALSE(h.last_user_query.has_value());
}

TEST_CASE("QuickSwitcher: activating a user row opens that mxid")
{
    Harness h;
    h.qs.set_query("@al");
    h.qs.set_user_results(two_users());
    h.layout();
    // set_user_results selects the first row; activate it.
    h.qs.activate_selected();
    REQUIRE(h.selected_user.has_value());
    CHECK(*h.selected_user == "@alice:srv");
    CHECK_FALSE(h.selected_room.has_value());
}

TEST_CASE("QuickSwitcher: arrow navigation selects the second user row")
{
    Harness h;
    h.qs.set_query("@al");
    h.qs.set_user_results(two_users());
    h.layout();
    h.qs.move_selection(+1);
    h.qs.activate_selected();
    REQUIRE(h.selected_user.has_value());
    CHECK(*h.selected_user == "@alicia:srv");
}

TEST_CASE("QuickSwitcher: user-mode selection is clamped to the list bounds")
{
    Harness h;
    h.qs.set_query("@al");
    h.qs.set_user_results(two_users());
    h.layout();
    h.qs.move_selection(+10); // past the end → last row
    h.qs.activate_selected();
    REQUIRE(h.selected_user.has_value());
    CHECK(*h.selected_user == "@alicia:srv");
}

TEST_CASE("QuickSwitcher: empty user results → activate is a no-op")
{
    Harness h;
    h.qs.set_query("@nobody:srv");
    h.qs.set_user_results({});
    h.layout();
    h.qs.activate_selected();
    CHECK_FALSE(h.selected_user.has_value());
}

TEST_CASE("QuickSwitcher: clearing @ returns to room mode and opens a room")
{
    Harness h;
    h.qs.set_query("@al");
    h.qs.set_user_results(two_users());
    // Back to room mode: late user results must be ignored, room activates.
    h.qs.set_query("alpha");
    h.layout();
    h.qs.activate_selected();
    REQUIRE(h.selected_room.has_value());
    CHECK(*h.selected_room == "!a:srv");
    CHECK_FALSE(h.selected_user.has_value());
}

TEST_CASE("QuickSwitcher: set_user_results is ignored once out of user mode")
{
    Harness h;
    h.qs.set_query("@al");
    h.qs.set_query("alpha"); // back to room mode
    h.qs.set_user_results(two_users());
    h.layout();
    h.qs.activate_selected();
    // The room list is active, so a room is selected, not a user.
    CHECK_FALSE(h.selected_user.has_value());
    REQUIRE(h.selected_room.has_value());
    CHECK(*h.selected_room == "!a:srv");
}
