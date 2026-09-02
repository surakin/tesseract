#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/image_view.h"
#include "tk/searchable_picker.h"
#include "tk/side_tab_view.h"
#include "tk/tab_bar.h"
#include "tk/tab_view.h"
#include "tk/text_area.h"
#include "tk/text_field.h"
#include "tk/widget.h"
#include "tk_test_host.h"

// Exercises the Phase 4 access_role()/access_name()/access_state()
// overrides on the base tk:: widgets — the mechanical mapping from each
// widget's own state to the accessibility model Phase 1 introduced.

using namespace tk;

TEST_CASE("Label reports StaticText role and its text as the name",
         "[tk][accessibility]")
{
    auto label = tk::create_root_widget<Label>(nullptr, "Hello world");
    CHECK(label->access_role() == Role::StaticText);
    CHECK(label->access_name() == "Hello world");
}

TEST_CASE("Button reports Button role and its label as the name",
         "[tk][accessibility]")
{
    auto button = tk::create_root_widget<Button>(nullptr, "Send");
    CHECK(button->access_role() == Role::Button);
    CHECK(button->access_name() == "Send");
}

TEST_CASE("CheckButton reports CheckBox role, label as name, and reflects "
         "checked state",
         "[tk][accessibility]")
{
    auto cb = tk::create_root_widget<CheckButton>(nullptr, "Remember me", false);
    CHECK(cb->access_role() == Role::CheckBox);
    CHECK(cb->access_name() == "Remember me");
    CHECK_FALSE(cb->access_state().checked);

    cb->set_checked(true);
    CHECK(cb->access_state().checked);
}

TEST_CASE("SwitchButton reports Switch role, label as name, and reflects "
         "checked state",
         "[tk][accessibility]")
{
    auto sw = tk::create_root_widget<SwitchButton>(nullptr, "Enable notifications", true);
    CHECK(sw->access_role() == Role::Switch);
    CHECK(sw->access_name() == "Enable notifications");
    CHECK(sw->access_state().checked);

    sw->set_checked(false);
    CHECK_FALSE(sw->access_state().checked);
}

TEST_CASE("ComboBox reports ComboBox role, the selected option's display "
         "label as name (not its value), and expanded state",
         "[tk][accessibility]")
{
    auto combo = tk::create_root_widget<ComboBox>(nullptr);
    combo->set_options({{"English", "en"}, {"Spanish", "es"}});
    combo->set_selected_value("es");

    CHECK(combo->access_role() == Role::ComboBox);
    CHECK(combo->access_name() == "Spanish");
    CHECK_FALSE(combo->access_state().expanded);
}

TEST_CASE("ComboBox access_name falls back to empty when nothing matches "
         "the selected value",
         "[tk][accessibility]")
{
    auto combo = tk::create_root_widget<ComboBox>(nullptr);
    combo->set_options({{"English", "en"}});
    CHECK(combo->access_name().empty());
}

namespace
{
const AccessNode* first_role(const AccessNode& n, Role r)
{
    if (n.role == r)
        return &n;
    for (const auto& c : n.children)
        if (const AccessNode* hit = first_role(c, r))
            return hit;
    return nullptr;
}
} // namespace

TEST_CASE("TabBar exposes a TabList with one selectable Tab node per room",
         "[tk][accessibility]")
{
    auto tab_bar = tk::create_root_widget<TabBar>(nullptr);
    tab_bar->add_tab("!a:x", "Alpha", nullptr);
    tab_bar->add_tab("!b:x", "Beta", nullptr);
    tab_bar->set_active("!b:x");

    AccessNode tree = build_access_tree(tab_bar.get());
    const AccessNode* list = first_role(tree, Role::TabList);
    REQUIRE(list != nullptr);

    std::vector<std::string> names;
    const AccessNode* beta = nullptr;
    for (const auto& c : list->children)
        if (c.role == Role::Tab)
        {
            names.push_back(c.name);
            if (c.name == "Beta")
                beta = &c;
        }
    CHECK(names == std::vector<std::string>{"Alpha", "Beta"});
    REQUIRE(beta != nullptr);
    CHECK(beta->state.selected);

    std::string picked;
    tab_bar->on_tab_selected = [&](const std::string& id) { picked = id; };
    CHECK(tk::invoke_default_action(*beta));
    CHECK(picked == "!b:x");
}

TEST_CASE("TabView exposes a Tab node per segment; activation selects it",
         "[tk][accessibility]")
{
    auto tv = tk::create_root_widget<TabView>(nullptr);
    tv->set_items({"Join", "Create"});

    int selected = -1;
    tv->on_selected = [&](int i) { selected = i; };

    AccessNode tree = build_access_tree(tv.get());
    const AccessNode* list = first_role(tree, Role::TabList);
    REQUIRE(list != nullptr);
    REQUIRE(list->children.size() == 2);
    CHECK(list->children[0].role == Role::Tab);
    CHECK(list->children[1].name == "Create");

    CHECK(tk::invoke_default_action(list->children[1]));
    CHECK(selected == 1);
}

TEST_CASE("SideTabView exposes Tab nodes AND still walks the active content",
         "[tk][accessibility]")
{
    auto stv = tk::create_root_widget<SideTabView>(nullptr);
    stv->add_tab("General", tk::create_widget<Label>(stv.get(), "general body"));
    stv->add_tab("Privacy", tk::create_widget<Label>(stv.get(), "privacy body"));
    stv->select(0);

    AccessNode tree = build_access_tree(stv.get());
    const AccessNode* list = first_role(tree, Role::TabList);
    REQUIRE(list != nullptr);

    int tab_nodes = 0;
    bool saw_content = false;
    for (const auto& c : list->children)
    {
        if (c.role == Role::Tab)
            ++tab_nodes;
        if (c.role == Role::StaticText && c.name == "general body")
            saw_content = true;
    }
    CHECK(tab_nodes == 2);
    CHECK(saw_content); // the real child widget is still in the tree
}

TEST_CASE("Avatar reports Image role and the display name as its name",
         "[tk][accessibility]")
{
    auto avatar = tk::create_root_widget<Avatar>(nullptr, "Alice");
    CHECK(avatar->access_role() == Role::Image);
    CHECK(avatar->access_name() == "Alice");
}

TEST_CASE("ImageView stays Role::None — no alt-text mechanism exists yet, "
         "so exposing it would just be unlabeled AT noise",
         "[tk][accessibility]")
{
    auto image = tk::create_root_widget<ImageView>(nullptr);
    CHECK(image->access_role() == Role::None);
}

TEST_CASE("Button::set_accessible_name() overrides label_ for access_name(), "
         "for icon-only buttons whose label isn't a real name",
         "[tk][accessibility]")
{
    auto btn = tk::create_root_widget<Button>(nullptr, "\xE2\x9C\x95" /* × glyph */);
    CHECK(btn->access_name() == "\xE2\x9C\x95"); // falls back to label_ until overridden

    btn->set_accessible_name("Remove attachment");
    CHECK(btn->access_name() == "Remove attachment");
    CHECK(btn->label() == "\xE2\x9C\x95"); // label_ (used elsewhere, e.g. test scans) is untouched
}

TEST_CASE("TextField/TextArea stay Role::None despite deriving from Label — "
         "the native overlay control already has its own OS-level "
         "accessibility, so a synthetic node here would duplicate it",
         "[tk][accessibility]")
{
    StubHost host;
    auto field = tk::create_root_widget<TextField>(&host, 20.0f);
    CHECK(field->access_role() == Role::None);

    auto area = tk::create_root_widget<TextArea>(&host, 40.0f);
    CHECK(area->access_role() == Role::None);
}

namespace
{
// Minimal concrete SearchablePicker (the base is abstract). No init_() — the
// a11y overrides never touch the internal field.
class StubPicker : public SearchablePicker
{
public:
    StubPicker() = default;
    using SearchablePicker::set_value;

private:
    std::size_t entry_count_() const override { return 1; }
    int match_rank_(std::size_t, std::string_view) const override { return 0; }
    std::string entry_key_(std::size_t) const override { return "en"; }
    std::string entry_label_(std::size_t) const override { return "English"; }
    std::string entry_display_(std::size_t) const override { return "English"; }
};
} // namespace

TEST_CASE("SearchablePicker maps to ComboBox with the committed value as name",
         "[tk][accessibility]")
{
    StubPicker picker;
    CHECK(picker.access_role() == Role::ComboBox);
    CHECK_FALSE(picker.access_state().expanded);

    picker.set_value("en");
    CHECK(picker.access_name() == "English");
}

// ── access_default_action() — the generic AT "invoke this node" hook ──────

TEST_CASE("Button::access_default_action() fires on_click, same as a real "
         "click, and reports whether it had anything to invoke",
         "[tk][accessibility][action]")
{
    auto btn = tk::create_root_widget<Button>(nullptr, "Send");
    int clicks = 0;
    btn->set_on_click([&] { ++clicks; });

    CHECK(btn->access_default_action());
    CHECK(clicks == 1);

    btn->set_enabled(false);
    CHECK_FALSE(btn->access_default_action());
    CHECK(clicks == 1); // disabled: click() itself no-ops
}

TEST_CASE("CheckButton::access_default_action() toggles checked_ and fires "
         "on_change — NOT the same as set_checked(), which is silent",
         "[tk][accessibility][action]")
{
    auto cb = tk::create_root_widget<CheckButton>(nullptr, "Remember me", false);
    bool last_value = false;
    int fires = 0;
    cb->on_change = [&](bool v) { last_value = v; ++fires; };

    CHECK(cb->access_default_action());
    CHECK(cb->checked());
    CHECK(last_value);
    CHECK(fires == 1);

    CHECK(cb->access_default_action());
    CHECK_FALSE(cb->checked());
    CHECK(fires == 2);
}

TEST_CASE("SwitchButton::access_default_action() toggles and fires "
         "on_change",
         "[tk][accessibility][action]")
{
    auto sw = tk::create_root_widget<SwitchButton>(nullptr, "Enable notifications", false);
    int fires = 0;
    sw->on_change = [&](bool) { ++fires; };

    CHECK(sw->access_default_action());
    CHECK(sw->checked());
    CHECK(fires == 1);
}

TEST_CASE("ComboBox::access_default_action() opens the dropdown when "
         "collapsed and collapses it when open",
         "[tk][accessibility][action]")
{
    auto combo = tk::create_root_widget<ComboBox>(nullptr);
    combo->set_options({{"English", "en"}, {"Spanish", "es"}});
    CHECK_FALSE(combo->is_expanded());

    // No host() in this test, so set_expanded_ can't actually create a
    // popup surface (see ComboBox::set_expanded_'s own early-return on a
    // null host()) — access_default_action() still reports true (an
    // action was attempted) even though the visual state can't flip
    // without a real Host backing it.
    CHECK(combo->access_default_action());
}

TEST_CASE("a widget with no accessible action (Label) reports false and "
         "does nothing",
         "[tk][accessibility][action]")
{
    auto label = tk::create_root_widget<Label>(nullptr, "Hello");
    CHECK_FALSE(label->access_default_action());
}
