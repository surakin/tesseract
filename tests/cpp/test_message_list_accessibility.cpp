#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/MessageListView.h"

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using namespace tk;

// Exercises MessageListView::Adapter's tk::ListAdapterAccessibility
// implementation — the "flattened bubble" mapping (one accessible name per
// message row combining sender + content description + a reaction count
// summary) that was explicitly chosen over a richer per-reaction subtree,
// since the mechanism only supports one role/name/state per row for now.

namespace
{
MessageRowData text_row(std::string id, std::string sender_name, std::string body)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = std::move(id);
    r.sender_name = std::move(sender_name);
    r.body = std::move(body);
    return r;
}

MessageRowData day_separator_row(std::uint64_t ts)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::DaySeparator;
    r.timestamp_ms = ts;
    return r;
}

const AccessNode* find_role_msg(const AccessNode& node, Role role)
{
    if (node.role == role)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_role_msg(ch, role))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("a text message row is a ListItem named \"{sender}: {body}\"",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(text_row("$a", "Alice", "on my way, 5 minutes"));
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role_msg(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* row = nullptr;
    for (const auto& ch : list->children)
        if (ch.role == Role::ListItem)
        {
            row = &ch;
            break;
        }
    REQUIRE(row != nullptr);
    CHECK(row->name == "Alice: on my way, 5 minutes");
}

TEST_CASE("a message row with reactions folds a reaction-count summary "
         "into its name (flattened bubble, not individually actionable)",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    auto row = text_row("$a", "Alice", "nice!");
    tesseract::Reaction r1;
    r1.key   = "\U0001F44D";
    r1.count = 2;
    tesseract::Reaction r2;
    r2.key   = "\U0001F389";
    r2.count = 1;
    row.reactions = {r1, r2};
    msgs.push_back(row);
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role_msg(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* item = nullptr;
    for (const auto& ch : list->children)
        if (ch.role == Role::ListItem)
        {
            item = &ch;
            break;
        }
    REQUIRE(item != nullptr);
    CHECK(item->name == "Alice: nice! (3 reactions)");
}

TEST_CASE("a redacted message reads as \"Message deleted\", not empty body",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    auto row = text_row("$a", "Alice", "");
    row.kind = MessageRowData::Kind::Redacted;
    msgs.push_back(row);
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role_msg(tree, Role::List);
    REQUIRE(list != nullptr);
    const AccessNode* item = nullptr;
    for (const auto& ch : list->children)
        if (ch.role == Role::ListItem)
        {
            item = &ch;
            break;
        }
    REQUIRE(item != nullptr);
    CHECK(item->name == "Alice: Message deleted");
}

TEST_CASE("a day separator with no content after it is excluded entirely, "
         "matching paint_row's own suppression",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(day_separator_row(1000));
    // No real content row follows — has_content_before_next_separator()
    // is false, so paint_row never draws this separator either.
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role_msg(tree, Role::List);
    REQUIRE(list != nullptr);
    // Both rows are excluded: the separator (suppressed) and the
    // always-present typing row (empty typing_text_ by default) both
    // return an empty access_name, which access_role_for_row maps to
    // Role::None — contributing no node at all, not merely a
    // non-StaticText one.
    CHECK(list->children.empty());
}

TEST_CASE("a day separator followed by real content is announced with the "
         "formatted date",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(day_separator_row(1'700'000'000'000ULL));
    msgs.push_back(text_row("$a", "Alice", "hello"));
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    const AccessNode* list = find_role_msg(tree, Role::List);
    REQUIRE(list != nullptr);

    const AccessNode* separator = nullptr;
    for (const auto& ch : list->children)
        if (ch.role == Role::StaticText)
        {
            separator = &ch;
            break;
        }
    REQUIRE(separator != nullptr);
    CHECK_FALSE(separator->name.empty());
}
