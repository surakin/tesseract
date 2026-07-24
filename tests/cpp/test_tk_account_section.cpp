#include <catch2/catch_test_macros.hpp>

#include "views/settings/AccountSection.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using tesseract::views::AccountSection;

namespace
{

struct TkAccountSectionStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 200);
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

TEST_CASE("AccountSection: name_field_rect empty when not editable",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_user_id("@alice:example.org");

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // Not editable by default -> empty rect
    CHECK(sec.name_field_rect().empty());
}

TEST_CASE("AccountSection: name_field_rect non-empty when editable",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_user_id("@alice:example.org");
    sec.set_editable(true);

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    CHECK(!sec.name_field_rect().empty());
}

TEST_CASE("AccountSection: name_field_rect empty when busy",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_editable(true);
    sec.set_name_busy(true);

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    CHECK(sec.name_field_rect().empty());
}

TEST_CASE("AccountSection: paints without crash in all edit states",
          "[account][section]")
{
    TkAccountSectionStage st;
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_user_id("@alice:example.org");

    // non-editable
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // editable + error
    sec.set_editable(true);
    sec.set_name_error("Server refused");
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // busy
    sec.set_name_busy(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // avatar editable
    sec.set_name_busy(false);
    sec.set_avatar_editable(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // avatar busy
    sec.set_avatar_busy(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // avatar error
    sec.set_avatar_busy(false);
    sec.set_avatar_error("Upload failed");
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});
}

TEST_CASE("AccountSection: avatar upload callback fires on disc click",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_avatar_editable(true);

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    int upload_count = 0;
    sec.on_avatar_upload_clicked = [&]{ ++upload_count; };

    // Click centre of disc (kPadX + kAvatarDiameter/2 = 24 + 32 = 56, kPadY + kAvatarRadius = 56).
    // Use dispatch_pointer_down so the click walks into the section's bespoke
    // Content child — matches how the host delivers real pointer events.
    sec.dispatch_pointer_down({56.0f, 56.0f});
    CHECK(upload_count == 1);
}

TEST_CASE("AccountSection: remove callback fires on X chip click",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_avatar_url("mxc://example.org/avatar");
    sec.set_avatar_editable(true);

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    int remove_count = 0;
    sec.on_avatar_remove_clicked = [&]{ ++remove_count; };

    // The X chip sits at the top-right of the disc.
    // disc centre ~= (56, 56), radius = 32 -> top-right corner ~= (75, 37)
    sec.dispatch_pointer_down({75.0f, 37.0f});
    CHECK(remove_count == 1);
}

TEST_CASE("AccountSection: avatar click ignored when not editable",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    // avatar_editable_ defaults to false

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    int upload_count = 0;
    sec.on_avatar_upload_clicked = [&]{ ++upload_count; };
    sec.dispatch_pointer_down({56.0f, 56.0f});
    CHECK(upload_count == 0);
}

TEST_CASE("AccountSection: avatar click ignored when busy",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_avatar_editable(true);
    sec.set_avatar_busy(true);

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    int upload_count = 0;
    sec.on_avatar_upload_clicked = [&]{ ++upload_count; };
    sec.dispatch_pointer_down({56.0f, 56.0f});
    CHECK(upload_count == 0);
}

TEST_CASE("AccountSection: extended profile fields hidden until server "
          "support confirmed",
          "[account][section]")
{
    auto sec_owner = tk::create_root_widget<AccountSection>(nullptr);
    AccountSection& sec = *sec_owner;
    sec.set_display_name("Alice");
    sec.set_user_id("@alice:example.org");

    TkAccountSectionStage st;
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});

    // Not yet told the server supports MSC4133 -> the whole Pronouns/
    // Timezone/Bio block is absent, not merely non-editable.
    const float hidden_h = sec.content_height_for_testing();

    sec.set_profile_fields_editable(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});
    const float shown_h = sec.content_height_for_testing();

    CHECK(shown_h > hidden_h);

    // Server later reports no/disabled support -> collapses back to zero
    // extra height, not just non-interactive.
    sec.set_profile_fields_editable(false);
    st.run(sec, {0.0f, 0.0f, 640.0f, 200.0f});
    CHECK(sec.content_height_for_testing() == hidden_h);
}
