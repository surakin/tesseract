#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk_test_host.h"
#include "views/MruSwitcher.h"
#include "views/QuickSwitcher.h"

#include <functional>
#include <string>
#include <vector>

using tesseract::views::MruSwitcher;
using tesseract::views::QuickSwitcher;

namespace
{
tesseract::RoomInfo room(std::string id, std::string name)
{
    tesseract::RoomInfo r;
    r.id = std::move(id);
    r.name = std::move(name);
    return r;
}

std::vector<std::string> list_item_names(const tk::AccessNode& n)
{
    std::vector<std::string> out;
    std::function<void(const tk::AccessNode&)> walk = [&](const tk::AccessNode& x)
    {
        if (x.role == tk::Role::ListItem)
            out.push_back(x.name);
        for (const auto& c : x.children)
            walk(c);
    };
    walk(n);
    return out;
}
} // namespace

TEST_CASE("QuickSwitcher room results are activatable ListItem nodes",
         "[quick_switcher][accessibility]")
{
    PopupCapableStubHost host;
    auto qs = tk::create_root_widget<QuickSwitcher>(&host);
    qs->set_rooms_provider(
        [] { return std::vector<tesseract::RoomInfo>{room("!a:x", "Alpha"),
                                                     room("!b:x", "Beta")}; });

    std::string chosen;
    qs->on_room_selected = [&](const std::string& id) { chosen = id; };
    qs->open();

    tk::AccessNode tree = tk::build_access_tree(qs.get());
    CHECK(list_item_names(tree) == std::vector<std::string>{"Alpha", "Beta"});

    const tk::AccessNode* list = nullptr;
    std::function<void(const tk::AccessNode&)> find =
        [&](const tk::AccessNode& n)
    {
        if (n.role == tk::Role::List)
            list = &n;
        for (const auto& c : n.children)
            find(c);
    };
    find(tree);
    REQUIRE(list != nullptr);
    REQUIRE(list->children.size() >= 2);
    CHECK(tk::invoke_default_action(list->children[1]));
    CHECK(chosen == "!b:x");
}

TEST_CASE("MruSwitcher chips are ListItem nodes with the held one selected",
         "[mru_switcher][accessibility]")
{
    StubHost host;
    auto mru = tk::create_root_widget<MruSwitcher>(&host);
    mru->set_recent_provider(
        [] { return std::vector<tesseract::RoomInfo>{
                 room("!a:x", "Alpha"), room("!b:x", "Beta"),
                 room("!c:x", "Gamma")}; });

    std::string switched;
    mru->on_room_selected = [&](const std::string& id) { switched = id; };
    mru->begin_cycle(); // preselects index 1 (the previous room)

    tk::AccessNode tree = tk::build_access_tree(mru.get());
    REQUIRE(tree.role == tk::Role::List);
    REQUIRE(tree.children.size() == 3);
    CHECK(tree.children[0].name == "Alpha");
    CHECK(tree.children[1].state.selected);
    CHECK_FALSE(tree.children[0].state.selected);

    CHECK(tk::invoke_default_action(tree.children[2]));
    CHECK(switched == "!c:x");
}
