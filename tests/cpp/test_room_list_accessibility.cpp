#include <catch2/catch_test_macros.hpp>
#include "tesseract/types.h"
#include "tk/access_tree.h"
#include "views/RoomListView.h"

#include <algorithm>

// Exercises RoomListView::Adapter's tk::ListAdapterAccessibility
// implementation end-to-end through build_access_tree() — the real-world
// consumer of the generic virtualized-list mechanism added to
// ui/shared/tk/access_tree.cpp/list_view.h.

using tesseract::RoomInfo;
using namespace tesseract::views;
using namespace tk;

namespace
{
RoomInfo named_room(std::string name, uint64_t last_ts = 1)
{
    RoomInfo r;
    r.name = std::move(name);
    r.last_activity_ts = last_ts;
    return r;
}

// Finds the first descendant (breadth-first through the already-flattened
// tree) with the given role — RoomListView itself has no access_role() of
// its own, so its ListView child's node attaches directly to the root's
// children per build_access_tree's flattening rule.
const AccessNode* find_role(const AccessNode& node, Role role)
{
    if (node.role == role)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_role(ch, role))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("RoomListView's room rows are accessible ListItems named after "
         "the room, under a List node",
         "[roomlist][accessibility]")
{
    auto v_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& v = *v_owner;

    std::vector<RoomInfo> rooms;
    rooms.push_back(named_room("Alpha Room", 3));
    rooms.push_back(named_room("Beta Room", 2));
    v.set_rooms(std::move(rooms));

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role(tree, Role::List);
    REQUIRE(list != nullptr);

    // At least the two room rows should be present as ListItems (a Rooms
    // section header is also a row, but role Button, not ListItem).
    std::vector<std::string> room_names;
    for (const auto& row : list->children)
        if (row.role == Role::ListItem)
            room_names.push_back(row.name);

    CHECK(std::find(room_names.begin(), room_names.end(), "Alpha Room") !=
         room_names.end());
    CHECK(std::find(room_names.begin(), room_names.end(), "Beta Room") !=
         room_names.end());
}

TEST_CASE("RoomListView's section headers are accessible Buttons reflecting "
         "expanded state",
         "[roomlist][accessibility]")
{
    auto v_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& v = *v_owner;

    std::vector<RoomInfo> rooms;
    rooms.push_back(named_room("Some Room"));
    v.set_rooms(std::move(rooms));

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* header = nullptr;
    for (const auto& row : list->children)
        if (row.role == Role::Button)
        {
            header = &row;
            break;
        }
    REQUIRE(header != nullptr);
    CHECK(header->state.expanded); // sections start expanded (collapsed_ = false)

    v.set_section_collapsed(RoomListView::kSecRooms, true);
    AccessNode tree2 = build_access_tree(&v);
    const AccessNode* list2 = find_role(tree2, Role::List);
    REQUIRE(list2 != nullptr);
    const AccessNode* header2 = nullptr;
    for (const auto& row : list2->children)
        if (row.role == Role::Button)
        {
            header2 = &row;
            break;
        }
    REQUIRE(header2 != nullptr);
    CHECK_FALSE(header2->state.expanded);
}

TEST_CASE("invoking a room row's default action selects that room, the "
         "same as a real click",
         "[roomlist][accessibility][action]")
{
    auto v_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& v = *v_owner;

    std::vector<RoomInfo> rooms;
    rooms.push_back(named_room("Alpha Room", 3));
    RoomInfo beta = named_room("Beta Room", 2);
    beta.id = "!beta:example.org";
    rooms.push_back(beta);
    v.set_rooms(std::move(rooms));

    std::string selected;
    v.on_room_selected = [&](const std::string& id) { selected = id; };

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* beta_row = nullptr;
    for (const auto& row : list->children)
        if (row.role == Role::ListItem && row.name == "Beta Room")
            beta_row = &row;
    REQUIRE(beta_row != nullptr);

    CHECK(invoke_default_action(*beta_row));
    CHECK(selected == "!beta:example.org");
}

TEST_CASE("invoking a header row's default action toggles its section's "
         "collapsed state, the same as a real click",
         "[roomlist][accessibility][action]")
{
    auto v_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& v = *v_owner;

    std::vector<RoomInfo> rooms;
    rooms.push_back(named_room("Some Room"));
    v.set_rooms(std::move(rooms));

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* header = nullptr;
    for (const auto& row : list->children)
        if (row.role == Role::Button)
            header = &row;
    REQUIRE(header != nullptr);
    CHECK(header->state.expanded);

    CHECK(invoke_default_action(*header));

    AccessNode tree2 = build_access_tree(&v);
    const AccessNode* list2 = find_role(tree2, Role::List);
    REQUIRE(list2 != nullptr);
    const AccessNode* header2 = nullptr;
    for (const auto& row : list2->children)
        if (row.role == Role::Button)
            header2 = &row;
    REQUIRE(header2 != nullptr);
    CHECK_FALSE(header2->state.expanded);
}
