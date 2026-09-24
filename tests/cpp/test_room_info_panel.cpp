#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "tesseract/types.h"
#include "views/ConfirmDialog.h"
#include "views/RoomInfoPanel.h"

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
    info.id   = "!room:example.org";
    info.name = "Room";
    panel.set_media_count(3);
    panel.open(info);

    int fired = 0;
    panel.on_media_view_requested = [&](std::string) { ++fired; };

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
        info.id   = "!room:example.org";
        info.name = "Room";
        panel.open(info);
        std::vector<tesseract::RoomMember> members(2);
        members[0].user_id      = "@alice:example.org";
        members[0].display_name = "Alice";
        members[1].user_id      = "@bob:example.org";
        members[1].display_name = "Bob";
        panel.set_members(std::move(members));

        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        panel.measure(lc, {1000, 800});
        panel.arrange(lc, {0, 0, 1000, 800});
    }

    tesseract::views::RoomInfoPanelBody& body() { return *panel.body_for_test(); }

    tk::Point row_center(int i)
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
    st.panel.set_member_actions_provider([&](const std::string& uid) {
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
    st.panel.on_member_clicked = [&](std::string uid, std::string, std::string) {
        profile_uid = std::move(uid);
    };
    st.panel.on_kick_member = [&](std::string room, std::string uid, std::string) {
        kick_room = std::move(room);
        kick_uid  = std::move(uid);
    };
    st.panel.on_ban_member = [&](std::string, std::string uid, std::string) {
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

TEST_CASE("ConfirmDialog: reason field hands its trimmed text to on_reason before confirm",
          "[dialog][confirm]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<tesseract::views::ConfirmDialog>(&host);
    REQUIRE(dlg->reason_field());

    std::vector<std::string> order;
    tesseract::views::ConfirmDialog::Options opts;
    opts.title        = "Kick Bob?";
    opts.reason_field = true;
    opts.on_reason    = [&](std::string r) { order.push_back("reason:" + r); };
    dlg->open(std::move(opts), [&] { order.push_back("confirm"); });
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

TEST_CASE("ConfirmDialog: reason field hidden and on_reason unused by default",
          "[dialog][confirm]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<tesseract::views::ConfirmDialog>(&host);
    REQUIRE(dlg->reason_field());
    bool reason_fired = false;
    tesseract::views::ConfirmDialog::Options opts;
    opts.title     = "Leave?";
    opts.on_reason = [&](std::string) { reason_fired = true; };
    bool confirmed = false;
    dlg->open(std::move(opts), [&] { confirmed = true; });
    CHECK_FALSE(dlg->reason_field()->own_visible());
    dlg->confirm();
    CHECK(confirmed);
    CHECK_FALSE(reason_fired);
}
