#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"
#include "views/MembershipGroupExpander.h"
#include "tesseract/types.h"

#include <tesseract/settings.h>

#include <cstdint>
#include <utility>

using tesseract::MembershipAction;
using tesseract::Settings;
using tesseract::views::is_membership_group_start;
using tesseract::views::MembershipGroupExpander;
using tesseract::views::membership_group_actions;
using tesseract::views::membership_group_end;
using tesseract::views::membership_group_expandable;
using tesseract::views::membership_group_start_of;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::make_row_data;
using tesseract::views::with_membership_reason;

namespace
{

MessageRowData membership_row(const std::string& event_id,
                              MembershipAction action)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Membership;
    r.event_id = event_id;
    r.membership_action = action;
    return r;
}

MessageRowData text_row(const std::string& event_id)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = event_id;
    return r;
}

MessageRowData day_separator_row()
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::DaySeparator;
    return r;
}

std::vector<std::uint32_t> render_system_row(Settings::MessageLayout layout,
                                             bool membership,
                                             bool repeated = false)
{
    Settings::instance().message_layout = layout;
    auto surface = TestSurface::create(320, 64);
    if (!surface)
        return {};

    MessageRowData row;
    if (membership)
    {
        row = membership_row("$membership", MembershipAction::Left);
        row.membership_target_name = "Alice";
        row.membership_target_user_id = "@alice:example.org";
    }
    else
    {
        row.kind = MessageRowData::Kind::PinnedEvent;
        row.sender_name = "Alice";
        row.body = "left the room";
    }

    std::vector<MessageRowData> rows{row};
    if (membership && repeated)
    {
        row.event_id = "$membership-2";
        rows.push_back(row);
    }

    MessageListView view;
    view.set_messages(std::move(rows), true);
    surface->canvas().clear(tk::Color::rgb(0xffffff));
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    view.measure(lc, {320.0f, 64.0f});
    view.arrange(lc, {0.0f, 0.0f, 320.0f, 64.0f});
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
    view.paint(pc);

    std::vector<std::uint32_t> pixels;
    pixels.reserve(320 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 320; ++x)
        {
            const tk::Color c = surface->read_pixel(x, y);
            pixels.push_back((std::uint32_t(c.r) << 24) |
                             (std::uint32_t(c.g) << 16) |
                             (std::uint32_t(c.b) << 8) | c.a);
        }
    return pixels;
}

} // namespace

TEST_CASE("consecutive same-action membership rows form one group",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Joined),
        membership_row("$c", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK_FALSE(is_membership_group_start(msgs, 1));
    CHECK_FALSE(is_membership_group_start(msgs, 2));
    CHECK(membership_group_end(msgs, 0) == 3);
    CHECK(membership_group_start_of(msgs, 2) == 0);
}

TEST_CASE("mixed membership actions stay in one group",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Joined),
        membership_row("$c", MembershipAction::Left),
        membership_row("$d", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK_FALSE(is_membership_group_start(msgs, 1));
    CHECK_FALSE(is_membership_group_start(msgs, 2));
    CHECK_FALSE(is_membership_group_start(msgs, 3));
    CHECK(membership_group_end(msgs, 0) == 4);
    CHECK(membership_group_start_of(msgs, 3) == 0);
}

TEST_CASE("membership_group_actions lists actions in first-appearance order",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Left),
        membership_row("$c", MembershipAction::Joined),
        membership_row("$d", MembershipAction::Invited),
    };
    const auto runs = membership_group_actions(msgs, 0, 4);
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].action == MembershipAction::Joined);
    CHECK(runs[0].rows == std::vector<std::size_t>{0, 2});
    CHECK(runs[1].action == MembershipAction::Left);
    CHECK(runs[1].rows == std::vector<std::size_t>{1});
    CHECK(runs[2].action == MembershipAction::Invited);
    CHECK(runs[2].rows == std::vector<std::size_t>{3});
}

TEST_CASE("a group is expandable only when an action repeats",
          "[message_list][membership]")
{
    std::vector<MessageRowData> distinct = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Left),
        membership_row("$c", MembershipAction::Invited),
    };
    CHECK_FALSE(membership_group_expandable(distinct, 0));

    std::vector<MessageRowData> repeated = {
        membership_row("$a", MembershipAction::Joined),
        membership_row("$b", MembershipAction::Left),
        membership_row("$c", MembershipAction::Joined),
    };
    CHECK(membership_group_expandable(repeated, 0));

    std::vector<MessageRowData> single = {
        membership_row("$a", MembershipAction::Joined),
    };
    CHECK_FALSE(membership_group_expandable(single, 0));
}

TEST_CASE("a non-membership row breaks a group", "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        text_row("$msg"),
        membership_row("$b", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK(membership_group_end(msgs, 0) == 1); // {a} only — text row breaks it
    CHECK(is_membership_group_start(msgs, 2)); // $b starts its own new group
}

TEST_CASE("a day separator breaks a group even with the same action",
          "[message_list][membership]")
{
    std::vector<MessageRowData> msgs = {
        membership_row("$a", MembershipAction::Joined),
        day_separator_row(),
        membership_row("$b", MembershipAction::Joined),
    };
    CHECK(is_membership_group_start(msgs, 0));
    CHECK(membership_group_end(msgs, 0) == 1);
    CHECK(is_membership_group_start(msgs, 2));
}

TEST_CASE("MembershipGroupExpander toggles independently per group key",
          "[message_list][membership]")
{
    MembershipGroupExpander e;
    CHECK_FALSE(e.is_expanded("$a"));
    e.toggle("$a");
    CHECK(e.is_expanded("$a"));
    CHECK_FALSE(e.is_expanded("$b"));
    e.toggle("$b");
    CHECK(e.is_expanded("$a"));
    CHECK(e.is_expanded("$b"));
    e.toggle("$a");
    CHECK_FALSE(e.is_expanded("$a"));
    CHECK(e.is_expanded("$b"));
    e.clear();
    CHECK_FALSE(e.is_expanded("$b"));
}

TEST_CASE("MembershipStateEvent produces Kind::Membership row with correct fields",
          "[message_list][membership]")
{
    tesseract::MembershipStateEvent ev;
    ev.event_id = "$mem:server";
    ev.sender = "@admin:server";
    ev.sender_name = "Admin";
    ev.timestamp = 1700000042000ULL;
    ev.action = MembershipAction::Kicked;
    ev.target_user_id = "@bob:server";
    ev.target_display_name = "Bob";
    ev.target_avatar_url = "mxc://server/bob";
    ev.reason = "spamming";

    MessageRowData row = make_row_data(ev, /*my_user_id=*/"@me:server");

    REQUIRE(row.kind == MessageRowData::Kind::Membership);
    CHECK(row.event_id == "$mem:server");
    CHECK(row.sender_name == "Admin");
    CHECK(row.timestamp_ms == 1700000042000ULL);
    CHECK(row.membership_action == MembershipAction::Kicked);
    CHECK(row.membership_target_user_id == "@bob:server");
    CHECK(row.membership_target_name == "Bob");
    CHECK(row.membership_target_avatar_url == "mxc://server/bob");
    CHECK(row.membership_reason == "spamming");
    // Gendered narration (MSC4247): defaults to the neutral pronoun,
    // unresolved, until MessageListView::update_member_pronoun() resolves it
    // — Rust never populates this field (see timeline_convert.rs), it's
    // filled in lazily on the C++ side only for the narrow set of actions
    // that actually use a pronoun.
    CHECK(row.target_pronoun == "their");
    CHECK_FALSE(row.pronoun_resolved);
}

TEST_CASE("MessageRowData pronoun fields default to unresolved/neutral",
          "[message_list][membership][pronoun]")
{
    MessageRowData row;
    CHECK(row.target_pronoun == "their");
    CHECK_FALSE(row.pronoun_resolved);
}

TEST_CASE("with_membership_reason appends the reason to removal phrases only",
          "[message_list][membership]")
{
    auto row = membership_row("$e", MembershipAction::Kicked);
    CHECK(with_membership_reason("Bob was removed", row) == "Bob was removed");

    row.membership_reason = "spamming";
    CHECK(with_membership_reason("Bob was removed", row) ==
          "Bob was removed. Reason: spamming");
    row.membership_action = MembershipAction::Banned;
    CHECK(with_membership_reason("Bob was banned", row) ==
          "Bob was banned. Reason: spamming");
    row.membership_action = MembershipAction::KickedAndBanned;
    CHECK(with_membership_reason("x", row) == "x. Reason: spamming");

    // Other transitions keep their phrase even when the event carries a reason.
    row.membership_action = MembershipAction::Left;
    CHECK(with_membership_reason("Bob left the room", row) == "Bob left the room");
}

TEST_CASE("make_row_data keeps a sender status only when its emoji is pure emoji",
          "[message_list][status]")
{
    auto row_for = [](const std::string& emoji)
    {
        tesseract::Event ev;
        ev.event_id = "$a";
        ev.sender = "@alice:server";
        ev.sender_name = "Alice";
        ev.body = "hi";
        ev.sender_status_emoji = emoji;
        ev.sender_status_text = "On holiday";
        return make_row_data(ev, "@me:server");
    };

    for (const char* ok : {"🌴", "👩‍💻", "🇫🇷", "❤️"})
    {
        const auto r = row_for(ok);
        CHECK(r.sender_status_emoji == ok);
        CHECK(r.sender_status_text == "On holiday");
    }
    // MSC4426 limits the emoji to 32 bytes: 8 x 4-byte emoji fit, 9 don't.
    const std::string at_limit = "🌴🌴🌴🌴🌴🌴🌴🌴";
    const std::string too_long = at_limit + "🌴";
    CHECK(row_for(at_limit).sender_status_emoji == at_limit);
    CHECK(row_for(too_long).sender_status_emoji.empty());
    CHECK(row_for(too_long).sender_status_text.empty());

    for (const char* bad : {"(admin)", "✔ mod", "🌴a", "", "1"})
    {
        const auto r = row_for(bad);
        CHECK(r.sender_status_emoji.empty());
        CHECK(r.sender_status_text.empty());
    }
}

TEST_CASE("update_member_profile patches only that sender's rows", "[message_list][membership]")
{
    using tesseract::views::MessageListView;
    MessageListView view;
    auto text = [](const std::string& id, const std::string& sender, const std::string& name,
                   const std::string& avatar)
    {
        MessageRowData r = text_row(id);
        r.sender = sender;
        r.sender_name = name;
        r.sender_avatar_url = avatar;
        return r;
    };
    view.set_messages({text("$1", "@me:x", "Me", "mxc://x/old"),
                       text("$2", "@bob:x", "Bob", "mxc://x/bob"),
                       text("$3", "@me:x", "Me", "mxc://x/old")});

    // Avatar only: name untouched, other senders untouched.
    view.update_member_profile("@me:x", std::nullopt, std::string("mxc://x/new"));
    CHECK(view.messages()[0].sender_avatar_url == "mxc://x/new");
    CHECK(view.messages()[2].sender_avatar_url == "mxc://x/new");
    CHECK(view.messages()[0].sender_name == "Me");
    CHECK(view.messages()[1].sender_avatar_url == "mxc://x/bob");

    // Name only.
    view.update_member_profile("@me:x", std::string("Me!"), std::nullopt);
    CHECK(view.messages()[0].sender_name == "Me!");
    CHECK(view.messages()[0].sender_avatar_url == "mxc://x/new");
    CHECK(view.messages()[1].sender_name == "Bob");

    // Removed avatar = empty url.
    view.update_member_profile("@me:x", std::nullopt, std::string());
    CHECK(view.messages()[2].sender_avatar_url.empty());

    // Unknown sender: no-op.
    view.update_member_profile("@nobody:x", std::string("X"), std::string("mxc://x/x"));
    CHECK(view.messages()[1].sender_name == "Bob");
}

TEST_CASE("membership_run_phrase counts each person once", "[message_list][membership]")
{
    using tesseract::views::MembershipActionRun;
    using tesseract::views::membership_run_phrase;

    auto row = [](const std::string& id, const std::string& user, const std::string& name)
    {
        MessageRowData r = membership_row(id, MembershipAction::AvatarChanged);
        r.membership_target_user_id = user;
        r.membership_target_name = name;
        return r;
    };

    SECTION("one person twice reads as one person")
    {
        std::vector<MessageRowData> msgs = {row("$1", "@john:x", "John"), row("$2", "@john:x", "John")};
        MembershipActionRun run{MembershipAction::AvatarChanged, {0, 1}};
        CHECK(membership_run_phrase(msgs, run) == "John updated their avatar");
    }
    SECTION("two people, one of them twice")
    {
        std::vector<MessageRowData> msgs = {row("$1", "@john:x", "John"), row("$2", "@bob:x", "Bob"),
                                            row("$3", "@john:x", "John")};
        MembershipActionRun run{MembershipAction::AvatarChanged, {0, 1, 2}};
        CHECK(membership_run_phrase(msgs, run) == "John and Bob updated their avatars");
    }
    SECTION("three distinct people keep first-appearance order")
    {
        std::vector<MessageRowData> msgs = {row("$1", "@a:x", "Ann"), row("$2", "@b:x", "Bob"),
                                            row("$3", "@c:x", "Cy"), row("$4", "@a:x", "Ann")};
        MembershipActionRun run{MembershipAction::AvatarChanged, {0, 1, 2, 3}};
        CHECK(membership_run_phrase(msgs, run) == "Ann, Bob and 1 other updated their avatars");
    }
    SECTION("a single row keeps its reason")
    {
        MessageRowData k = membership_row("$k", MembershipAction::Kicked);
        k.membership_target_user_id = "@bob:x";
        k.membership_target_name = "Bob";
        k.membership_reason = "spam";
        std::vector<MessageRowData> msgs = {k};
        MembershipActionRun run{MembershipAction::Kicked, {0}};
        CHECK(membership_run_phrase(msgs, run).find("Reason: spam") != std::string::npos);
    }
}

TEST_CASE("bubble membership notices are centered without horizontal rules",
          "[message_list][membership][layout]")
{
    struct RestoreLayout
    {
        Settings::MessageLayout previous = Settings::instance().message_layout;
        ~RestoreLayout() { Settings::instance().message_layout = previous; }
    } restore;

    const auto membership = render_system_row(Settings::MessageLayout::Bubbles, true);
    const auto pinned = render_system_row(Settings::MessageLayout::Bubbles, false);
    REQUIRE_FALSE(membership.empty());
    REQUIRE_FALSE(pinned.empty());

    bool pinned_left_rule = false;
    bool pinned_right_rule = false;
    for (std::size_t y = 0; y < 64; ++y)
    {
        const std::size_t row = y * 320;
        const std::size_t left = row + 20;
        const std::size_t right = row + 300;
        CHECK(membership[left] == membership[row + 5]);
        CHECK(membership[right] == membership[row + 315]);
        pinned_left_rule |= pinned[left] != pinned[row + 5];
        pinned_right_rule |= pinned[right] != pinned[row + 315];
    }
    CHECK(pinned_left_rule);
    CHECK(pinned_right_rule);

    CHECK(render_system_row(Settings::MessageLayout::Bubbles, true, true) ==
          membership);
    CHECK(render_system_row(Settings::MessageLayout::Classic, true) !=
          render_system_row(Settings::MessageLayout::Classic, false));
    CHECK(render_system_row(Settings::MessageLayout::Irc, true) !=
          render_system_row(Settings::MessageLayout::Irc, false));
}
