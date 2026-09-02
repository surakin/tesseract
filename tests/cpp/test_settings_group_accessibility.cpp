#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/controls.h"
#include "views/settings/SettingsGroup.h"

// SettingsGroup paints its section header on the canvas (not a child Label),
// so without an explicit mapping a screen reader would announce the controls
// inside it with no idea which section they belong to.

using tesseract::views::SettingsGroup;

TEST_CASE("SettingsGroup is a named Group so its header is announced",
         "[settings][accessibility]")
{
    SettingsGroup group("Notifications");
    CHECK(group.access_role() == tk::Role::Group);
    CHECK(group.access_name() == "Notifications");
}

TEST_CASE("SettingsGroup's controls attach under its Group node",
         "[settings][accessibility]")
{
    SettingsGroup group("Privacy");
    group.add_widget(
        tk::create_widget<tk::CheckButton>(&group, "Share presence", true));

    tk::AccessNode tree = tk::build_access_tree(&group);
    CHECK(tree.role == tk::Role::Group);
    CHECK(tree.name == "Privacy");
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].role == tk::Role::CheckBox);
    CHECK(tree.children[0].name == "Share presence");
}
