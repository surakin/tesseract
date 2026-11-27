#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "tesseract/types.h"
#include "views/ConfirmDialog.h"
#include "views/RoomInfoPanel.h"
#include "views/power_level_presets.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using tesseract::views::RoomInfoPanel;

TEST_CASE("RoomInfoPanel: Media row is clickable when the panel sits right of x=0",
          "[room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};

    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    panel.set_media_count(3);
    panel.open(info);

    int fired = 0;
    panel.on_media_view_requested = [&](std::string)
    {
        ++fired;
    };

    const tk::Rect bounds{0, 0, 1000, 800};
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light(), nullptr, &host};
    panel.paint(pc);

    // Sweep down the panel's left inset (clear of the inset buttons and the
    // right-edge scrollbar); the full-width Media row must be hit somewhere.
    const float x = 1000.0f - RoomInfoPanel::kPanelW + 2.0f;
    for (float y = 50.0f; y < 800.0f && fired == 0; y += 2.0f)
    {
        host.dispatch_pointer_down({x, y});
        host.dispatch_pointer_up({x, y});
    }
    CHECK(fired == 1);
}

namespace
{

// An open panel with two members, laid out at the right edge of a 1000x800
// surface; `body` is where member rows are hit-tested.
struct MemberMenuStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 800);
    TestHost host{nullptr};
    std::unique_ptr<RoomInfoPanel> owner = tk::create_root_widget<RoomInfoPanel>(&host);
    RoomInfoPanel& panel = *owner;

    MemberMenuStage()
    {
        host.set_root(owner.get());
        tesseract::RoomInfo info;
        info.id = "!room:example.org";
        info.name = "Room";
        panel.open(info);
        std::vector<tesseract::RoomMember> members(2);
        members[0].user_id = "@alice:example.org";
        members[0].display_name = "Alice";
        members[1].user_id = "@bob:example.org";
        members[1].display_name = "Bob";
        panel.set_members(std::move(members));

        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        panel.measure(lc, {1000, 800});
        panel.arrange(lc, {0, 0, 1000, 800});
    }

    tesseract::views::RoomInfoPanelBody& body() const
    {
        return *panel.body_for_test();
    }

    tk::Point row_center(int i) const
    {
        const tk::Rect r = body().member_row_rect_for_test(i);
        REQUIRE_FALSE(r.empty());
        return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
};

} // namespace

TEST_CASE("RoomInfoPanel: right-clicking a member opens profile/kick/ban menu",
          "[room_info_panel][context-menu]")
{
    MemberMenuStage st;
    std::vector<std::string> queried;
    st.panel.set_member_actions_provider(
        [&](const std::string& uid)
        {
            queried.push_back(uid);
            return RoomInfoPanel::MemberActions{true, false};
        });

    REQUIRE(st.body().on_right_click(st.row_center(1)));
    CHECK(queried == std::vector<std::string>{"@bob:example.org"});

    const auto& items = st.body().member_menu_items_for_test();
    REQUIRE(items.size() == 4);
    CHECK(items[0].label == "Show profile");
    CHECK(items[0].enabled);
    CHECK(items[1].is_separator);
    CHECK(items[2].label == "Kick user\xe2\x80\xa6");
    CHECK(items[2].destructive);
    CHECK(items[2].enabled);
    CHECK(items[3].label == "Ban user\xe2\x80\xa6");
    CHECK(items[3].destructive);
    CHECK_FALSE(items[3].enabled);
}

TEST_CASE("RoomInfoPanel: member menu items fire the matching callbacks",
          "[room_info_panel][context-menu]")
{
    MemberMenuStage st;
    std::string profile_uid, kick_uid, kick_room, ban_uid;
    st.panel.on_member_clicked = [&](std::string uid, std::string, std::string)
    {
        profile_uid = std::move(uid);
    };
    st.panel.on_kick_member = [&](std::string room, std::string uid, std::string)
    {
        kick_room = std::move(room);
        kick_uid = std::move(uid);
    };
    st.panel.on_ban_member = [&](std::string, std::string uid, std::string)
    {
        ban_uid = std::move(uid);
    };

    REQUIRE(st.body().on_right_click(st.row_center(0)));
    const auto items = st.body().member_menu_items_for_test();
    REQUIRE(items.size() == 4);
    items[0].on_selected();
    items[2].on_selected();
    items[3].on_selected();
    CHECK(profile_uid == "@alice:example.org");
    CHECK(kick_uid == "@alice:example.org");
    CHECK(kick_room == "!room:example.org");
    CHECK(ban_uid == "@alice:example.org");
}

TEST_CASE("RoomInfoPanel: without a provider kick/ban are disabled; empty space opens nothing",
          "[room_info_panel][context-menu]")
{
    MemberMenuStage st;
    REQUIRE(st.body().on_right_click(st.row_center(0)));
    const auto& items = st.body().member_menu_items_for_test();
    REQUIRE(items.size() == 4);
    CHECK_FALSE(items[2].enabled);
    CHECK_FALSE(items[3].enabled);

    // Top of the body is the room avatar/name, never a member row.
    CHECK_FALSE(st.body().on_right_click({5.0f, 5.0f}));
}

TEST_CASE("RoomInfoPanel: power-level item appears only when allowed",
          "[room_info_panel][context-menu]")
{
    {
        MemberMenuStage st;
        st.panel.set_member_actions_provider(
            [](const std::string&)
            {
                return RoomInfoPanel::MemberActions{false, false, true, 100};
            });
        REQUIRE(st.body().on_right_click(st.row_center(1)));
        const auto& items = st.body().member_menu_items_for_test();
        REQUIRE(items.size() == 5);
        CHECK(items[2].label == "Change power level\xe2\x80\xa6");
        CHECK(items[2].enabled);
        CHECK_FALSE(items[2].destructive);
        CHECK(items[3].label == "Kick user\xe2\x80\xa6");
        CHECK(items[4].label == "Ban user\xe2\x80\xa6");
    }
    {
        MemberMenuStage st;
        st.panel.set_member_actions_provider(
            [](const std::string&)
            {
                return RoomInfoPanel::MemberActions{true, true};
            });
        REQUIRE(st.body().on_right_click(st.row_center(1)));
        const auto& items = st.body().member_menu_items_for_test();
        REQUIRE(items.size() == 4);
        for (const auto& item : items)
        {
            CHECK(item.label != "Change power level\xe2\x80\xa6");
        }
    }
}

namespace
{

// Re-seeds the stage's members so Bob carries `bob_level` and an avatar.
void set_bob_level(MemberMenuStage& st, std::int64_t bob_level)
{
    std::vector<tesseract::RoomMember> members(2);
    members[0].user_id = "@alice:example.org";
    members[0].display_name = "Alice";
    members[1].user_id = "@bob:example.org";
    members[1].display_name = "Bob";
    members[1].avatar_url = "mxc://example.org/bob";
    members[1].power_level = bob_level;
    st.panel.set_members(std::move(members));
    tk::LayoutCtx lc{st.surface->factory(), tk::Theme::light()};
    st.panel.measure(lc, {1000, 800});
    st.panel.arrange(lc, {0, 0, 1000, 800});
}

// Bob's row index: a higher power level sorts him above Alice.
int bob_row(const MemberMenuStage& st)
{
    const auto names = st.body().shown_member_names_for_test();
    const auto it = std::find(names.begin(), names.end(), "Bob");
    REQUIRE(it != names.end());
    return static_cast<int>(it - names.begin());
}

} // namespace

TEST_CASE("RoomInfoPanel: power-level item forwards member and levels",
          "[room_info_panel][context-menu]")
{
    MemberMenuStage st;
    set_bob_level(st, 50);
    st.panel.set_member_actions_provider(
        [](const std::string&)
        {
            return RoomInfoPanel::MemberActions{false, false, true, 100};
        });

    std::string room, uid, name, avatar;
    std::int64_t current = -1, own = -1;
    st.panel.on_change_power_level = [&](std::string r, std::string u, std::string n, std::string a,
                                         std::int64_t c, std::int64_t o)
    {
        room = std::move(r);
        uid = std::move(u);
        name = std::move(n);
        avatar = std::move(a);
        current = c;
        own = o;
    };

    REQUIRE(st.body().on_right_click(st.row_center(bob_row(st))));
    const auto items = st.body().member_menu_items_for_test();
    REQUIRE(items.size() == 5);
    items[2].on_selected();
    CHECK(room == "!room:example.org");
    CHECK(uid == "@bob:example.org");
    CHECK(name == "Bob");
    CHECK(avatar == "mxc://example.org/bob");
    CHECK(current == 50);
    CHECK(own == 100);
}

TEST_CASE("RoomInfoPanel: power-level item hidden for a member at or above own level",
          "[room_info_panel][context-menu]")
{
    MemberMenuStage st;
    set_bob_level(st, 50);
    st.panel.set_member_actions_provider(
        [](const std::string&)
        {
            return RoomInfoPanel::MemberActions{false, false, true, 50};
        });

    REQUIRE(st.body().on_right_click(st.row_center(bob_row(st))));
    const auto& items = st.body().member_menu_items_for_test();
    REQUIRE(items.size() == 4);
    for (const auto& item : items)
    {
        CHECK(item.label != "Change power level\xe2\x80\xa6");
    }
}

TEST_CASE("ConfirmDialog: reason field hands its trimmed text to on_reason before confirm",
          "[dialog][confirm]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<tesseract::views::ConfirmDialog>(&host);
    REQUIRE(dlg->reason_field());

    std::vector<std::string> order;
    tesseract::views::ConfirmDialog::Options opts;
    opts.title = "Kick Bob?";
    opts.reason_field = true;
    opts.on_reason = [&](std::string r)
    {
        order.push_back("reason:" + r);
    };
    dlg->open(std::move(opts),
              [&]
              {
                  order.emplace_back("confirm");
              });
    CHECK(dlg->reason_field()->own_visible());

    auto surface = TestSurface::create(800, 600);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    dlg->measure(lc, {800, 600});
    dlg->arrange(lc, {0, 0, 800, 600});

    dlg->reason_field()->set_text("  spamming  ");
    dlg->confirm();
    CHECK(order == std::vector<std::string>{"reason:spamming", "confirm"});
    CHECK_FALSE(dlg->is_open());
}

TEST_CASE("ConfirmDialog: reason field hidden and on_reason unused by default", "[dialog][confirm]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<tesseract::views::ConfirmDialog>(&host);
    REQUIRE(dlg->reason_field());
    bool reason_fired = false;
    tesseract::views::ConfirmDialog::Options opts;
    opts.title = "Leave?";
    opts.on_reason = [&](std::string)
    {
        reason_fired = true;
    };
    bool confirmed = false;
    dlg->open(std::move(opts),
              [&]
              {
                  confirmed = true;
              });
    CHECK_FALSE(dlg->reason_field()->own_visible());
    dlg->confirm();
    CHECK(confirmed);
    CHECK_FALSE(reason_fired);
}

TEST_CASE("RoomInfoPanel: the history badge names the room's visibility", "[room_info_panel]")
{
    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    info.history_visibility = "joined";
    panel.open(info);
    CHECK(panel.body_for_test()->badge_labels_for_test() == "History: Members (since joined)");

    // An unknown wire value is shown as it arrived rather than dropped.
    info.history_visibility = "something_new";
    panel.open(info);
    CHECK(panel.body_for_test()->badge_labels_for_test() == "something_new");
}

TEST_CASE("RoomInfoPanel: a collapsed roster lists the top five in display order",
          "[room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    panel.open(info);

    // Forty ordinary members arrive in reverse, plus three with a power level.
    std::vector<tesseract::RoomMember> members;
    for (int i = 39; i >= 0; --i)
    {
        tesseract::RoomMember m;
        m.user_id = "@m" + std::to_string(i) + ":example.org";
        m.display_name = "m" + std::string(i < 10 ? "0" : "") + std::to_string(i);
        members.push_back(m);
    }
    auto add = [&](const char* name, int64_t level)
    {
        tesseract::RoomMember m;
        m.user_id = std::string("@") + name + ":example.org";
        m.display_name = name;
        m.power_level = level;
        members.push_back(m);
    };
    add("Zoe", 100);
    add("adam", 100);
    add("Mod", 50);
    panel.set_members(members);

    const tk::Rect bounds{0, 0, 1000, 800};
    auto* body = panel.body_for_test();
    REQUIRE(body);
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    // Admins sort first, names case-insensitively; the rest follow by name.
    CHECK(body->shown_member_names_for_test() ==
          std::vector<std::string>{"adam", "Zoe", "Mod", "m00", "m01"});

    // Expanding orders the whole roster and lists every member.
    body->expand_members_for_test();
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    const auto all = body->shown_member_names_for_test();
    REQUIRE(all.size() == 43);
    CHECK(all[0] == "adam");
    CHECK(all[2] == "Mod");
    CHECK(all.back() == "m39");
}

TEST_CASE("RoomInfoPanel: service members are listed apart from and below the regular members",
          "[room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    panel.open(info);

    auto member = [](const char* name, bool service)
    {
        tesseract::RoomMember m;
        m.user_id = std::string("@") + name + ":example.org";
        m.display_name = name;
        m.is_service_member = service;
        return m;
    };
    // A bot sorts first alphabetically but is listed under the regular members.
    panel.set_members({member("bridgebot", true), member("carol", false), member("alice", false)});

    const tk::Rect bounds{0, 0, 1000, 800};
    auto* body = panel.body_for_test();
    REQUIRE(body);
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    CHECK(body->service_section_shown_for_test());
    CHECK(body->shown_member_names_for_test() ==
          std::vector<std::string>{"alice", "carol", "bridgebot"});
    // The service row is laid out below the last regular row.
    CHECK(body->member_row_rect_for_test(2).y > body->member_row_rect_for_test(1).y);

    // Without service members there is no section.
    panel.set_members({member("alice", false)});
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    CHECK_FALSE(body->service_section_shown_for_test());
    CHECK(body->shown_member_names_for_test() == std::vector<std::string>{"alice"});
}

TEST_CASE("RoomInfoPanel: expanding the roster does not reuse a service member's cached text",
          "[room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    panel.open(info);

    auto member = [](const std::string& name, bool service)
    {
        tesseract::RoomMember m;
        m.user_id = "@" + name + ":example.org";
        m.display_name = name;
        m.is_service_member = service;
        return m;
    };
    // Six regular members: collapsed, row 5 is the service bot; expanded, it is
    // the sixth regular member.
    std::vector<tesseract::RoomMember> members;
    for (const char* n : {"a", "b", "c", "d", "e", "f"})
    {
        members.push_back(member(n, false));
    }
    members.push_back(member("bridgebot", true));
    panel.set_members(members);

    const tk::Rect bounds{0, 0, 1000, 800};
    auto* body = panel.body_for_test();
    REQUIRE(body);
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light(), nullptr, &host};
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    panel.paint(pc);
    CHECK(body->member_layout_owner_for_test(5) == "@bridgebot:example.org");

    body->expand_members_for_test();
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    panel.paint(pc);
    const auto names = body->shown_member_names_for_test();
    REQUIRE(names.size() == 7);
    CHECK(names[5] == "f");
    CHECK(body->member_layout_owner_for_test(5) == "@f:example.org");
    CHECK(body->member_layout_owner_for_test(6) == "@bridgebot:example.org");
}

TEST_CASE("member_role: labels only levels that differ from the room default",
          "[room_info_panel][roles]")
{
    using tesseract::views::member_role;
    using tesseract::views::member_role_label;
    using tesseract::views::MemberRole;
    constexpr auto kCreator = std::numeric_limits<std::int64_t>::max();

    CHECK(member_role(0, 0) == MemberRole::None);
    CHECK(member_role(100, 0) == MemberRole::Admin);
    CHECK(member_role(50, 0) == MemberRole::Moderator);
    CHECK(member_role(kCreator, 0) == MemberRole::Owner);
    CHECK(member_role(25, 0) == MemberRole::Custom);
    CHECK(member_role(-1, 0) == MemberRole::Custom);
    // A non-zero default: members at it are unlabelled, 0 is not the default.
    CHECK(member_role(10, 10) == MemberRole::None);
    CHECK(member_role(0, 10) == MemberRole::Custom);
    CHECK(member_role(50, 50) == MemberRole::None);

    CHECK(member_role_label(MemberRole::None, 0).empty());
    CHECK(member_role_label(MemberRole::Owner, kCreator) == "Owner");
    CHECK(member_role_label(MemberRole::Admin, 100) == "Admin");
    CHECK(member_role_label(MemberRole::Moderator, 50) == "Moderator");
    CHECK(member_role_label(MemberRole::Custom, 25) == "Custom (25)");
}

TEST_CASE("member_role_color: fixed role hues, muted for custom", "[room_info_panel][roles]")
{
    using tesseract::views::member_role_color;
    using tesseract::views::MemberRole;
    for (const auto& theme : {tk::Theme::light(), tk::Theme::dark()})
    {
        const auto& pal = theme.palette;
        CHECK(member_role_color(MemberRole::Owner, pal) == pal.role_owner);
        CHECK(member_role_color(MemberRole::Admin, pal) == pal.role_admin);
        CHECK(member_role_color(MemberRole::Moderator, pal) == pal.role_moderator);
        CHECK(member_role_color(MemberRole::Custom, pal) == pal.text_muted);
        CHECK(pal.role_owner != pal.role_admin);
        CHECK(pal.role_admin != pal.role_moderator);
    }
}

TEST_CASE("RoomInfoPanel: member rows carry role labels against the room default",
          "[room_info_panel][roles]")
{
    auto surface = TestSurface::create(1000, 800);
    TestHost host(nullptr);
    auto owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(owner.get());
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    owner->open(info);
    owner->set_default_power_level(10);
    std::vector<tesseract::RoomMember> members(3);
    members[0].user_id = "@admin:example.org";
    members[0].display_name = "Ada";
    members[0].power_level = 100;
    members[1].user_id = "@plain:example.org";
    members[1].display_name = "Pat";
    members[1].power_level = 10;
    members[2].user_id = "@zero:example.org";
    members[2].display_name = "Zed";
    members[2].power_level = 0;
    owner->set_members(std::move(members));

    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    owner->measure(lc, {1000, 800});
    owner->arrange(lc, {0, 0, 1000, 800});
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light(), nullptr, &host};
    owner->paint(pc); // builds the role layouts without crashing

    auto& body = *owner->body_for_test();
    const auto names = body.shown_member_names_for_test();
    REQUIRE(names.size() == 3);
    for (int i = 0; i < 3; ++i)
    {
        const std::string label = body.member_role_label_for_test(i);
        const std::string access = body.member_access_name_for_test(i);
        if (names[static_cast<std::size_t>(i)] == "Ada")
        {
            CHECK(label == "Admin");
            CHECK(access == "Ada, Admin");
        }
        else if (names[static_cast<std::size_t>(i)] == "Pat")
        {
            CHECK(label.empty());
            CHECK(access == "Pat");
        }
        else
        {
            CHECK(label == "Custom (0)");
            CHECK(access == "Zed, Custom (0)");
        }
    }

    // Changing the default re-evaluates every row.
    owner->set_default_power_level(0);
    for (int i = 0; i < 3; ++i)
    {
        if (names[static_cast<std::size_t>(i)] == "Zed")
        {
            CHECK(body.member_role_label_for_test(i).empty());
        }
        if (names[static_cast<std::size_t>(i)] == "Pat")
        {
            CHECK(body.member_role_label_for_test(i) == "Custom (10)");
        }
    }
}
