#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/combobox.h"
#include "tk_test_host.h"
#include "views/AlertDialog.h"
#include "views/ConfirmDialog.h"
#include "views/PopupMenu.h"

#include <functional>
#include <string>
#include <vector>

// The context menu and the combobox/searchable-picker dropdowns render their
// rows in a separate popup Surface. These exercise the WidgetRowAccessibility
// mapping on those popup roots via a StubPopupSurface that just retains the
// mounted root for build_access_tree().

using tesseract::views::PopupMenu;

namespace
{
std::vector<std::string> row_names(const tk::AccessNode& n, tk::Role r)
{
    std::vector<std::string> out;
    std::function<void(const tk::AccessNode&)> walk = [&](const tk::AccessNode& x)
    {
        if (x.role == r)
            out.push_back(x.name);
        for (const auto& c : x.children)
            walk(c);
    };
    walk(n);
    return out;
}
} // namespace

TEST_CASE("PopupMenu items are MenuItem nodes; separators/disabled are omitted",
         "[popup_menu][accessibility]")
{
    PopupCapableStubHost host;
    auto menu = tk::create_root_widget<PopupMenu>(&host);

    int reacted = 0;
    std::vector<PopupMenu::Item> items;
    items.push_back({{}, {}, "React", false, [&] { ++reacted; }, false, true});
    items.push_back({{}, {}, "", false, {}, /*is_separator=*/true, true});
    items.push_back({{}, {}, "Delete", true, {}, false, /*enabled=*/false});
    items.push_back({{}, {}, "Reply", false, [] {}, false, true});
    menu->open(std::move(items), {});

    REQUIRE(host.popups_created.size() == 1);
    tk::Widget* root = host.popups_created[0]->root();
    REQUIRE(root != nullptr);

    tk::AccessNode tree = tk::build_access_tree(root);
    CHECK(row_names(tree, tk::Role::MenuItem) ==
          std::vector<std::string>{"React", "Reply"});

    // Invoke the "React" node → fires its on_selected (and on_dismissed).
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
    REQUIRE_FALSE(list->children.empty());
    CHECK(tk::invoke_default_action(list->children[0]));
    CHECK(reacted == 1);
}

TEST_CASE("ComboBox dropdown options are selectable ListItem nodes",
         "[combobox][accessibility]")
{
    PopupCapableStubHost host;
    auto combo = tk::create_root_widget<tk::ComboBox>(&host);
    combo->set_options({{"Light", "light"}, {"Dark", "dark"}}); // {label, value}
    combo->set_selected_value("dark");

    REQUIRE(combo->access_default_action()); // opens the dropdown
    REQUIRE(combo->is_expanded());
    REQUIRE(host.popups_created.size() == 1);

    tk::AccessNode tree =
        tk::build_access_tree(host.popups_created[0]->root());
    CHECK(row_names(tree, tk::Role::ListItem) ==
          std::vector<std::string>{"Light", "Dark"});

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
    REQUIRE(list->children.size() == 2);
    CHECK(list->children[1].state.selected); // "Dark" is the committed value
}

TEST_CASE("ConfirmDialog / AlertDialog report a Dialog role named for their "
         "title + body",
         "[dialog][accessibility]")
{
    PopupCapableStubHost host;

    auto confirm = tk::create_root_widget<tesseract::views::ConfirmDialog>(&host);
    confirm->open({"Leave room?", "You can rejoin later.", "Leave", "Cancel",
                   true},
                  [] {});
    CHECK(confirm->access_role() == tk::Role::Dialog);
    CHECK(confirm->access_name() == "Leave room?. You can rejoin later.");

    auto alert = tk::create_root_widget<tesseract::views::AlertDialog>(&host);
    alert->open({"Upload failed", "The file is too large.", "OK", ""}, [] {});
    CHECK(alert->access_role() == tk::Role::Dialog);
    CHECK(alert->access_name() == "Upload failed. The file is too large.");
}
