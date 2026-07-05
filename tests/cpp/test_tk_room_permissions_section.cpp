#include <catch2/catch_test_macros.hpp>

#include "views/settings/RoomPermissionsSection.h"
#include "tk_test_surface.h"

using tesseract::views::RoomPermissionsSection;
using tesseract::RoomPermissions;

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

} // namespace

TEST_CASE("RoomPermissionsSection: paints without crashing", "[room_permissions_section]")
{
    RoomPermissionsSection s;
    Stage st;
    st.run(s, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomPermissionsSection: set_permissions seeds all 9 combos to matching presets",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    RoomPermissions p;
    p.default_role       = 0;
    p.send_messages      = 0;
    p.remove_messages    = 50;
    p.invite_users       = 0;
    p.kick_users         = 50;
    p.ban_users          = 50;
    p.change_settings    = 50;
    p.change_permissions = 100;
    p.notify_everyone    = 50;
    s.set_permissions(p);

    CHECK(s.default_role_combo()->selected_value() == "0");
    CHECK(s.send_messages_combo()->selected_value() == "0");
    CHECK(s.remove_messages_combo()->selected_value() == "50");
    CHECK(s.invite_users_combo()->selected_value() == "0");
    CHECK(s.kick_users_combo()->selected_value() == "50");
    CHECK(s.ban_users_combo()->selected_value() == "50");
    CHECK(s.change_settings_combo()->selected_value() == "50");
    CHECK(s.change_permissions_combo()->selected_value() == "100");
    CHECK(s.notify_everyone_combo()->selected_value() == "50");
}

TEST_CASE("RoomPermissionsSection: a non-preset value synthesizes and selects "
          "a Custom(N) option",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    RoomPermissions p;
    p.kick_users = 30;
    s.set_permissions(p);
    CHECK(s.kick_users_combo()->selected_value() == "30");
}

TEST_CASE("RoomPermissionsSection: re-seeding with a preset value removes the "
          "stale Custom option",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    RoomPermissions p;
    p.kick_users = 30;
    s.set_permissions(p);
    REQUIRE(s.kick_users_combo()->selected_value() == "30");

    p.kick_users = 50;
    s.set_permissions(p);
    CHECK(s.kick_users_combo()->selected_value() == "50");
}

TEST_CASE("RoomPermissionsSection: set_field_permissions(false) disables all "
          "9 combos",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    s.set_field_permissions(false);
    CHECK_FALSE(s.default_role_combo()->enabled());
    CHECK_FALSE(s.send_messages_combo()->enabled());
    CHECK_FALSE(s.remove_messages_combo()->enabled());
    CHECK_FALSE(s.invite_users_combo()->enabled());
    CHECK_FALSE(s.kick_users_combo()->enabled());
    CHECK_FALSE(s.ban_users_combo()->enabled());
    CHECK_FALSE(s.change_settings_combo()->enabled());
    CHECK_FALSE(s.change_permissions_combo()->enabled());
    CHECK_FALSE(s.notify_everyone_combo()->enabled());
}

TEST_CASE("RoomPermissionsSection: set_field_permissions(true) enables all 9 "
          "combos when not committing",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    s.set_field_permissions(true);
    CHECK(s.default_role_combo()->enabled());
    CHECK(s.notify_everyone_combo()->enabled());
}

TEST_CASE("RoomPermissionsSection: set_committing(true) disables all 9 combos "
          "regardless of field permissions",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    s.set_field_permissions(true);
    s.set_committing(true);
    CHECK_FALSE(s.default_role_combo()->enabled());
    CHECK_FALSE(s.kick_users_combo()->enabled());
    CHECK_FALSE(s.notify_everyone_combo()->enabled());
}

TEST_CASE("RoomPermissionsSection: each combo's on_changed fires "
          "on_permissions_changed with the full updated struct",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    int count = 0;
    RoomPermissions last;
    s.on_permissions_changed = [&](RoomPermissions p)
    {
        ++count;
        last = p;
    };

    s.kick_users_combo()->on_changed("100");
    REQUIRE(count == 1);
    CHECK(last.kick_users == 100);
    // Only the changed field should differ from the default struct.
    CHECK(last.default_role == RoomPermissions{}.default_role);
    CHECK(last.ban_users == RoomPermissions{}.ban_users);
}

TEST_CASE("RoomPermissionsSection: lockout warning hidden by default",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    CHECK_FALSE(s.lockout_warning()->visible());
}

TEST_CASE("RoomPermissionsSection: set_would_lock_out_self(true) shows the "
          "warning, (false) hides it again",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    s.set_would_lock_out_self(true);
    CHECK(s.lockout_warning()->visible());

    s.set_would_lock_out_self(false);
    CHECK_FALSE(s.lockout_warning()->visible());
}

TEST_CASE("RoomPermissionsSection: set_would_lock_out_self fires "
          "on_layout_changed only when visibility actually flips",
          "[room_permissions_section]")
{
    RoomPermissionsSection s;
    int count = 0;
    s.on_layout_changed = [&]() { ++count; };

    s.set_would_lock_out_self(true);
    CHECK(count == 1);
    s.set_would_lock_out_self(true);
    CHECK(count == 1);
    s.set_would_lock_out_self(false);
    CHECK(count == 2);
}
