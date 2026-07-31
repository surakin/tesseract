#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <memory>
#include <vector>

// Exercises build_access_tree()'s core contract: Role::None widgets flatten
// through (contribute no node of their own, but their accessible
// descendants still attach to the nearest accessible ancestor), invisible
// subtrees are skipped entirely, and children are ordered in reading order
// — matching next_focusable()'s traversal so Tab order and AT reading order
// can't silently diverge.

using namespace tk;

namespace
{

// A configurable probe: Role::None by default (a plain structural
// container, like VBox/Stack), or an explicit role/name/state when a test
// needs an accessible leaf. bounds_ is set directly in the constructor
// (mirrors FocusProbeWidget in test_tk_host_focus.cpp) since these tests
// only need reading_order_less's inputs, not real layout.
class AccessProbe : public Widget
{
public:
    explicit AccessProbe(Rect rect, Role role = Role::None,
                         std::string name = {}, AccessState state = {})
        : role_(role), name_(std::move(name)), state_(state)
    {
        bounds_ = rect;
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}

    Role access_role() const override
    {
        return role_;
    }
    std::string access_name() const override
    {
        return name_;
    }
    AccessState access_state() const override
    {
        return state_;
    }

private:
    Role role_;
    std::string name_;
    AccessState state_;
};

} // namespace

TEST_CASE("build_access_tree excludes Role::None widgets by default",
         "[tk][access_tree]")
{
    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});

    AccessNode tree = build_access_tree(root.get());
    CHECK(tree.role == Role::None);
    CHECK(tree.children.empty());
}

TEST_CASE("a widget with an explicit role produces a node with its name/state",
         "[tk][access_tree]")
{
    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});
    AccessState checked;
    checked.checked = true;
    root->add_child(std::make_unique<AccessProbe>(
        Rect{10, 10, 20, 20}, Role::CheckBox, "Remember me", checked));

    AccessNode tree = build_access_tree(root.get());
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].role == Role::CheckBox);
    CHECK(tree.children[0].name == "Remember me");
    CHECK(tree.children[0].state.checked);
}

TEST_CASE("Role::None widgets flatten through, attaching their accessible "
         "descendants to the nearest accessible ancestor",
         "[tk][access_tree]")
{
    // root (None) -> group (None, e.g. a plain VBox) -> button (Button)
    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});
    auto* group = root->add_child(
        std::make_unique<AccessProbe>(Rect{0, 0, 100, 50}));
    group->add_child(std::make_unique<AccessProbe>(
        Rect{10, 10, 20, 20}, Role::Button, "OK"));

    AccessNode tree = build_access_tree(root.get());
    // The intermediate structural group contributed no node of its own —
    // the button attaches directly to the root.
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].role == Role::Button);
    CHECK(tree.children[0].name == "OK");
}

TEST_CASE("invisible subtrees are skipped entirely, even if they contain "
         "accessible widgets",
         "[tk][access_tree]")
{
    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});
    auto* hidden = root->add_child(std::make_unique<AccessProbe>(
        Rect{0, 0, 100, 50}, Role::Group, "Hidden group"));
    hidden->set_visible(false);
    hidden->add_child(std::make_unique<AccessProbe>(
        Rect{10, 10, 20, 20}, Role::Button, "Unreachable"));

    root->add_child(std::make_unique<AccessProbe>(
        Rect{0, 50, 100, 20}, Role::Button, "Visible"));

    AccessNode tree = build_access_tree(root.get());
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].name == "Visible");
}

TEST_CASE("children are ordered in reading order, top-to-bottom then "
         "left-to-right — not insertion order",
         "[tk][access_tree]")
{
    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});
    // Added out of visual order: bottom-row-right, top-row-left,
    // bottom-row-left, top-row-right.
    root->add_child(std::make_unique<AccessProbe>(
        Rect{60, 60, 20, 20}, Role::Button, "bottom-right"));
    root->add_child(std::make_unique<AccessProbe>(
        Rect{0, 0, 20, 20}, Role::Button, "top-left"));
    root->add_child(std::make_unique<AccessProbe>(
        Rect{0, 60, 20, 20}, Role::Button, "bottom-left"));
    root->add_child(std::make_unique<AccessProbe>(
        Rect{60, 0, 20, 20}, Role::Button, "top-right"));

    AccessNode tree = build_access_tree(root.get());
    REQUIRE(tree.children.size() == 4);
    CHECK(tree.children[0].name == "top-left");
    CHECK(tree.children[1].name == "top-right");
    CHECK(tree.children[2].name == "bottom-left");
    CHECK(tree.children[3].name == "bottom-right");
}

TEST_CASE("a null or invisible root produces an empty tree",
         "[tk][access_tree]")
{
    CHECK(build_access_tree(nullptr).role == Role::None);

    auto root = tk::create_root_widget<AccessProbe>(nullptr, Rect{0, 0, 100, 100});
    root->set_visible(false);
    AccessNode tree = build_access_tree(root.get());
    CHECK(tree.widget == nullptr);
}

namespace
{

// A minimal ListAdapter that also implements ListAdapterAccessibility, to
// exercise the "rows have no per-row Widget" synthesis path in
// collect_list_rows (access_tree.cpp) — a real-world adapter (e.g.
// RoomListView::Adapter) works the same way, just with real row content.
class FakeRowAdapter : public ListAdapter, public ListAdapterAccessibility
{
public:
    explicit FakeRowAdapter(std::vector<std::string> names) : names_(std::move(names)) {}

    std::size_t count() const override
    {
        return names_.size();
    }
    float measure_row_height(std::size_t, LayoutCtx&, float) override
    {
        return 20.0f;
    }
    void paint_row(std::size_t, PaintCtx&, Rect, bool, bool) override {}

    Role access_role_for_row(std::size_t) const override
    {
        return Role::ListItem;
    }
    std::string access_name_for_row(std::size_t index) const override
    {
        return names_[index];
    }

private:
    std::vector<std::string> names_;
};

} // namespace

TEST_CASE("a ListView whose adapter implements ListAdapterAccessibility "
         "gets synthesized ListItem children, one per row",
         "[tk][access_tree]")
{
    FakeRowAdapter adapter({"Alice", "Bob", "Carol"});
    auto list = tk::create_root_widget<ListView>(nullptr);
    list->set_adapter(&adapter);

    AccessNode tree = build_access_tree(list.get());
    REQUIRE(tree.role == Role::List);
    REQUIRE(tree.children.size() == 3);

    CHECK(tree.children[0].role == Role::ListItem);
    CHECK(tree.children[0].name == "Alice");
    CHECK(tree.children[0].row_index == 0);
    CHECK(tree.children[0].row_set_size == 3);

    CHECK(tree.children[1].name == "Bob");
    CHECK(tree.children[1].row_index == 1);

    CHECK(tree.children[2].name == "Carol");
    CHECK(tree.children[2].row_index == 2);
}

TEST_CASE("a ListView whose adapter does NOT implement "
         "ListAdapterAccessibility produces no children (not a crash)",
         "[tk][access_tree]")
{
    class PlainAdapter : public ListAdapter
    {
    public:
        std::size_t count() const override
        {
            return 5;
        }
        float measure_row_height(std::size_t, LayoutCtx&, float) override
        {
            return 20.0f;
        }
        void paint_row(std::size_t, PaintCtx&, Rect, bool, bool) override {}
    };

    PlainAdapter adapter;
    auto list = tk::create_root_widget<ListView>(nullptr);
    list->set_adapter(&adapter);

    AccessNode tree = build_access_tree(list.get());
    CHECK(tree.role == Role::List);
    CHECK(tree.children.empty());
}
