#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "access_test_util.h"
#include "tk/theme.h"
#include "views/AvatarEditControl.h"
#include "views/ComposeBar.h"
#include "views/DatePickerView.h"
#include "views/ExportHistoryDialog.h"
#include "views/KnockStatusCard.h"
#include "views/RoomHeader.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>
#include <string>

// Custom-painted views that now expose their painted content and controls:
// DatePickerView's day grid, ExportHistoryDialog, KnockStatusCard,
// AvatarEditControl (via its owners), RoomHeader's rich topic and lock.

using namespace tk;
using tesseract::views::AvatarEditControl;
using tesseract::views::ComposeBar;
using tesseract::views::DatePickerView;
using tesseract::views::ExportHistoryDialog;
using tesseract::views::KnockStatusCard;
using tesseract::views::RoomHeader;
using access_test::find_named;
using access_test::find_prefix;

TEST_CASE("DatePickerView is a grid of named day cells with month navigation",
          "[datepicker][accessibility]")
{
    auto surface = TestSurface::create(400, 400);
    DatePickerView picker;
    picker.set_max_date(2026, 10, 3);
    picker.open_at({0, 0, 10, 10});
    LayoutCtx lc{surface->factory(), Theme::light()};
    picker.measure(lc, {DatePickerView::kWidth, DatePickerView::kHeight});
    picker.arrange(lc, {0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    picker.paint_overlay(pc); // builds the day cells

    AccessNode tree = build_access_tree(&picker);
    CHECK(tree.role == Role::Grid);
    CHECK(tree.grid_row_count == 6);
    CHECK(tree.grid_col_count == 7);
    CHECK_FALSE(tree.name.empty());

    const AccessNode* prev = find_named(tree, "Previous month");
    REQUIRE(prev != nullptr);
    CHECK(prev->role == Role::Button);
    const AccessNode* next = find_named(tree, "Next month");
    REQUIRE(next != nullptr);
    CHECK(next->state.disabled); // already at the max month

    int cells = 0;
    int picked_day = 0;
    picker.on_date_picked = [&](int, int, int d)
    {
        picked_day = d;
    };
    const AccessNode* first_enabled = nullptr;
    for (const auto& ch : tree.children)
    {
        if (ch.role != Role::GridCell)
        {
            continue;
        }
        ++cells;
        CHECK(ch.grid_row >= 0);
        CHECK(ch.grid_col >= 0);
        if (!ch.state.disabled && !first_enabled)
        {
            first_enabled = &ch;
        }
    }
    CHECK(cells == 31); // October: only in-month days are exposed
    REQUIRE(first_enabled != nullptr);
    CHECK(invoke_default_action(*first_enabled));
    CHECK(picked_day == 1);

    const std::string before = tree.name;
    CHECK(invoke_default_action(*prev));
    CHECK(picker.access_name() != before);
}

TEST_CASE("ExportHistoryDialog is a modal dialog named by its title", "[export][accessibility]")
{
    auto dlg = create_root_widget<ExportHistoryDialog>(nullptr);
    dlg->open("!r:s", "General");
    CHECK(dlg->access_role() == Role::Dialog);
    CHECK(dlg->access_modal());
    CHECK(dlg->access_name() == "Export History of General");
}

TEST_CASE("KnockStatusCard names the room and pending status", "[knock][accessibility]")
{
    KnockStatusCard card;
    tesseract::KnockedRoomInfo info;
    info.room_id = "!r:s";
    info.room_name = "Secret Club";
    info.room_topic = "Members only";
    info.reason = "Friend of Bob";
    card.set_knock(info, {});
    CHECK(card.access_role() == Role::Group);
    CHECK(card.access_name() == "Secret Club: Request pending");
    CHECK(card.access_description() == "Members only. Reason: Friend of Bob");
}

TEST_CASE("AvatarEditControl offers change / remove items only while editable",
          "[avatar][accessibility]")
{
    AvatarEditControl av;
    av.set_geometry({50, 50}, 64);
    CHECK(av.access_items({0, 0}).empty());

    av.set_editable(true);
    auto items = av.access_items({100, 200});
    REQUIRE(items.size() == 1);
    CHECK(items[0].kind == AvatarEditControl::AccessItem::Kind::Change);
    CHECK(items[0].rect.x == 118.0f);

    av.set_avatar_url("mxc://s/a");
    av.set_error("Too big");
    items = av.access_items({0, 0});
    REQUIRE(items.size() == 3);
    CHECK(items[1].kind == AvatarEditControl::AccessItem::Kind::Remove);
    CHECK(items[2].name == "Too big");
}

TEST_CASE("RoomHeader exposes the lock and a rich topic's links after the name",
          "[room_header][accessibility]")
{
    RoomHeader h;
    tesseract::RoomInfo info;
    info.id = "!r:s";
    info.name = "Rust";
    info.is_encrypted = true;
    info.topic = "Docs at https://doc.rust-lang.org";
    h.set_room(info);
    std::string clicked;
    h.on_link_clicked = [&](const std::string& url)
    {
        clicked = url;
    };

    AccessNode tree = build_access_tree(&h);
    const AccessNode* lock = find_named(tree, "Encrypted room");
    REQUIRE(lock != nullptr);
    CHECK(lock->role == Role::Image);
    CHECK(find_prefix(tree, "Docs at") != nullptr);
    const AccessNode* link = find_named(tree, "https://doc.rust-lang.org");
    REQUIRE(link != nullptr);
    CHECK(link->role == Role::Link);
    CHECK(invoke_default_action(*link));
    CHECK(clicked == "https://doc.rust-lang.org");
    // Supplementary rows come after the real children.
    CHECK(&tree.children.back() == link);
}

TEST_CASE("ComposeBar's reply banner is readable and its cancel is a real button",
          "[compose][accessibility]")
{
    auto surface = TestSurface::create(600, 200);
    auto bar = create_root_widget<ComposeBar>(nullptr);
    bar->set_reply_to("$e", "Alice", "Hello");
    LayoutCtx lc{surface->factory(), Theme::light()};
    bar->measure(lc, {600, 200});
    bar->arrange(lc, {0, 0, 600, bar->natural_height()});

    AccessNode tree = build_access_tree(bar.get());
    const AccessNode* banner = find_named(tree, "Replying to Alice: Hello");
    REQUIRE(banner != nullptr);
    CHECK(banner->role == Role::StaticText);
    const AccessNode* cancel = find_named(tree, "Cancel reply");
    REQUIRE(cancel != nullptr);
    CHECK(cancel->role == Role::Button);
    CHECK(invoke_default_action(*cancel));
    CHECK_FALSE(bar->has_reply());
}

TEST_CASE("the composer is labelled with the current room", "[compose][accessibility]")
{
    StubHost host;
    auto bar = create_root_widget<ComposeBar>(&host);
    bar->set_room_name("General");
    AccessNode tree = build_access_tree(bar.get());
    const AccessNode* field = find_named(tree, "Message General");
    REQUIRE(field != nullptr);
    CHECK(field->role == Role::TextInput);
}
