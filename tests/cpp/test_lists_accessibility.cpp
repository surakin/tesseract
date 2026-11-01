#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "access_test_util.h"
#include "tk/list_view.h"
#include "views/ForwardRoomPicker.h"
#include "views/ReceiptGridPopup.h"
#include "views/RoomDirectoryView.h"
#include "views/SpaceAddRoomList.h"
#include "views/SpaceChildRoomGrid.h"

#include <tesseract/types.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// List/grid adapters that used to expose nothing to assistive technology,
// plus the per-row subtree identity fix in build_access_tree.

using namespace tk;
using tesseract::views::ForwardRoomPicker;
using tesseract::views::ReceiptGridPopup;
using tesseract::views::RoomDirectoryView;
using tesseract::views::SpaceAddRoomList;
using tesseract::views::SpaceChildRoomGrid;

namespace lists_a11y_test
{

tesseract::RoomInfo lists_a11y_room(std::string id, std::string name, std::string topic = {})
{
    tesseract::RoomInfo r;
    r.id = std::move(id);
    r.name = std::move(name);
    r.topic = std::move(topic);
    return r;
}

// Rows with a two-node subtree each, to check subtree nodes get distinct
// identities.
class ListsA11ySubtreeAdapter : public ListAdapter, public ListAdapterAccessibility
{
public:
    bool extra_first = false; // prepend a node to row 1's subtree
    bool hide_row_1 = false;  // expose row 1 as Role::None
    std::size_t count() const override
    {
        return 3;
    }
    float measure_row_height(std::size_t, LayoutCtx&, float) override
    {
        return 20;
    }
    void paint_row(std::size_t, PaintCtx&, Rect, bool, bool) override
    {
    }
    Role access_role_for_row(std::size_t i) const override
    {
        return (hide_row_1 && i == 1) ? Role::None : Role::ListItem;
    }
    std::string access_name_for_row(std::size_t i) const override
    {
        return "row " + std::to_string(i);
    }
    std::vector<AccessNode> access_subtree_for_row(std::size_t i) const override
    {
        std::vector<AccessNode> out;
        if (extra_first && i == 1)
        {
            AccessNode n;
            n.role = Role::Link;
            n.name = "new link";
            n.activate = []
            {
                return true;
            };
            out.push_back(std::move(n));
        }
        for (int k = 0; k < 2; ++k)
        {
            AccessNode n;
            n.role = Role::Button;
            n.name = "row " + std::to_string(i) + " action " + std::to_string(k);
            n.activate = []
            {
                return true;
            };
            out.push_back(std::move(n));
        }
        return out;
    }
};

} // namespace lists_a11y_test

using lists_a11y_test::lists_a11y_room;
using lists_a11y_test::ListsA11ySubtreeAdapter;
using access_test::find_named;
using access_test::find_prefix;

TEST_CASE("subtree nodes get distinct, stable identities", "[tk][access_tree][accessibility]")
{
    auto list = create_root_widget<ListView>(nullptr);
    ListsA11ySubtreeAdapter ad;
    list->set_adapter(&ad);
    AccessNode tree = build_access_tree(list.get());
    REQUIRE(tree.children.size() == 3);

    std::set<std::pair<Widget*, int>> keys;
    std::vector<std::pair<Widget*, int>> first_pass;
    for (const auto& row : tree.children)
    {
        keys.insert({row.widget, row.row_index});
        for (const auto& sub : row.children)
        {
            CHECK(sub.widget == list.get());
            CHECK(sub.row_index <= -2);
            keys.insert({sub.widget, sub.row_index});
            first_pass.emplace_back(sub.widget, sub.row_index);
        }
    }
    CHECK(keys.size() == 3 + 6);

    AccessNode again = build_access_tree(list.get());
    std::vector<std::pair<Widget*, int>> second_pass;
    for (const auto& row : again.children)
    {
        for (const auto& sub : row.children)
        {
            second_pass.emplace_back(sub.widget, sub.row_index);
        }
    }
    CHECK(first_pass == second_pass);
}

TEST_CASE("RoomDirectoryView results are list items that select on activate",
          "[roomdirectory][accessibility]")
{
    auto v = create_root_widget<RoomDirectoryView>(nullptr);
    std::uint64_t id = 0;
    v->on_search_requested = [&](std::uint64_t rid, const std::string&, const std::string&)
    {
        id = rid;
    };
    v->open();
    tesseract::RoomDirectoryEntry e;
    e.room_id = "!a:s";
    e.name = "Rust";
    e.topic = "Crabs";
    e.join_rule = "public";
    e.joined_members = 3;
    v->set_results(id, {e}, false);

    AccessNode tree = build_access_tree(v.get());
    const AccessNode* row = find_named(tree, "Rust");
    REQUIRE(row != nullptr);
    CHECK(row->role == Role::ListItem);
    CHECK(row->description.find("3 members") != std::string::npos);
    CHECK(row->description.find("Crabs") != std::string::npos);
    CHECK_FALSE(row->state.selected);
    CHECK(invoke_default_action(*row));

    tree = build_access_tree(v.get());
    row = find_named(tree, "Rust");
    REQUIRE(row != nullptr);
    CHECK(row->state.selected);
}

TEST_CASE("SpaceAddRoomList rows are add buttons, disabled without permission",
          "[space][accessibility]")
{
    auto v = create_root_widget<SpaceAddRoomList>(nullptr);
    v->set_rooms_provider(
        []
        {
            return std::vector<tesseract::RoomInfo>{lists_a11y_room("!r:s", "Lobby")};
        });
    v->refresh();
    std::string added;
    v->on_add_requested = [&](std::string rid)
    {
        added = std::move(rid);
    };

    AccessNode tree = build_access_tree(v.get());
    const AccessNode* row = find_named(tree, "Add Lobby to space");
    REQUIRE(row != nullptr);
    CHECK(row->role == Role::Button);
    CHECK_FALSE(row->state.disabled);
    CHECK(invoke_default_action(*row));
    CHECK(added == "!r:s");

    v->set_can_manage(false);
    added.clear();
    tree = build_access_tree(v.get());
    row = find_named(tree, "Add Lobby to space");
    REQUIRE(row != nullptr);
    CHECK(row->state.disabled);
    CHECK_FALSE(invoke_default_action(*row));
    CHECK(added.empty());
}

TEST_CASE("SpaceChildRoomGrid cells are named rooms with a remove hint", "[space][accessibility]")
{
    auto v = create_root_widget<SpaceChildRoomGrid>(nullptr);
    v->on_remove_requested = [](std::string) {};
    v->set_children(
        {{lists_a11y_room("!a:s", "General", "Chat"), true}, {lists_a11y_room("!b:s", ""), false}});

    AccessNode tree = build_access_tree(v.get());
    const AccessNode* cell = find_named(tree, "General");
    REQUIRE(cell != nullptr);
    CHECK(cell->role == Role::GridCell);
    CHECK(cell->description.find("Chat") != std::string::npos);
    CHECK(cell->description.find("Delete") != std::string::npos);
    CHECK(cell->grid_row == 0);
    CHECK(find_named(tree, "Loading\xe2\x80\xa6") != nullptr);
}

TEST_CASE("ReceiptGridPopup names each reader under a 'Read by' group", "[receipts][accessibility]")
{
    auto v = create_root_widget<ReceiptGridPopup>(nullptr);
    v->set_entries({{"@a:s", "Alice", "", 0}, {"@b:s", "", "", 0}});
    v->set_visible(true);

    AccessNode tree = build_access_tree(v.get());
    CHECK(tree.role == Role::Group);
    CHECK(tree.name == "Read by 2 people");
    CHECK(find_named(tree, "Alice") != nullptr);
    CHECK(find_named(tree, "@b:s") != nullptr);
}

TEST_CASE("ForwardRoomPicker is a modal dialog of lists_a11y_room checkboxes",
          "[forward][accessibility]")
{
    auto v = create_root_widget<ForwardRoomPicker>(nullptr);
    v->set_rooms_provider(
        []
        {
            return std::vector<tesseract::RoomInfo>{lists_a11y_room("!a:s", "Alpha"),
                                                    lists_a11y_room("!b:s", "Beta")};
        });
    v->open("!src:s");

    AccessNode tree = build_access_tree(v.get());
    CHECK(tree.role == Role::Dialog);
    CHECK(tree.modal);
    const AccessNode* beta = find_named(tree, "Beta");
    REQUIRE(beta != nullptr);
    CHECK(beta->role == Role::CheckBox);
    CHECK_FALSE(beta->state.checked);
    CHECK(invoke_default_action(*beta));

    tree = build_access_tree(v.get());
    beta = find_named(tree, "Beta");
    REQUIRE(beta != nullptr);
    CHECK(beta->state.checked);

    bool closed = false;
    v->on_close = [&]
    {
        closed = true;
    };
    const AccessNode* cancel = find_named(tree, "Cancel");
    REQUIRE(cancel != nullptr);
    CHECK(invoke_default_action(*cancel));
    CHECK(closed);
}

TEST_CASE("a subtree node keeps its key when a sibling is inserted before it",
          "[tk][access_tree][accessibility]")
{
    auto list = create_root_widget<ListView>(nullptr);
    ListsA11ySubtreeAdapter ad;
    list->set_adapter(&ad);
    auto key_of = [&](const std::string& name)
    {
        AccessNode tree = build_access_tree(list.get());
        const AccessNode* n = find_named(tree, name);
        REQUIRE(n != nullptr);
        return n->row_index;
    };
    const int before = key_of("row 1 action 0");
    ad.extra_first = true;
    CHECK(key_of("row 1 action 0") == before);
    CHECK(key_of("new link") != before);
}

TEST_CASE("item positions count only exposed rows", "[tk][access_tree][accessibility]")
{
    auto list = create_root_widget<ListView>(nullptr);
    ListsA11ySubtreeAdapter ad;
    ad.hide_row_1 = true;
    list->set_adapter(&ad);
    AccessNode tree = build_access_tree(list.get());
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[1].name == "row 2");
    CHECK(tree.children[1].pos_in_set == 2);
    CHECK(tree.children[1].row_set_size == 2);
    CHECK(tree.children[1].row_index == 2); // identity unchanged
}

TEST_CASE("ForwardRoomPicker can't be confirmed twice or escaped mid-forward",
          "[forward][accessibility]")
{
    auto v = create_root_widget<ForwardRoomPicker>(nullptr);
    v->set_rooms_provider(
        []
        {
            return std::vector<tesseract::RoomInfo>{lists_a11y_room("!a:s", "Alpha")};
        });
    v->open("!src:s");
    int fired = 0;
    v->on_confirmed = [&](std::vector<std::string>)
    {
        ++fired;
        v->set_forwarding(1);
    };
    AccessNode tree = build_access_tree(v.get());
    const AccessNode* alpha = find_named(tree, "Alpha");
    REQUIRE(alpha != nullptr);
    invoke_default_action(*alpha);
    v->confirm();
    v->confirm();
    CHECK(fired == 1);

    KeyEvent esc;
    esc.key = Key::Escape;
    CHECK(v->on_key_down(esc));
    CHECK(v->is_open()); // still forwarding
}
