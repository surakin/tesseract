#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/ThreadView.h"
#include "views/UserInfo.h"

#include <string>

using tesseract::views::ThreadView;
using tesseract::views::UserInfo;
using namespace tk;

// Exercises ThreadView (flattens through — its embedded MessageListView and
// close button surface directly, per access_tree's Role::None flattening
// rule) and UserInfo (a leaf Button-role widget) — both real-view consumers
// of the Phase 4 accessibility model, mirroring the RoomListView/
// MessageListView/ThreadListView test pattern.

namespace
{
const AccessNode* find_role_tv(const AccessNode& node, Role role)
{
    if (node.role == role)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_role_tv(ch, role))
            return found;
    return nullptr;
}

const AccessNode* find_button_named_tv(const AccessNode& node, const std::string& name)
{
    if (node.role == Role::Button && node.name == name)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_button_named_tv(ch, name))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("ThreadView flattens through (Role::None), exposing its embedded "
         "MessageListView and close button directly",
         "[thread_view][accessibility]")
{
    ThreadView v;
    CHECK(v.access_role() == Role::None);

    AccessNode tree = build_access_tree(&v);
    CHECK(tree.role == Role::None); // the root itself, per build_access_tree's contract

    const AccessNode* list = find_role_tv(tree, Role::List);
    CHECK(list != nullptr); // the embedded MessageListView surfaces directly

    // Role::Button alone isn't unique here: the embedded RoomSearchBar (added
    // after this test was written) contributes its own visible prev/next
    // match buttons, so look up the close button by name specifically.
    const AccessNode* close = find_button_named_tv(tree, "Close");
    REQUIRE(close != nullptr);
}

TEST_CASE("UserInfo reports Button role, combining display name and user id",
         "[user_info][accessibility]")
{
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");

    CHECK(info.access_role() == Role::Button);
    CHECK(info.access_name() == "Alice (@alice:example.org)");
}

TEST_CASE("UserInfo falls back to whichever identity field is present when "
         "the other is empty",
         "[user_info][accessibility]")
{
    UserInfo name_only;
    name_only.set_display_name("Bob");
    CHECK(name_only.access_name() == "Bob");

    UserInfo id_only;
    id_only.set_user_id("@carol:example.org");
    CHECK(id_only.access_name() == "@carol:example.org");
}
