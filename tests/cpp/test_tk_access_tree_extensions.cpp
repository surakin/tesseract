#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "access_test_util.h"
#include "tk/controls.h"
#include "tk/list_view.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// The access-tree extensions added for the 2026-10 accessibility gap pass:
// detached (register_popup'd) children, modal pruning, description/value,
// 2-D grid positions, and Host::announce routing.

using namespace tk;

namespace access_ext_test
{

class AccessExtProbe : public Widget
{
public:
    explicit AccessExtProbe(Rect rect, Role role = Role::None, std::string name = {})
        : role_(role), name_(std::move(name))
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override
    {
    }

    Role access_role() const override
    {
        return role_;
    }
    std::string access_name() const override
    {
        return name_;
    }
    std::string access_description() const override
    {
        return description;
    }
    bool access_modal() const override
    {
        return modal;
    }
    void access_detached_children(std::vector<Widget*>& out) const override
    {
        for (Widget* w : detached)
        {
            out.push_back(w);
        }
    }

    std::string description;
    bool modal = false;
    std::vector<Widget*> detached;

private:
    Role role_;
    std::string name_;
};

class AccessExtCellAdapter : public GridAdapter, public GridAdapterAccessibility
{
public:
    std::size_t n = 12;
    std::size_t count() const override
    {
        return n;
    }
    void paint_cell(std::size_t, PaintCtx&, Rect, bool, bool) override
    {
    }
    Role access_role_for_cell(std::size_t) const override
    {
        return Role::GridCell;
    }
    std::string access_name_for_cell(std::size_t i) const override
    {
        return "cell " + std::to_string(i);
    }
};

class AccessExtAnnounceHost : public TestHost
{
public:
    using TestHost::TestHost;
    std::vector<std::pair<std::string, Politeness>> announced;

protected:
    void on_announce_(const std::string& text, Politeness p) override
    {
        announced.emplace_back(text, p);
    }
};

} // namespace access_ext_test

using access_ext_test::AccessExtProbe;
using access_ext_test::AccessExtCellAdapter;
using access_ext_test::AccessExtAnnounceHost;
using access_test::find_named;
using access_test::find_prefix;

TEST_CASE("detached children are walked after real children, hidden ones skipped",
          "[tk][access_tree][accessibility]")
{
    auto root = create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 200, 200});
    root->add_child(std::make_unique<AccessExtProbe>(Rect{0, 0, 10, 10}, Role::Button, "real"));
    auto popup =
        create_root_widget<AccessExtProbe>(nullptr, Rect{50, 50, 50, 50}, Role::Dialog, "popup");
    popup->add_child(
        std::make_unique<AccessExtProbe>(Rect{55, 55, 10, 10}, Role::Button, "inside"));
    auto hidden =
        create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 5, 5}, Role::Button, "hidden");
    hidden->set_visible(false);
    root->detached = {popup.get(), hidden.get()};

    AccessNode tree = build_access_tree(root.get());
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].name == "real");
    CHECK(tree.children[1].name == "popup");
    REQUIRE(tree.children[1].children.size() == 1);
    CHECK(tree.children[1].children[0].name == "inside");
    CHECK(find_named(tree, "hidden") == nullptr);
}

TEST_CASE("an open modal hides everything else under the root", "[tk][access_tree][accessibility]")
{
    auto root = create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 200, 200});
    root->add_child(std::make_unique<AccessExtProbe>(Rect{0, 0, 10, 10}, Role::Button, "behind"));
    auto* dialog = root->add_child(
        std::make_unique<AccessExtProbe>(Rect{20, 20, 100, 100}, Role::Dialog, "Leave room?"));
    dialog->add_child(
        std::make_unique<AccessExtProbe>(Rect{30, 80, 40, 20}, Role::Button, "Leave"));

    SECTION("not modal: both visible")
    {
        AccessNode tree = build_access_tree(root.get());
        CHECK(find_named(tree, "behind") != nullptr);
        CHECK(find_named(tree, "Leave room?") != nullptr);
    }
    SECTION("modal: only the dialog's subtree")
    {
        dialog->modal = true;
        AccessNode tree = build_access_tree(root.get());
        CHECK(find_named(tree, "behind") == nullptr);
        REQUIRE(tree.children.size() == 1);
        CHECK(tree.children[0].modal);
        CHECK(tree.children[0].name == "Leave room?");
        CHECK(find_named(tree, "Leave") != nullptr);
    }
    SECTION("a hidden modal doesn't prune")
    {
        dialog->modal = true;
        dialog->set_visible(false);
        AccessNode tree = build_access_tree(root.get());
        CHECK(find_named(tree, "behind") != nullptr);
    }
}

TEST_CASE("description is copied onto the node", "[tk][access_tree][accessibility]")
{
    auto root = create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 100, 100});
    auto* card = root->add_child(
        std::make_unique<AccessExtProbe>(Rect{0, 0, 50, 20}, Role::Button, "Use recovery key"));
    card->description = "Enter the key you saved";
    AccessNode tree = build_access_tree(root.get());
    const AccessNode* n = find_named(tree, "Use recovery key");
    REQUIRE(n != nullptr);
    CHECK(n->description == "Enter the key you saved");
}

TEST_CASE("ProgressBar reports a value only when determinate", "[tk][access_tree][accessibility]")
{
    auto bar = create_root_widget<ProgressBar>(nullptr);
    bar->set_label("Exporting");
    CHECK(bar->access_role() == Role::ProgressBar);
    CHECK(bar->access_name() == "Exporting");
    CHECK_FALSE(bar->access_value().present);
    CHECK(bar->access_state().busy);

    bar->set_progress(0.25f);
    AccessValue v = bar->access_value();
    CHECK(v.present);
    CHECK(v.now == 25.0);
    CHECK(v.max == 100.0);
    CHECK_FALSE(bar->access_state().busy);
}

TEST_CASE("grid cells carry 2-D positions and the grid its dimensions",
          "[tk][access_tree][gridview][accessibility]")
{
    auto surface = TestSurface::create(200, 200);
    LayoutCtx lc{surface->factory(), Theme::light()};
    auto grid = create_root_widget<GridView>(nullptr);
    grid->set_cell_size(20, 20);
    grid->set_spacing(0, 0);
    AccessExtCellAdapter ad;
    grid->set_adapter(&ad);
    grid->arrange(lc, {0, 0, 100, 100}); // 5 columns

    AccessNode tree = build_access_tree(grid.get());
    CHECK(tree.role == Role::Grid);
    CHECK(tree.grid_col_count == 5);
    CHECK(tree.grid_row_count == 3); // 12 cells / 5 columns
    REQUIRE(tree.children.size() == 12);
    CHECK(tree.children[7].grid_row == 1);
    CHECK(tree.children[7].grid_col == 2);
    CHECK(tree.children[11].grid_row == 2);
    CHECK(tree.children[11].grid_col == 1);
}

TEST_CASE("Host::announce routes to the backend and show_toast announces",
          "[tk][host][accessibility]")
{
    auto root = create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 10, 10});
    AccessExtAnnounceHost host(root.get());

    host.announce("Invite sent");
    host.announce("Failed", Host::Politeness::Assertive);
    host.announce(""); // empty is dropped
    host.show_toast("Copied to clipboard");

    REQUIRE(host.announced.size() == 3);
    CHECK(host.announced[0].first == "Invite sent");
    CHECK(host.announced[0].second == Host::Politeness::Polite);
    CHECK(host.announced[1].second == Host::Politeness::Assertive);
    CHECK(host.announced[2].first == "Copied to clipboard");
}

TEST_CASE("a Link-variant Button is an ordinary accessible button", "[tk][controls][accessibility]")
{
    auto b = create_root_widget<Button>(nullptr, "Skip for now", std::function<void()>{},
                                        Button::Variant::Link);
    CHECK(b->access_role() == Role::Button);
    CHECK(b->access_name() == "Skip for now");
}

TEST_CASE("with two dialogs open, the one painted on top is exposed",
          "[tk][access_tree][accessibility]")
{
    auto root = create_root_widget<AccessExtProbe>(nullptr, Rect{0, 0, 200, 200});
    auto* under = root->add_child(
        std::make_unique<AccessExtProbe>(Rect{0, 0, 200, 200}, Role::Dialog, "under"));
    auto* over = root->add_child(
        std::make_unique<AccessExtProbe>(Rect{20, 20, 100, 100}, Role::Dialog, "over"));
    under->modal = true;
    over->modal = true;
    AccessNode tree = build_access_tree(root.get());
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].name == "over");
}
