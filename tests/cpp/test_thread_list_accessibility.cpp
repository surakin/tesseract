#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/ThreadListView.h"

#include <algorithm>

using tesseract::ThreadInfo;
using tesseract::views::ThreadListView;
using namespace tk;

// Exercises ThreadListView::Adapter's tk::ListAdapterAccessibility
// implementation — mirrors test_room_list_accessibility.cpp's pattern for
// another real ListAdapter consumer of the same generic mechanism.

namespace
{
ThreadInfo make_thread(std::string root_sender, std::string root_body,
                      std::uint64_t replies)
{
    ThreadInfo t;
    t.root_sender_name = std::move(root_sender);
    t.root_body = std::move(root_body);
    t.num_replies = replies;
    t.root_timestamp = 1000;
    return t;
}

const AccessNode* find_role_thread(const AccessNode& node, Role role)
{
    if (node.role == role)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_role_thread(ch, role))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("ThreadListView's header spacer row is excluded (Role::None), "
         "only real threads become ListItems",
         "[thread_list][accessibility]")
{
    ThreadListView view;
    std::vector<ThreadInfo> threads;
    threads.push_back(make_thread("Alice", "Hello world", 3));
    view.set_threads(std::move(threads));

    AccessNode tree = build_access_tree(&view);
    const AccessNode* list = find_role_thread(tree, Role::List);
    REQUIRE(list != nullptr);

    int list_item_count = 0;
    for (const auto& ch : list->children)
    {
        CHECK(ch.role != Role::None);
        if (ch.role == Role::ListItem)
            ++list_item_count;
    }
    CHECK(list_item_count == 1);
}

TEST_CASE("a thread row's accessible name combines sender, body, and a "
         "localized reply-count summary",
         "[thread_list][accessibility]")
{
    ThreadListView view;
    std::vector<ThreadInfo> threads;
    threads.push_back(make_thread("Alice", "Hello world", 1));
    threads.push_back(make_thread("Bob", "Second thread", 5));
    view.set_threads(std::move(threads));

    AccessNode tree = build_access_tree(&view);
    const AccessNode* list = find_role_thread(tree, Role::List);
    REQUIRE(list != nullptr);

    std::vector<std::string> names;
    for (const auto& ch : list->children)
        if (ch.role == Role::ListItem)
            names.push_back(ch.name);
    REQUIRE(names.size() == 2);

    // Singular vs. plural reply count, matching tk::trn's English default.
    CHECK(std::find(names.begin(), names.end(), "Alice: Hello world (1 reply)") !=
         names.end());
    CHECK(std::find(names.begin(), names.end(), "Bob: Second thread (5 replies)") !=
         names.end());
}

TEST_CASE("invoking a thread row's default action fires on_thread_clicked "
         "with that thread's root_event_id, the same as a real click",
         "[thread_list][accessibility][action]")
{
    ThreadListView view;
    std::vector<ThreadInfo> threads;
    ThreadInfo t = make_thread("Alice", "Hello world", 1);
    t.root_event_id = "$root1";
    threads.push_back(t);
    view.set_threads(std::move(threads));

    std::string clicked;
    view.on_thread_clicked = [&](const std::string& id) { clicked = id; };

    AccessNode tree = build_access_tree(&view);
    const AccessNode* list = find_role_thread(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* row = nullptr;
    for (const auto& ch : list->children)
        if (ch.role == Role::ListItem)
            row = &ch;
    REQUIRE(row != nullptr);

    CHECK(invoke_default_action(*row));
    CHECK(clicked == "$root1");
}
