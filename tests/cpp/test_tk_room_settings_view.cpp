#include <catch2/catch_test_macros.hpp>

#include "views/RoomSettingsView.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

using tesseract::views::RoomSettingsView;

namespace
{

struct Stage
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
    RoomSettingsView v;
    CHECK_FALSE(v.is_open());
    CHECK(v.name_field_rect().empty());
    CHECK(v.topic_edit_rect().empty());
}

TEST_CASE("RoomSettingsView: open() seeds state and opens the view",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    CHECK(v.is_open());
}

TEST_CASE("RoomSettingsView: close() closes the view",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.close();
    CHECK_FALSE(v.is_open());
}

TEST_CASE("RoomSettingsView: field rects empty until permissions granted",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // No set_field_permissions call yet -> every field denied by default.
    CHECK(v.name_field_rect().empty());
    CHECK(v.topic_edit_rect().empty());
}

TEST_CASE("RoomSettingsView: per-field permission gating",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(/*can_name=*/true, /*can_topic=*/false,
                            /*can_avatar=*/false);

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK_FALSE(v.name_field_rect().empty());
    CHECK(v.topic_edit_rect().empty());
}

TEST_CASE("RoomSettingsView: both fields editable when both permitted",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK_FALSE(v.name_field_rect().empty());
    CHECK_FALSE(v.topic_edit_rect().empty());
}

TEST_CASE("RoomSettingsView: on_cancel fires and closes nothing by itself",
          "[room_settings][view]")
{
    // Cancel is wired by the shell to call close(); the view itself only
    // fires the callback (mirrors ConfirmDialog's cancel_btn_ contract).
    RoomSettingsView v;
    v.open(make_room_info());

    Stage st;
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
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    v.set_name_edit_text("New Name");
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
}

TEST_CASE("RoomSettingsView: set_commit_result(true) closes the view",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    CHECK(v.is_open());

    v.set_commit_result(true, "");
    CHECK_FALSE(v.is_open());
}

TEST_CASE("RoomSettingsView: set_commit_result(false) keeps the view open",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());

    v.set_commit_result(false, "M_FORBIDDEN");
    CHECK(v.is_open());

    // Should still be paintable/interactable after a failed commit.
    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: re-opening reseeds staged state from scratch",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);
    v.set_name_edit_text("Edited but not accepted");

    // Switching rooms (or reopening the same room) reseeds staged_* from the
    // fresh RoomInfo, discarding any unsaved edit.
    tesseract::RoomInfo other;
    other.id         = "!other:example.org";
    other.name       = "Other Room";
    other.topic      = "Other Topic";
    other.avatar_url = "mxc://example.org/other";
    v.open(other);

    Stage st;
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
    Stage st;
    RoomSettingsView v;

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
    RoomSettingsView v;
    v.open(make_room_info());

    Stage st;
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
    RoomSettingsView v;
    CHECK(v.room_id().empty());
    v.open(make_room_info());
    CHECK(v.room_id() == "!room:example.org");
}

TEST_CASE("RoomSettingsView: set_media_override doesn't disturb General's "
          "staged state",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const tk::Rect name_before = v.name_field_rect();
    const tk::Rect topic_before = v.topic_edit_rect();
    const std::string name_text_before = v.name_edit_initial_text();
    const std::string topic_text_before = v.topic_edit_initial_text();

    v.set_media_override(true, tesseract::MediaPreviewConfig::Mode::Off);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK(v.name_field_rect().x == name_before.x);
    CHECK(v.name_field_rect().y == name_before.y);
    CHECK(v.topic_edit_rect().x == topic_before.x);
    CHECK(v.topic_edit_rect().y == topic_before.y);
    CHECK(v.name_edit_initial_text() == name_text_before);
    CHECK(v.topic_edit_initial_text() == topic_text_before);

    // Paints without crashing with the Media tab in a non-default state too.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSettingsView: set_permissions_state doesn't disturb General's "
          "staged state",
          "[room_settings][view]")
{
    RoomSettingsView v;
    v.open(make_room_info());
    v.set_field_permissions(true, true, true);

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const tk::Rect name_before = v.name_field_rect();
    const std::string name_text_before = v.name_edit_initial_text();

    tesseract::RoomPermissions perms;
    perms.kick_users = 100;
    v.set_permissions_field_permissions(true);
    v.set_permissions_state(perms);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    CHECK(v.name_field_rect().x == name_before.x);
    CHECK(v.name_field_rect().y == name_before.y);
    CHECK(v.name_edit_initial_text() == name_text_before);
}

TEST_CASE("RoomSettingsView: re-opening reseeds permissions to spec defaults",
          "[room_settings][view]")
{
    RoomSettingsView v;
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

    Stage st;
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}
