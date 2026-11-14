#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/UserProfilePanel.h"
#include "tk_test_surface.h"

#include <string>
#include <vector>

using namespace tk;
using tesseract::views::UserProfilePanel;

namespace
{
struct TkUserProfilePanelStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 400);
    LayoutCtx layout_ctx() const
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx() const
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds) const
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};
} // namespace

TEST_CASE("UserProfilePanel: Normal state shows Message label", "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Message");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: HasDM state shows Open DM label", "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    panel.set_dm_button_state(UserProfilePanel::DmButtonState::HasDM);
    st.run(panel, {0, 0, 300, 400});
    CHECK(panel.dm_button_label() == "Open DM");
    CHECK(panel.dm_button_enabled());
}

TEST_CASE("UserProfilePanel: Sending state disables button", "[tk][view][user_profile_panel]")
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
    panel.on_check_has_dm = [](const std::string&)
    {
        return true;
    };
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
    panel.on_check_has_dm = [](const std::string&)
    {
        return false;
    };
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
    for (auto state :
         {UserProfilePanel::DmButtonState::Normal, UserProfilePanel::DmButtonState::HasDM,
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

TEST_CASE("UserProfilePanel: paints with each UserTrust", "[tk][view][user_profile_panel]")
{
    using tesseract::UserTrust;
    for (auto trust : {UserTrust::Unknown, UserTrust::Verified, UserTrust::NotVerified,
                       UserTrust::VerificationViolation})
    {
        for (bool can_verify : {false, true})
        {
            TkUserProfilePanelStage st;
            UserProfilePanel panel;
            panel.open("@alice:example.org", "Alice", "");
            panel.set_trust(trust, can_verify);
            st.run(panel, {0, 0, 600, 500});
            CHECK(panel.dm_button_label() == "Message");
        }
    }
}

TEST_CASE("UserProfilePanel: paints with extended profile fields", "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    panel.open("@alice:example.org", "Alice", "");
    tesseract::ExtendedProfile ext;
    ext.tz = "Europe/Oslo";
    ext.biography = "A fairly long biography that has to wrap across several lines "
                    "of the narrower right column of the card.";
    ext.status_emoji = "ð";
    ext.status_text = "Working";
    panel.set_extended_profile(ext);
    panel.set_trust(tesseract::UserTrust::Verified, true);
    st.run(panel, {0, 0, 600, 500});
    SUCCEED();
}

TEST_CASE("UserProfilePanel: common rooms hook", "[tk][view][user_profile_panel]")
{
    // Declared before the panel: it caches layouts from the stage's factory.
    TkUserProfilePanelStage st;
    std::string requested;
    UserProfilePanel panel;
    panel.on_common_rooms_requested = [&](std::string id)
    {
        requested = std::move(id);
    };
    panel.open("@alice:example.org", "Alice", "");
    CHECK(requested == "@alice:example.org");

    auto make_rooms = [](int n)
    {
        std::vector<UserProfilePanel::CommonRoom> rooms;
        rooms.reserve(n);
        for (int i = 0; i < n; ++i)
        {
            rooms.push_back({"!r" + std::to_string(i) + ":example.org",
                             i % 3 == 0 ? std::string() : "Room " + std::to_string(i), ""});
        }
        return rooms;
    };

    for (int n : {0, 1, 40})
    {
        for (bool supported : {false, true})
        {
            panel.set_common_rooms_supported(supported);
            panel.set_common_rooms(make_rooms(n), n, n > 20);
            CHECK(panel.common_rooms().size() == static_cast<std::size_t>(n));
            st.run(panel, {0, 0, 600, 500});
        }
    }

    // Reopening for another user clears the previous user's rooms and re-requests.
    panel.open("@bob:example.org", "Bob", "");
    CHECK(requested == "@bob:example.org");
    CHECK(panel.common_rooms().empty());
}

TEST_CASE("UserProfilePanel: common room avatars are requested once while unloaded",
          "[tk][view][user_profile_panel]")
{
    TkUserProfilePanelStage st;
    UserProfilePanel panel;
    std::vector<std::string> needed;
    panel.on_common_room_avatar_needed = [&](std::string id)
    {
        needed.push_back(std::move(id));
    };
    panel.set_common_room_avatar_provider(
        [](const std::string&) -> const tk::Image*
        {
            return nullptr;
        });
    panel.open("@alice:example.org", "Alice", "");
    panel.set_common_rooms_supported(true);
    panel.set_common_rooms(
        {{"!a:example.org", "A", "mxc://example.org/a"}, {"!b:example.org", "B", ""}}, 2, false);

    st.run(panel, {0, 0, 600, 500});
    st.run(panel, {0, 0, 600, 500});
    // Only the room with an avatar URL asks, and only once across repaints.
    CHECK(needed == std::vector<std::string>{"!a:example.org"});

    // A fresh open() re-arms the request.
    panel.open("@bob:example.org", "Bob", "");
    panel.set_common_rooms({{"!a:example.org", "A", "mxc://example.org/a"}}, 1, false);
    st.run(panel, {0, 0, 600, 500});
    CHECK(needed.size() == 2);
}
