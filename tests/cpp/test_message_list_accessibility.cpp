#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "views/MessageListView.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "access_test_util.h"

#include <tesseract/settings.h>

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
    CHECK(item->name == "Alice: nice!, 3 reactions");
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

TEST_CASE("a message's name adds reply target, edited, and thread replies after the body",
         "[message_list][accessibility]")
{
    MessageListView v;
    auto row = text_row("$a", "Alice", "hi");
    row.in_reply_to_id          = "$o";
    row.in_reply_to_sender_name = "Bob";
    row.is_edited               = true;
    row.is_thread_root          = true;
    row.thread_reply_count      = 2;
    std::vector<MessageRowData> msgs{row};
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    CHECK(access_test::find_named(
              tree, "Alice: hi, replying to Bob, edited, 2 replies in thread") != nullptr);
}

TEST_CASE("a formatted table is summarised instead of read as Markdown source",
         "[message_list][accessibility]")
{
    MessageListView v;
    auto row = text_row("$a", "Alice", "| A | B |\n|---|---|\n| 1 | 2 |");
    row.formatted_body = "<table><thead><tr><th>A</th><th>B</th></tr></thead>"
                         "<tbody><tr><td>1</td><td>2</td></tr></tbody></table>";
    std::vector<MessageRowData> msgs{row};
    v.set_messages(std::move(msgs), false);

    AccessNode tree = build_access_tree(&v);
    CHECK(access_test::find_named(tree, "Alice: Table, 2 rows, 2 columns: A, B; 1, 2") !=
          nullptr);
}

TEST_CASE("visible messages expose their links and the reply jump as actions",
         "[message_list][accessibility]")
{
    auto surface = TestSurface::create(500, 600);
    MessageListView v;
    auto orig = text_row("$o", "Bob", "original");
    auto row  = text_row("$a", "Alice", "see site");
    row.formatted_body          = "see <a href=\"https://example.org\">site</a>";
    row.in_reply_to_id          = "$o";
    row.in_reply_to_sender_name = "Bob";
    std::vector<MessageRowData> msgs{orig, row};
    v.set_messages(std::move(msgs), false);
    LayoutCtx lc{surface->factory(), Theme::light()};
    v.measure(lc, {500, 600});
    v.arrange(lc, {0, 0, 500, 600});

    std::string opened;
    v.on_link_clicked = [&](const std::string& url) { opened = url; };

    AccessNode tree = build_access_tree(&v);
    const AccessNode* link = access_test::find_named(tree, "site");
    REQUIRE(link != nullptr);
    CHECK(link->role == Role::Link);
    CHECK(invoke_default_action(*link));
    CHECK(opened == "https://example.org");

    const AccessNode* jump = access_test::find_named(tree, "Jump to replied message");
    REQUIRE(jump != nullptr);
    CHECK(invoke_default_action(*jump)); // original is loaded: scrolls to it
}

TEST_CASE("a forwarded row's name says who it was forwarded from, and its "
         "subtree offers View original message only when the origin is known",
         "[message_list][accessibility]")
{
    auto surface = TestSurface::create(500, 600);
    const auto stage = [&](MessageListView& v, MessageRowData row)
    {
        v.set_messages({std::move(row)}, false);
        LayoutCtx lc{surface->factory(), Theme::light()};
        v.measure(lc, {500, 600});
        v.arrange(lc, {0, 0, 500, 600});
    };
    const auto row_name = [](const AccessNode& tree)
    {
        const AccessNode* list = find_role_msg(tree, Role::List);
        REQUIRE(list != nullptr);
        for (const auto& ch : list->children)
            if (ch.role == Role::ListItem)
                return ch.name;
        return std::string{};
    };

    SECTION("named sender with a link to the origin")
    {
        MessageListView v;
        auto row = text_row("$f", "Bob", "hello");
        row.forwarded = true;
        row.forwarded_sender = "@alice:x";
        row.forwarded_sender_name = "Alice";
        row.forwarded_room_id = "!r:x";
        row.forwarded_event_id = "$orig";
        stage(v, row);
        std::string got_room, got_event;
        v.on_forward_origin_clicked = [&](const std::string& r, const std::string& e)
        {
            got_room = r;
            got_event = e;
        };

        AccessNode tree = build_access_tree(&v);
        CHECK(row_name(tree).find("forwarded from Alice") != std::string::npos);
        const AccessNode* view = access_test::find_named(tree, "View original message");
        REQUIRE(view != nullptr);
        CHECK(invoke_default_action(*view));
        CHECK(got_room == "!r:x");
        CHECK(got_event == "$orig");
    }

    SECTION("falls back to the mxid when no display name is known")
    {
        MessageListView v;
        auto row = text_row("$f", "Bob", "hello");
        row.forwarded = true;
        row.forwarded_sender = "@alice:x";
        stage(v, row);
        AccessNode tree = build_access_tree(&v);
        CHECK(row_name(tree).find("forwarded from @alice:x") != std::string::npos);
    }

    SECTION("no sender reads as a forwarded message; no ids means no action")
    {
        MessageListView v;
        auto row = text_row("$f", "Bob", "hello");
        row.forwarded = true;
        stage(v, row);
        v.on_forward_origin_clicked = [](const std::string&, const std::string&) {};
        AccessNode tree = build_access_tree(&v);
        CHECK(row_name(tree).find("forwarded message") != std::string::npos);
        CHECK(access_test::find_named(tree, "View original message") == nullptr);
    }

    SECTION("an ordinary row is not announced as forwarded")
    {
        MessageListView v;
        stage(v, text_row("$f", "Bob", "hello"));
        AccessNode tree = build_access_tree(&v);
        CHECK(row_name(tree).find("forwarded") == std::string::npos);
        CHECK(access_test::find_named(tree, "View original message") == nullptr);
    }
}

TEST_CASE("a row's accessible name includes the sender status only when the setting is on",
         "[message_list][accessibility]")
{
    auto& settings = tesseract::Settings::instance();
    const bool prev = settings.show_sender_status_in_timeline;

    auto row_name = [](bool on)
    {
        tesseract::Settings::instance().show_sender_status_in_timeline = on;
        MessageListView v;
        auto r = text_row("$a", "Alice", "hello");
        r.sender_status_emoji = "🌴";
        r.sender_status_text = "On holiday";
        std::vector<MessageRowData> msgs;
        msgs.push_back(std::move(r));
        v.set_messages(std::move(msgs), false);
        AccessNode tree = build_access_tree(&v);
        const AccessNode* list = find_role_msg(tree, Role::List);
        REQUIRE(list != nullptr);
        for (const auto& ch : list->children)
            if (ch.role == Role::ListItem)
                return ch.name;
        return std::string{};
    };

    CHECK(row_name(false) == "Alice: hello");
    CHECK(row_name(true) == "Alice (🌴 On holiday): hello");

    // IRC layout never draws the status, so it isn't announced either.
    const auto prev_layout = settings.message_layout;
    settings.message_layout = tesseract::Settings::MessageLayout::Irc;
    CHECK(row_name(true) == "Alice: hello");
    settings.message_layout = prev_layout;

    settings.show_sender_status_in_timeline = prev;
}

TEST_CASE("message rows report the room language set on the list, and none by default",
         "[message_list][accessibility]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(text_row("$a", "Alice", "bonjour"));
    v.set_messages(std::move(msgs), false);

    auto row_language = [&v]
    {
        AccessNode tree = build_access_tree(&v);
        const AccessNode* list = find_role_msg(tree, Role::List);
        REQUIRE(list != nullptr);
        for (const auto& ch : list->children)
            if (ch.role == Role::ListItem)
                return std::pair{list->language, ch.language};
        FAIL("no message row");
        return std::pair<std::string, std::string>{};
    };

    CHECK(row_language() == std::pair<std::string, std::string>{"", ""});

    v.set_room_language("fr-CA");
    CHECK(row_language() == std::pair<std::string, std::string>{"fr-CA", "fr-CA"});

    // Room switch to a room without a language clears it again.
    v.set_room_language({});
    CHECK(row_language() == std::pair<std::string, std::string>{"", ""});
}
