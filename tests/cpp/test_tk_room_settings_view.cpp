#include <catch2/catch_test_macros.hpp>

#include "views/RoomSettingsView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

using tesseract::views::RoomSettingsView;

namespace
{

struct TkRoomSettingsViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

tesseract::RoomInfo make_room_info()
{
    tesseract::RoomInfo info;
    info.id         = "!room:example.org";
    info.name       = "My Room";
    info.topic      = "My Topic";
    info.avatar_url = "mxc://example.org/avatar";
    return info;
}

} // namespace

TEST_CASE("RoomSettingsView: closed by default", "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    CHECK_FALSE(v.is_open());
    REQUIRE(v.name_field() != nullptr);
    CHECK_FALSE(v.name_field()->visible());
    REQUIRE(v.topic_field() != nullptr);
    CHECK_FALSE(v.topic_field()->visible());
}

TEST_CASE("RoomSettingsView: open() seeds state and opens the view",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    CHECK(v.is_open());
}

TEST_CASE("RoomSettingsView: close() closes the view",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.close();
    CHECK_FALSE(v.is_open());
}

TEST_CASE("RoomSettingsView: field rects empty until permissions granted",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // No set_field_permissions call yet -> every field denied by default.
    REQUIRE(v.name_field() != nullptr);
    CHECK_FALSE(v.name_field()->visible());
    REQUIRE(v.topic_field() != nullptr);
    CHECK_FALSE(v.topic_field()->visible());
}

TEST_CASE("RoomSettingsView: the Emojis & Stickers tab's new-pack-name field "
          "stays hidden on General (the default tab)",
          "[room_settings][view]")
{
    // Regression test: ImagePackEditorView::open() used to force itself
    // visible(true) unconditionally, regardless of which tab SideTabView
    // actually had selected — so opening Room Settings (which always starts
    // on General, tab index 0) made the Emojis & Stickers tab's
    // new_pack_name_field_ ("Pack name" placeholder) show up over General
    // the moment image-pack edit permission was granted, even though its own
    // tab was never selected.
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_image_pack_field_permissions(true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.image_pack_editor() != nullptr);
    REQUIRE(v.image_pack_editor()->new_pack_name_field() != nullptr);
    CHECK_FALSE(v.image_pack_editor()->new_pack_name_field()->visible());
}

TEST_CASE("RoomSettingsView: per-field permission gating",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(/*can_name=*/true, /*can_topic=*/false,
                            /*can_avatar=*/false);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.name_field() != nullptr);
    CHECK(v.name_field()->visible());
    REQUIRE(v.topic_field() != nullptr);
    CHECK_FALSE(v.topic_field()->visible());
}

TEST_CASE("RoomSettingsView: both fields editable when both permitted",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.name_field() != nullptr);
    CHECK(v.name_field()->visible());
    REQUIRE(v.topic_field() != nullptr);
    CHECK(v.topic_field()->visible());
}

TEST_CASE("RoomSettingsView: General's fields stay hidden across a relayout "
          "after switching away from the tab",
          "[room_settings][view]")
{
    // Regression test: SideTabView::arrange() re-arranges every tab's
    // content on each relayout, not just the selected one — a deselected
    // tab's still-editable field used to reshow itself the moment a second
    // arrange() pass ran (e.g. the relayout the tab-switch handler itself
    // triggers via on_layout_changed), because RoomGeneralSection::Content::
    // arrange() decided visibility from permission state alone, ignoring
    // that its own ancestor tab had just been hidden.
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.name_field() != nullptr);
    CHECK(v.name_field()->visible());
    REQUIRE(v.topic_field() != nullptr);
    CHECK(v.topic_field()->visible());

    // Click the "Media" tab (index 1) — see the tab-coordinate comment in
    // the Emojis & Stickers test above: row center y = 49 + (1+0.5)*36 = 103.
    const tk::Point tab_pt{100.0f, 103.0f};
    tk::Widget* tab_hit = v.dispatch_pointer_down(tab_pt);
    REQUIRE(tab_hit != nullptr);
    tab_hit->on_pointer_up(tab_hit->world_to_local(tab_pt), /*inside_self=*/true);

    // A second full arrange() pass — simulating the relayout the tab-switch
    // handler's on_layout_changed()/surface_->relayout() triggers — must not
    // resurrect General's now-deselected fields.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK_FALSE(v.name_field()->visible());
    CHECK_FALSE(v.topic_field()->visible());
}

TEST_CASE("RoomSettingsView: on_cancel fires and closes nothing by itself",
          "[room_settings][view]")
{
    // Cancel is wired by the shell to call close(); the view itself only
    // fires the callback (mirrors ConfirmDialog's cancel_btn_ contract).
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    int cancel_count = 0;
    v.on_cancel = [&] { ++cancel_count; };

    // Cancel sits at the bottom-right of the footer bar, left of Accept.
    // With an 800x600 stage: footer_y=536 (600-kFooterH), btns_y=550
    // (footer_y + (kFooterH-kBtnH)/2), kBtnH=36, both buttons floor at the
    // 88px minimum width -> cancel spans x[592,680], y[550,586].
    // tk::Button fires on_click_ on release, so press+release both points.
    const tk::Point pt{630.0f, 558.0f};
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);
    hit->on_pointer_up(hit->world_to_local(pt), /*inside_self=*/true);
    CHECK(cancel_count == 1);
}

TEST_CASE("RoomSettingsView: on_accept fires only with changed fields",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Simulate the user typing "New Name" into the self-owned name field —
    // fire the stub native field's stored on_changed callback directly
    // (tk::TextField doesn't expose its private backing field for tests).
    REQUIRE_FALSE(host.fields_created.empty());
    host.fields_created[0]->on_changed("New Name");
    // Topic left unchanged.

    std::string accepted_room_id;
    tesseract::views::RoomSettingsChanges accepted_changes;
    int accept_count = 0;
    v.on_accept = [&](std::string room_id,
                      tesseract::views::RoomSettingsChanges changes) {
        ++accept_count;
        accepted_room_id = std::move(room_id);
        accepted_changes = std::move(changes);
    };

    // Accept sits at the bottom-right corner of the footer bar: with an
    // 800x600 stage, accept spans x[688,776], y[550,586] (see cancel's
    // comment above for the layout math).
    const tk::Point pt{730.0f, 558.0f};
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);
    hit->on_pointer_up(hit->world_to_local(pt), /*inside_self=*/true);

    REQUIRE(accept_count == 1);
    CHECK(accepted_room_id == "!room:example.org");
    REQUIRE(accepted_changes.name.has_value());
    CHECK(*accepted_changes.name == "New Name");
    CHECK_FALSE(accepted_changes.topic.has_value());
    CHECK_FALSE(accepted_changes.avatar_mxc.has_value());
    // Emojis & Stickers was never visited -> unset, same as any other
    // untouched field.
    CHECK_FALSE(accepted_changes.image_packs.has_value());
}

TEST_CASE("RoomSettingsView: shared footer stays visible and in the same "
          "place on the Emojis & Stickers tab",
          "[room_settings][view]")
{
    // The image-pack tab used to hide this view's own footer and paint its
    // own instead; it now commits through the one shared footer like every
    // other tab (see image_pack_editor()'s doc comment).
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_image_pack_field_permissions(true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.accept_button() != nullptr);
    CHECK(v.accept_button()->visible());
    const tk::Rect accept_rect_before = v.accept_button()->bounds();

    // Side tab column: kSidebarWidth=200, kTabHeight=36 (tk::SideTabView),
    // tabs start at y = kBarHeight(48) + 1 = 49. Emojis & Stickers is the
    // 5th tab (index 4): row center y = 49 + (4 + 0.5) * 36 = 211.
    const tk::Point tab_pt{100.0f, 211.0f};
    tk::Widget* tab_hit = v.dispatch_pointer_down(tab_pt);
    REQUIRE(tab_hit != nullptr);
    tab_hit->on_pointer_up(tab_hit->world_to_local(tab_pt), /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(!v.image_pack_new_pack_name_field_rect().empty());

    CHECK(v.accept_button()->visible());
    const tk::Rect accept_rect_after = v.accept_button()->bounds();
    CHECK(accept_rect_after.x == accept_rect_before.x);
    CHECK(accept_rect_after.y == accept_rect_before.y);
    CHECK(accept_rect_after.w == accept_rect_before.w);
    CHECK(accept_rect_after.h == accept_rect_before.h);
}

TEST_CASE("RoomSettingsView: on_accept's changes.image_packs is set when "
          "the Emojis & Stickers tab was actually edited",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_image_pack_field_permissions(true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Switch to the Emojis & Stickers tab (index 4; see the footer test
    // above for the sidebar tab-row geometry derivation).
    const tk::Point tab_pt{100.0f, 211.0f};
    tk::Widget* tab_hit = v.dispatch_pointer_down(tab_pt);
    REQUIRE(tab_hit != nullptr);
    tab_hit->on_pointer_up(tab_hit->world_to_local(tab_pt), /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    v.image_pack_editor()->set_new_pack_name_text("New Pack");

    // Create button: SideTabView's content area starts at x=200, tabs
    // start at y=49; ImagePackEditorView::arrange() puts Create at
    // x[688,776], y[85,117] within that -> world center (732, 101).
    const tk::Point create_pt{732.0f, 101.0f};
    tk::Widget* create_hit = v.dispatch_pointer_down(create_pt);
    REQUIRE(create_hit != nullptr);
    create_hit->on_pointer_up(create_hit->world_to_local(create_pt),
                              /*inside_self=*/true);
    REQUIRE(v.image_pack_editor()->packs().size() == 1);
    REQUIRE(v.image_pack_editor()->has_changes());

    tesseract::views::RoomSettingsChanges accepted_changes;
    v.on_accept = [&](std::string, tesseract::views::RoomSettingsChanges changes) {
        accepted_changes = std::move(changes);
    };
    const tk::Point accept_pt{730.0f, 558.0f};
    tk::Widget* accept_hit = v.dispatch_pointer_down(accept_pt);
    REQUIRE(accept_hit != nullptr);
    accept_hit->on_pointer_up(accept_hit->world_to_local(accept_pt),
                              /*inside_self=*/true);

    REQUIRE(accepted_changes.image_packs.has_value());
    REQUIRE(accepted_changes.image_packs->packs.size() == 1);
    CHECK(accepted_changes.image_packs->packs[0].display_name == "New Pack");
}

TEST_CASE("RoomSettingsView: dispatch_file_drop reaches the image pack "
          "editor only when the Emojis & Stickers tab is open and selected",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_image_pack_field_permissions(true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Switch to the Emojis & Stickers tab and create one pack via the UI
    // (mirrors the "on_accept's changes.image_packs" test's geometry) so
    // there's an active pack for the drop to fall back onto.
    const tk::Point tab_pt{100.0f, 211.0f};
    tk::Widget* tab_hit = v.dispatch_pointer_down(tab_pt);
    REQUIRE(tab_hit != nullptr);
    tab_hit->on_pointer_up(tab_hit->world_to_local(tab_pt), /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    v.image_pack_editor()->set_new_pack_name_text("New Pack");
    const tk::Point create_pt{732.0f, 101.0f};
    tk::Widget* create_hit = v.dispatch_pointer_down(create_pt);
    REQUIRE(create_hit != nullptr);
    create_hit->on_pointer_up(create_hit->world_to_local(create_pt),
                              /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    REQUIRE(v.image_pack_editor()->packs().size() == 1);

    const tk::Rect list_rect = v.image_pack_list_rect();
    REQUIRE_FALSE(list_rect.empty());
    const tk::Point drop_pt{list_rect.x + list_rect.w * 0.5f, list_rect.y + 10.0f};

    tk::FileDropPayload payload{{1, 2, 3}, "image/png", ""};
    tk::Widget* target = v.dispatch_file_drop(drop_pt, payload);
    CHECK(target == v.image_pack_editor());
    CHECK(v.image_pack_editor()->packs()[0].images.size() == 1);
    CHECK(payload.bytes.empty()); // accepted — moved out

    // Switch to a different tab (General, index 0; row center y = 49 +
    // (0 + 0.5) * 36 = 67, per the sidebar tab-row geometry derivation
    // above) — the same point must no longer reach the (now invisible)
    // image pack editor.
    const tk::Point general_tab_pt{100.0f, 67.0f};
    tk::Widget* general_hit = v.dispatch_pointer_down(general_tab_pt);
    REQUIRE(general_hit != nullptr);
    general_hit->on_pointer_up(general_hit->world_to_local(general_tab_pt),
                               /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    tk::FileDropPayload payload2{{4, 5, 6}, "image/png", ""};
    tk::Widget* target2 = v.dispatch_file_drop(drop_pt, payload2);
    CHECK(target2 == nullptr);
    CHECK(v.image_pack_editor()->packs()[0].images.size() == 1); // unchanged
    CHECK_FALSE(payload2.bytes.empty()); // rejected — untouched

    // Closing the whole dialog must also stop the drop from reaching it.
    v.close();
    tk::FileDropPayload payload3{{7, 8, 9}, "image/png", ""};
    tk::Widget* target3 = v.dispatch_file_drop(drop_pt, payload3);
    CHECK(target3 == nullptr);
}

TEST_CASE("RoomSettingsView: set_image_pack_field_permissions forwards to "
          "the Emojis & Stickers tab, and defaults to read-only",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Switch to the Emojis & Stickers tab (see the footer test above for
    // the sidebar tab-row geometry derivation).
    const tk::Point tab_pt{100.0f, 211.0f};
    tk::Widget* tab_hit = v.dispatch_pointer_down(tab_pt);
    REQUIRE(tab_hit != nullptr);
    tab_hit->on_pointer_up(tab_hit->world_to_local(tab_pt), /*inside_self=*/true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Read-only by default, before the host ever calls this.
    CHECK(v.image_pack_new_pack_name_field_rect().empty());

    v.set_image_pack_field_permissions(true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    CHECK_FALSE(v.image_pack_new_pack_name_field_rect().empty());

    v.set_image_pack_field_permissions(false);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    CHECK(v.image_pack_new_pack_name_field_rect().empty());
}

TEST_CASE("RoomSettingsView: set_commit_result(true) closes the view",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    CHECK(v.is_open());

    v.set_commit_result(true, "");
    CHECK_FALSE(v.is_open());
}

TEST_CASE("RoomSettingsView: set_commit_result(false) keeps the view open",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());

    v.set_commit_result(false, "M_FORBIDDEN");
    CHECK(v.is_open());

    // Should still be paintable/interactable after a failed commit.
    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: re-opening reseeds staged state from scratch",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);
    REQUIRE_FALSE(host.fields_created.empty());
    host.fields_created[0]->on_changed("Edited but not accepted");

    // Switching rooms (or reopening the same room) reseeds staged_* from the
    // fresh RoomInfo, discarding any unsaved edit.
    tesseract::RoomInfo other;
    other.id         = "!other:example.org";
    other.name       = "Other Room";
    other.topic      = "Other Topic";
    other.avatar_url = "mxc://example.org/other";
    v.open(other);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    v.set_field_permissions(true, true, true);

    int accept_count = 0;
    tesseract::views::RoomSettingsChanges accepted_changes;
    v.on_accept = [&](std::string, tesseract::views::RoomSettingsChanges changes) {
        ++accept_count;
        accepted_changes = std::move(changes);
    };
    const tk::Point pt{730.0f, 558.0f};
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);
    hit->on_pointer_up(hit->world_to_local(pt), /*inside_self=*/true);

    REQUIRE(accept_count == 1);
    // Nothing was edited on the new room, so nothing should be reported as
    // changed even though the first room's staged name was "dirty".
    CHECK_FALSE(accepted_changes.name.has_value());
}

TEST_CASE("RoomSettingsView: paints without crashing across states",
          "[room_settings][view]")
{
    TkRoomSettingsViewStage st;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;

    // Closed
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Open, no permissions yet
    v.open(make_room_info());
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Fully permitted
    v.set_field_permissions(true, true, true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Avatar busy / error
    v.set_avatar_busy(true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    v.set_avatar_busy(false);
    v.set_avatar_error("Upload failed");
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Commit failure error banner
    v.set_commit_result(false, "M_FORBIDDEN");
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Permissions tab granted + a non-default power-levels state.
    v.set_permissions_field_permissions(true);
    tesseract::RoomPermissions perms;
    perms.kick_users = 100;
    v.set_permissions_state(perms);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: clicking Room ID fires on_copy_to_clipboard",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Room ID row: with an 800x600 stage, tabs_ occupies {0,49,800,487}
    // (kBarHeight=48 + 1px separator to kFooterH-from-bottom=64), SideTabView
    // reserves a 200px sidebar, and RoomGeneralSection::Content lays out
    // Name -> Topic -> Room Address -> Room ID top to bottom, giving
    // roomid_rect_ = {344, 267, 432, 26}.
    std::string copied;
    int copy_count = 0;
    v.on_copy_to_clipboard = [&](std::string text)
    {
        ++copy_count;
        copied = std::move(text);
    };

    const tk::Point pt{400.0f, 275.0f};
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);

    CHECK(copy_count == 1);
    CHECK(copied == "!room:example.org");
}

TEST_CASE("RoomSettingsView: room_id() reflects the open()'d room",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    CHECK(v.room_id().empty());
    v.open(make_room_info());
    CHECK(v.room_id() == "!room:example.org");
}

TEST_CASE("RoomSettingsView: set_media_override doesn't disturb General's "
          "staged state",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.name_field() != nullptr);
    REQUIRE(v.topic_field() != nullptr);
    const tk::Rect name_before = v.name_field()->bounds();
    const tk::Rect topic_before = v.topic_field()->bounds();
    const std::string name_text_before = v.name_field()->text();
    const std::string topic_text_before = v.topic_field()->text();

    v.set_media_override(true, tesseract::MediaPreviewConfig::Mode::Off);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK(v.name_field()->bounds().x == name_before.x);
    CHECK(v.name_field()->bounds().y == name_before.y);
    CHECK(v.topic_field()->bounds().x == topic_before.x);
    CHECK(v.topic_field()->bounds().y == topic_before.y);
    CHECK(v.name_field()->text() == name_text_before);
    CHECK(v.topic_field()->text() == topic_text_before);

    // Paints without crashing with the Media tab in a non-default state too.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: set_permissions_state doesn't disturb General's "
          "staged state",
          "[room_settings][view]")
{
    StubHost host;
    auto v_owner = tk::create_root_widget<RoomSettingsView>(&host);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.name_field() != nullptr);
    const tk::Rect name_before = v.name_field()->bounds();
    const std::string name_text_before = v.name_field()->text();

    tesseract::RoomPermissions perms;
    perms.kick_users = 100;
    v.set_permissions_field_permissions(true);
    v.set_permissions_state(perms);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK(v.name_field()->bounds().x == name_before.x);
    CHECK(v.name_field()->bounds().y == name_before.y);
    CHECK(v.name_field()->text() == name_text_before);
}

TEST_CASE("RoomSettingsView: re-opening reseeds permissions to spec defaults",
          "[room_settings][view]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    tesseract::RoomPermissions perms;
    perms.kick_users = 100;
    v.set_permissions_field_permissions(true);
    v.set_permissions_state(perms);

    // Switching rooms reseeds the Permissions tab to the spec-default
    // placeholder — ShellBase's on_room_settings_opened handler is
    // responsible for calling set_permissions_state() again with the new
    // room's actual levels right after this.
    tesseract::RoomInfo other;
    other.id   = "!other:example.org";
    other.name = "Other Room";
    v.open(other);

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: a staged Permissions change that would lock "
          "the user out disables Accept and shows the warning",
          "[room_settings][view][lockout]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_permissions_field_permissions(true);
    v.set_permissions_state(tesseract::RoomPermissions{});
    // Explicit override at 50 — meets the default change_permissions
    // requirement (50), so Accept starts out enabled.
    v.set_own_power_level(
        tesseract::RoomOwnPowerLevel{.level = 50, .has_explicit_override = true});

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    REQUIRE(v.accept_button()->enabled());

    // Raise "Change permissions" above the user's own (overridden) level.
    v.permissions_section()->change_permissions_combo()->on_changed("100");
    CHECK_FALSE(v.accept_button()->enabled());
    CHECK(v.permissions_section()->lockout_warning()->visible());

    // Revert — Accept re-enables and the warning hides.
    v.permissions_section()->change_permissions_combo()->on_changed("50");
    CHECK(v.accept_button()->enabled());
    CHECK_FALSE(v.permissions_section()->lockout_warning()->visible());
}

TEST_CASE("RoomSettingsView: lowering Default Role below the requirement "
          "locks out a user with no explicit power-level override",
          "[room_settings][view][lockout]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    v.set_permissions_field_permissions(true);
    tesseract::RoomPermissions perms;
    perms.default_role       = 50;
    perms.change_permissions = 50;
    v.set_permissions_state(perms);
    v.set_own_power_level(
        tesseract::RoomOwnPowerLevel{.level = 0, .has_explicit_override = false});

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    REQUIRE(v.accept_button()->enabled());

    v.permissions_section()->default_role_combo()->on_changed("0");
    CHECK_FALSE(v.accept_button()->enabled());
}

TEST_CASE("RoomSettingsView: no lockout warning or Accept-disable when the "
          "user can't edit permissions at all (own level already below the "
          "requirement, unrelated to any staged change)",
          "[room_settings][view][lockout]")
{
    auto v_owner = tk::create_root_widget<RoomSettingsView>(nullptr);
    RoomSettingsView& v = *v_owner;
    v.open(make_room_info());
    // A moderator (level 50) in a room requiring 100 to edit permissions —
    // set_permissions_field_permissions(false) mirrors
    // Client::can_set_room_power_levels() returning false for them.
    v.set_permissions_field_permissions(false);
    tesseract::RoomPermissions perms;
    perms.change_permissions = 100;
    v.set_permissions_state(perms);
    v.set_own_power_level(
        tesseract::RoomOwnPowerLevel{.level = 50, .has_explicit_override = true});

    TkRoomSettingsViewStage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Nothing was staged, and the user has no ability to edit permissions
    // in the first place — the warning would be redundant with the
    // already-disabled combos, and disabling Accept would wrongly block
    // unrelated General/Security changes too.
    CHECK_FALSE(v.permissions_section()->lockout_warning()->visible());
    CHECK(v.accept_button()->enabled());
}
