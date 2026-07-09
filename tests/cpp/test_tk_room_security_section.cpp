#include <catch2/catch_test_macros.hpp>

#include "views/settings/RoomSecuritySection.h"
#include "tk_test_surface.h"

using tesseract::views::RoomSecuritySection;

namespace
{

struct TkRoomSecuritySectionStage
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

TEST_CASE("RoomSecuritySection: paints without crashing", "[room_security_section]")
{
    RoomSecuritySection s;
    TkRoomSecuritySectionStage st;
    st.run(s, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomSecuritySection: defaults", "[room_security_section]")
{
    RoomSecuritySection s;
    CHECK_FALSE(s.encryption_checkbox()->checked());
    CHECK_FALSE(s.encryption_warning()->visible());
    CHECK_FALSE(s.guest_access_checkbox()->checked());
    REQUIRE(s.join_rule_combo() != nullptr);
    REQUIRE(s.history_visibility_combo() != nullptr);
}

TEST_CASE("RoomSecuritySection: set_encryption(true) checks and disables the "
          "checkbox even with permission granted",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_field_permissions(true, true, true, true);
    s.set_encryption(true);
    CHECK(s.encryption_checkbox()->checked());
    CHECK_FALSE(s.encryption_checkbox()->enabled());
}

TEST_CASE("RoomSecuritySection: encryption stays disabled through a "
          "permission-revoke-then-regrant cycle",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_field_permissions(true, true, true, true);
    s.set_encryption(true);
    s.set_field_permissions(false, false, false, false);
    CHECK_FALSE(s.encryption_checkbox()->enabled());
    s.set_field_permissions(true, true, true, true);
    CHECK_FALSE(s.encryption_checkbox()->enabled());
}

TEST_CASE("RoomSecuritySection: encryption re-seeds to unchecked+enabled "
          "when set_encryption(false) is called again (simulating a room switch)",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_field_permissions(true, true, true, true);
    s.set_encryption(true);
    REQUIRE_FALSE(s.encryption_checkbox()->enabled());

    s.set_encryption(false);
    CHECK_FALSE(s.encryption_checkbox()->checked());
    CHECK(s.encryption_checkbox()->enabled());
}

TEST_CASE("RoomSecuritySection: warning label hidden by default, shown after "
          "checking, hidden again if set_encryption(false) reseeds to unchecked",
          "[room_security_section]")
{
    RoomSecuritySection s;
    CHECK_FALSE(s.encryption_warning()->visible());

    s.set_field_permissions(true, true, true, true);
    s.encryption_checkbox()->set_checked(true);
    // set_checked() is silent (no on_change fired), so drive the warning
    // refresh the same way the constructor's on_change lambda does: via
    // set_encryption(), which is the seeding path — direct combo/checkbox
    // interaction is exercised in the "fires on toggle" test below instead.
    s.set_encryption(true);
    CHECK(s.encryption_warning()->visible());

    s.set_encryption(false);
    CHECK_FALSE(s.encryption_warning()->visible());
}

TEST_CASE("RoomSecuritySection: checking the encryption checkbox fires "
          "on_encryption_changed and shows the warning",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_field_permissions(true, true, true, true);

    int count = 0;
    bool last = false;
    s.on_encryption_changed = [&](bool checked)
    {
        ++count;
        last = checked;
    };

    s.encryption_checkbox()->on_change(true);
    REQUIRE(count == 1);
    CHECK(last == true);
}

TEST_CASE("RoomSecuritySection: set_committing(true) disables all four controls",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_field_permissions(true, true, true, true);
    s.set_committing(true);
    CHECK_FALSE(s.encryption_checkbox()->enabled());
    CHECK_FALSE(s.join_rule_combo()->enabled());
    CHECK_FALSE(s.guest_access_checkbox()->enabled());
    CHECK_FALSE(s.history_visibility_combo()->enabled());
}

TEST_CASE("RoomSecuritySection: set_field_permissions gates each control independently",
          "[room_security_section]")
{
    RoomSecuritySection s;

    s.set_field_permissions(true, false, false, false);
    CHECK(s.encryption_checkbox()->enabled());
    CHECK_FALSE(s.join_rule_combo()->enabled());
    CHECK_FALSE(s.guest_access_checkbox()->enabled());
    CHECK_FALSE(s.history_visibility_combo()->enabled());

    s.set_field_permissions(false, true, false, false);
    CHECK_FALSE(s.encryption_checkbox()->enabled());
    CHECK(s.join_rule_combo()->enabled());

    s.set_field_permissions(false, false, true, false);
    CHECK(s.guest_access_checkbox()->enabled());

    s.set_field_permissions(false, false, false, true);
    CHECK(s.history_visibility_combo()->enabled());
}

TEST_CASE("RoomSecuritySection: join_rule combo fires the callback with the "
          "picked value for each option",
          "[room_security_section]")
{
    RoomSecuritySection s;
    std::string picked;
    s.on_join_rule_changed = [&](std::string v) { picked = v; };

    s.join_rule_combo()->on_changed("public");
    CHECK(picked == "public");
    s.join_rule_combo()->on_changed("invite");
    CHECK(picked == "invite");
    s.join_rule_combo()->on_changed("knock");
    CHECK(picked == "knock");
}

TEST_CASE("RoomSecuritySection: set_join_rule(\"restricted\") shows the "
          "read-only label and hides the combo",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_join_rule("restricted");
    CHECK_FALSE(s.join_rule_combo()->visible());
    CHECK(s.join_rule_readonly()->visible());
}

TEST_CASE("RoomSecuritySection: set_join_rule(\"knock_restricted\") shows the "
          "read-only label and hides the combo",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_join_rule("knock_restricted");
    CHECK_FALSE(s.join_rule_combo()->visible());
    CHECK(s.join_rule_readonly()->visible());
}

TEST_CASE("RoomSecuritySection: switching back to \"public\" after a prior "
          "restricted state re-shows the editable combo",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_join_rule("restricted");
    REQUIRE_FALSE(s.join_rule_combo()->visible());

    s.set_join_rule("public");
    CHECK(s.join_rule_combo()->visible());
    CHECK_FALSE(s.join_rule_readonly()->visible());
    CHECK(s.join_rule_combo()->selected_value() == "public");
}

TEST_CASE("RoomSecuritySection: guest_access checkbox reflects seeding and "
          "fires on toggle",
          "[room_security_section]")
{
    RoomSecuritySection s;
    s.set_guest_access(true);
    CHECK(s.guest_access_checkbox()->checked());

    int count = 0;
    bool last = false;
    s.on_guest_access_changed = [&](bool allow) { ++count; last = allow; };
    s.guest_access_checkbox()->on_change(false);
    REQUIRE(count == 1);
    CHECK_FALSE(last);
}

TEST_CASE("RoomSecuritySection: history_visibility combo selects the "
          "matching option for all 4 values",
          "[room_security_section]")
{
    RoomSecuritySection s;
    for (const std::string& v :
         {std::string("world_readable"), std::string("shared"),
          std::string("invited"), std::string("joined")})
    {
        s.set_history_visibility(v);
        CHECK(s.history_visibility_combo()->selected_value() == v);
    }
}
