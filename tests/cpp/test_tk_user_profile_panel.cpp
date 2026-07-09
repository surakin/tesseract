#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/UserProfilePanel.h"
#include "tk_test_surface.h"

#include <string>

using namespace tk;
using tesseract::views::UserProfilePanel;

namespace
{
struct TkUserProfilePanelStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 400);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};
} // namespace

TEST_CASE("UserProfilePanel: Normal state shows Message label",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Message");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: HasDM state shows Open DM label",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::HasDM);
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Open DM");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: Sending state disables button",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::Sending);
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Sending\xe2\x80\xa6"); // UTF-8 ellipsis
    CHECK_FALSE(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: set_dm_button_state can cycle back to Normal",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::Sending);
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::Normal);
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Message");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: on_check_has_dm returning true sets HasDM on open",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.on_check_has_dm = [](const std::string&) { return true; };
    panel.open("@alice:example.org", "Alice", "");
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Open DM");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: on_check_has_dm returning false sets Normal on open",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.on_check_has_dm = [](const std::string&) { return false; };
    panel.open("@alice:example.org", "Alice", "");
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Message");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: open resets Sending state from a prior open",
          "[tk][view][user_profile_panel]")
{
    // If panel was left in Sending state (async still in flight for another
    // user), opening it for a new user must reset to Normal/HasDM.
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::Sending);

    panel.open("@bob:example.org", "Bob", ""); // new user
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Message");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: paints without crash in each DmButtonState",
          "[tk][view][user_profile_panel]")
{
    for (auto state : {UserProfilePanel::DmButtonState::Normal,
                       UserProfilePanel::DmButtonState::HasDM,
                       UserProfilePanel::DmButtonState::Sending})
    {
        TkUserProfilePanelStage st;
        UserProfilePanel panel;
        panel.open("@alice:example.org", "Alice", "");
        panel.set_dm_button_state(state);
        st.run(panel, {0, 0, 300, 400});
        SUCCEED();
    }
}
