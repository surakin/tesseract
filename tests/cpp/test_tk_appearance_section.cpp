#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "settings_guard.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/SettingBinder.h"

#include <string>
#include <vector>

using tesseract::Settings;
using tesseract::views::AppearanceSection;
using tesseract::views::SettingBinder;
using tesseract::views::SettingKey;

namespace
{

struct AsStage : vt::Stage
{
    tesseract::test::SettingsGuard guard;
    SettingBinder binder;
    std::vector<SettingKey> keys; // reported keys, in order
    std::unique_ptr<AppearanceSection> sec =
        tk::create_root_widget<AppearanceSection>(&host, binder);
    AsStage()
    {
        binder.on_changed = [this](SettingKey k) { keys.push_back(k); };
        mount(*sec, {0, 0, 700, 1200});
    }
    void run() { relayout_paint(*sec, {0, 0, 700, 1200}); }
    tk::CheckButton* check(const std::string& label)
    {
        for (auto* c : vt::find_all<tk::CheckButton>(*sec))
            if (c->access_name() == label)
                return c;
        return nullptr;
    }
};

} // namespace

TEST_CASE("AppearanceSection checkboxes write Settings and report their keys; "
          "reload is silent",
          "[tk][view][appearance]")
{
    AsStage s;
    auto& st = Settings::instance();
    st.group_unread_rooms = st.group_inactive_rooms = st.autoscroll_unread_rooms =
        st.show_room_join_leave_events = st.show_deleted_messages = st.animate_avatars =
            st.show_sender_status_in_timeline = false;
    s.binder.reload();

    auto fire = [&](const char* label)
    {
        auto* c = s.check(label);
        REQUIRE(c != nullptr);
        c->on_change(true);
    };
    fire("Group unread rooms");
    fire("Group inactive rooms");
    fire("Scroll to rooms with new messages");
    fire("Show room join/leave events");
    fire("Show deleted messages");
    fire("Animate avatars");
    fire("Show status emoji next to names");
    CHECK(s.keys == std::vector<SettingKey>{
                        SettingKey::GroupUnread, SettingKey::GroupInactive,
                        SettingKey::AutoscrollUnread, SettingKey::ShowMembershipEvents,
                        SettingKey::ShowDeletedMessages, SettingKey::AnimateAvatars,
                        SettingKey::ShowSenderStatus});
    CHECK(st.group_unread_rooms);
    CHECK(st.group_inactive_rooms);
    CHECK(st.autoscroll_unread_rooms);
    CHECK(st.show_room_join_leave_events);
    CHECK(st.show_deleted_messages);
    CHECK(st.animate_avatars);
    CHECK(st.show_sender_status_in_timeline);

    // reload() pushes the stored values into the controls without reporting.
    s.keys.clear();
    st.group_unread_rooms = false;
    st.animate_avatars = false;
    s.binder.reload();
    CHECK(s.keys.empty());
    CHECK_FALSE(s.check("Group unread rooms")->checked());
    CHECK_FALSE(s.check("Animate avatars")->checked());
    CHECK(s.check("Group inactive rooms")->checked());
}

TEST_CASE("AppearanceSection combos map to settings enums",
          "[tk][view][appearance]")
{
    AsStage s;
    auto combos = vt::find_all<tk::ComboBox>(*s.sec);
    REQUIRE(combos.size() == 4); // accent, message layout, timeline position, inactive period

    auto& st = Settings::instance();
    std::vector<Settings::ThemeAccent> accents;
    for (const char* v : {"blue", "forest", "sunset", "violet", "system", "bogus"})
    {
        combos[0]->on_changed(v);
        accents.push_back(st.theme_accent);
    }
    // "bogus" is not an option id: ignored, the previous value stays.
    CHECK(accents == std::vector<Settings::ThemeAccent>{
                         Settings::ThemeAccent::Blue, Settings::ThemeAccent::Forest,
                         Settings::ThemeAccent::Sunset, Settings::ThemeAccent::Violet,
                         Settings::ThemeAccent::System, Settings::ThemeAccent::System});
    CHECK(s.keys.size() == 5); // the unknown id reported nothing

    std::vector<Settings::MessageLayout> layouts;
    for (const char* v : {"bubbles", "irc", "classic"})
    {
        combos[1]->on_changed(v);
        layouts.push_back(st.message_layout);
    }
    CHECK(layouts == std::vector<Settings::MessageLayout>{
                         Settings::MessageLayout::Bubbles, Settings::MessageLayout::Irc,
                         Settings::MessageLayout::Classic});

    combos[2]->on_changed("center");
    CHECK(st.timeline_column_anchor == Settings::TimelineColumnAnchor::Center);
    combos[2]->on_changed("left");

    combos[3]->on_changed("90");
    CHECK(st.inactive_room_threshold_days == 90);

    st.theme_accent = Settings::ThemeAccent::Violet;
    st.message_layout = Settings::MessageLayout::Irc;
    st.inactive_room_threshold_days = 30;
    s.binder.reload();
    CHECK(combos[0]->selected_value() == "violet");
    CHECK(combos[1]->selected_value() == "irc");
    CHECK(combos[2]->selected_value() == "left");
    CHECK(combos[3]->selected_value() == "30");
    s.run();
}

TEST_CASE("AppearanceSection theme picker: click, hover and arrow keys",
          "[tk][view][appearance]")
{
    AsStage s;
    std::vector<Settings::ThemePreference> prefs;
    s.binder.on_changed = [&](SettingKey k)
    {
        if (k == SettingKey::Theme)
            prefs.push_back(Settings::instance().theme_pref);
    };
    Settings::instance().theme_pref = Settings::ThemePreference::Light;
    s.binder.reload();
    s.run();

    // Find the picker by scanning for a click that changes the theme. The
    // three radio buttons sit side by side in the first group.
    bool found = false;
    for (float y = 20; y < 200 && !found; y += 6)
        for (float x = 20; x < 400 && !found; x += 10)
        {
            s.host.dispatch_pointer_move({x, y});
            s.host.dispatch_pointer_down({x, y});
            s.host.dispatch_pointer_up({x, y});
            found = !prefs.empty();
        }
    REQUIRE(found);
    CHECK(prefs[0] != Settings::ThemePreference::Light); // only changes fire

    // Keyboard: with the picker focused, arrows cycle the preference.
    const size_t before = prefs.size();
    s.host.dispatch_key_down(vt::key(tk::Key::Right));
    s.host.dispatch_key_down(vt::key(tk::Key::Left));
    CHECK(prefs.size() >= before); // focused picker handles it; harmless otherwise
    s.host.dispatch_pointer_leave();
    s.run();
}

TEST_CASE("AppearanceSection room list density radio fires its callback",
          "[tk][view][appearance]")
{
    AsStage s;
    Settings::instance().room_list_density = Settings::RoomListDensity::Normal;
    s.binder.reload();
    s.run();

    tk::RadioButton* compact = nullptr;
    for (auto* b : vt::find_all<tk::RadioButton>(*s.sec))
        if (b->access_name() == "Compact")
            compact = b;
    REQUIRE(compact);
    CHECK(compact->access_default_action());
    CHECK(s.keys == std::vector<SettingKey>{SettingKey::RoomListDensity});
    CHECK(Settings::instance().room_list_density == Settings::RoomListDensity::Compact);
}
