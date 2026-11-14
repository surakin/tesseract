#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/combobox.h"
#include "tk_test_host.h"
#include "views/AlertDialog.h"
#include "views/BusyDialog.h"
#include "views/ConfirmDialog.h"
#include "view_test_util.h"
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
        {
            out.push_back(x.name);
        }
        for (const auto& c : x.children)
        {
            walk(c);
        }
    };
    walk(n);
    return out;
}
} // namespace

TEST_CASE("PopupMenu items are MenuItem nodes; separators omitted, disabled reported",
          "[popup_menu][accessibility]")
{
    PopupCapableStubHost host;
    auto menu = tk::create_root_widget<PopupMenu>(&host);

    int reacted = 0;
    std::vector<PopupMenu::Item> items;
    items.push_back({{},
                     {},
                     "React",
                     false,
                     [&]
                     {
                         ++reacted;
                     },
                     false,
                     true});
    items.push_back({{}, {}, "", false, {}, /*is_separator=*/true, true});
    items.push_back({{}, {}, "Delete", true, {}, false, /*enabled=*/false});
    items.push_back({{}, {}, "Reply", false, [] {}, false, true});
    menu->open(std::move(items), {});

    REQUIRE(host.popups_created.size() == 1);
    tk::Widget* root = host.popups_created[0]->root();
    REQUIRE(root != nullptr);

    tk::AccessNode tree = tk::build_access_tree(root);
    CHECK(row_names(tree, tk::Role::MenuItem) ==
          std::vector<std::string>{"React", "Delete", "Reply"});

    // Invoke the "React" node → fires its on_selected (and on_dismissed).
    const tk::AccessNode* list = nullptr;
    std::function<void(const tk::AccessNode&)> find = [&](const tk::AccessNode& n)
    {
        if (n.role == tk::Role::List)
        {
            list = &n;
        }
        for (const auto& c : n.children)
        {
            find(c);
        }
    };
    find(tree);
    REQUIRE(list != nullptr);
    REQUIRE_FALSE(list->children.empty());
    CHECK(list->children[1].state.disabled); // "Delete"
    CHECK_FALSE(tk::invoke_default_action(list->children[1]));
    CHECK(tk::invoke_default_action(list->children[0]));
    CHECK(reacted == 1);
}

TEST_CASE("PopupMenu is keyboard-driven: arrows skip separators/disabled, Enter "
          "activates, Escape dismisses",
          "[popup_menu][accessibility]")
{
    PopupCapableStubHost host;
    auto menu = tk::create_root_widget<PopupMenu>(&host);
    std::string picked;
    int dismissed = 0;
    menu->on_dismissed = [&]
    {
        ++dismissed;
        menu->close();
    };
    std::vector<PopupMenu::Item> items;
    items.push_back({{},
                     {},
                     "React",
                     false,
                     [&]
                     {
                         picked = "React";
                     },
                     false,
                     true});
    items.push_back({{}, {}, "", false, {}, /*is_separator=*/true, true});
    items.push_back({{},
                     {},
                     "Delete",
                     true,
                     [&]
                     {
                         picked = "Delete";
                     },
                     false,
                     false});
    items.push_back({{},
                     {},
                     "Reply",
                     false,
                     [&]
                     {
                         picked = "Reply";
                     },
                     false,
                     true});
    menu->open(std::move(items), {});

    auto key = [&](tk::Key k)
    {
        tk::KeyEvent e;
        e.key = k;
        return menu->on_key_down(e);
    };
    auto selected = [&]
    {
        tk::AccessNode tree = tk::build_access_tree(host.popups_created[0]->root());
        for (const auto& n : tree.children)
        {
            if (n.state.selected)
            {
                return n.name;
            }
        }
        return std::string();
    };

    CHECK(key(tk::Key::Down));
    CHECK(selected() == "React");
    CHECK(key(tk::Key::Down)); // skips the separator and disabled "Delete"
    CHECK(selected() == "Reply");
    CHECK(key(tk::Key::Down)); // wraps
    CHECK(selected() == "React");
    CHECK(key(tk::Key::End));
    CHECK(selected() == "Reply");
    CHECK(key(tk::Key::Enter));
    CHECK(picked == "Reply");
    CHECK(dismissed == 1);

    std::vector<PopupMenu::Item> again;
    again.push_back({{}, {}, "React", false, [] {}, false, true});
    menu->open(std::move(again), {});
    CHECK(key(tk::Key::Escape));
    CHECK(dismissed == 2);
    CHECK_FALSE(menu->is_open());
}

TEST_CASE("ComboBox dropdown options are selectable ListItem nodes", "[combobox][accessibility]")
{
    PopupCapableStubHost host;
    auto combo = tk::create_root_widget<tk::ComboBox>(&host);
    combo->set_options({{"Light", "light"}, {"Dark", "dark"}}); // {label, value}
    combo->set_selected_value("dark");

    REQUIRE(combo->access_default_action()); // opens the dropdown
    REQUIRE(combo->is_expanded());
    REQUIRE(host.popups_created.size() == 1);

    tk::AccessNode tree = tk::build_access_tree(host.popups_created[0]->root());
    CHECK(row_names(tree, tk::Role::ListItem) == std::vector<std::string>{"Light", "Dark"});

    const tk::AccessNode* list = nullptr;
    std::function<void(const tk::AccessNode&)> find = [&](const tk::AccessNode& n)
    {
        if (n.role == tk::Role::List)
        {
            list = &n;
        }
        for (const auto& c : n.children)
        {
            find(c);
        }
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
    confirm->open({"Leave room?", "You can rejoin later.", "Leave", "Cancel", true}, [] {});
    CHECK(confirm->access_role() == tk::Role::Dialog);
    CHECK(confirm->access_name() == "Leave room?");
    CHECK(confirm->access_description() == "You can rejoin later.");
    CHECK(confirm->access_modal());

    auto alert = tk::create_root_widget<tesseract::views::AlertDialog>(&host);
    alert->open({"Upload failed", "The file is too large.", "OK", ""}, [] {});
    CHECK(alert->access_role() == tk::Role::Dialog);
    CHECK(alert->access_name() == "Upload failed");
    CHECK(alert->access_description() == "The file is too large.");
}

TEST_CASE("BusyDialog opens and closes, consumes pointer and keys, and is modal",
          "[dialog][accessibility]")
{
    PopupCapableStubHost host;
    auto busy = tk::create_root_widget<tesseract::views::BusyDialog>(&host);
    int layout_changes = 0;
    busy->on_layout_changed = [&]
    {
        ++layout_changes;
    };

    CHECK_FALSE(busy->is_open());
    CHECK_FALSE(busy->on_pointer_down({1, 1}));

    busy->open("Applying", "Please wait");
    CHECK(busy->is_open());
    CHECK(layout_changes == 1);
    CHECK(busy->access_role() == tk::Role::Dialog);
    CHECK(busy->access_name() == "Applying");
    CHECK(busy->access_description() == "Please wait");
    CHECK(busy->access_modal());
    CHECK(busy->on_pointer_down({1, 1}));

    tk::KeyEvent esc;
    esc.key = tk::Key::Escape;
    CHECK(busy->on_key_down(esc));
    CHECK(busy->dispatch_key_down(esc));
    CHECK(busy->is_open());

    busy->close();
    CHECK_FALSE(busy->is_open());
    CHECK(layout_changes == 2);
}

TEST_CASE("PopupMenu widens to fit a long label and left-anchors a pointer anchor", "[popup_menu]")
{
    vt::Stage stage;
    auto menu = tk::create_root_widget<PopupMenu>(&stage.host);

    auto open_with = [&](const std::string& label, tk::Rect anchor)
    {
        std::vector<PopupMenu::Item> items;
        items.push_back({{}, {}, label, false, {}, false, true});
        menu->open(std::move(items), anchor);
        stage.layout(*menu, {0, 0, 800, 600});
        REQUIRE(!stage.host.popups_created.empty());
        return stage.host.popups_created.back();
    };

    auto* shortp = open_with("Reply", {300, 100, 0, 0});
    CHECK(shortp->last_size.w == PopupMenu::kWidth);
    CHECK(shortp->last_anchor.x == 300.0f);

    menu->close();
    auto* longp = open_with("Dies ist ein sehr langer uebersetzter Menueeintrag fuer den Test",
                            {300, 100, 0, 0});
    CHECK(longp->last_size.w > PopupMenu::kWidth);
    CHECK(longp->last_size.w <= PopupMenu::kMaxWidth);
    CHECK(longp->last_anchor.x == 300.0f);

    // A real (non-zero) anchor still right-aligns to its right edge.
    menu->close();
    auto* right = open_with("Reply", {100, 50, 40, 20});
    CHECK(right->last_anchor.x + right->last_size.w == 140.0f);
}
