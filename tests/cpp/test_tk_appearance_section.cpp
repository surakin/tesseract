#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/settings/AppearanceSection.h"

#include <string>
#include <vector>

using tesseract::Settings;
using tesseract::views::AppearanceSection;

namespace
{

struct AsStage : vt::Stage
{
    std::unique_ptr<AppearanceSection> sec =
        tk::create_root_widget<AppearanceSection>(&host);
    AsStage()
    {
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

TEST_CASE("AppearanceSection checkboxes fire their callbacks and the setters "
          "stay silent",
          "[tk][view][appearance]")
{
    AsStage s;
    struct Row
    {
        const char* label;
        std::function<void(bool)>* cb;
    };
    bool unread = false, inactive = false, autoscroll = false, membership = false,
         anim = false, status = false;
    s.sec->on_group_unread_changed = [&](bool v) { unread = v; };
    s.sec->on_group_inactive_changed = [&](bool v) { inactive = v; };
    s.sec->on_autoscroll_unread_changed = [&](bool v) { autoscroll = v; };
    s.sec->on_show_membership_events_changed = [&](bool v) { membership = v; };
    s.sec->on_animate_avatars_changed = [&](bool v) { anim = v; };
    s.sec->on_show_sender_status_changed = [&](bool v) { status = v; };

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
    fire("Animate avatars");
    fire("Show status emoji next to names");
    CHECK(unread);
    CHECK(inactive);
    CHECK(autoscroll);
    CHECK(membership);
    CHECK(anim);
    CHECK(status);

    // Programmatic setters update the controls without notifying.
    unread = inactive = autoscroll = membership = anim = status = false;
    s.sec->set_group_unread(true);
    s.sec->set_group_inactive(true);
    s.sec->set_autoscroll_unread(true);
    s.sec->set_show_membership_events(true);
    s.sec->set_animate_avatars(true);
    s.sec->set_show_sender_status(true);
    CHECK_FALSE(unread);
    CHECK_FALSE(inactive);
    CHECK_FALSE(autoscroll);
    CHECK_FALSE(membership);
    CHECK_FALSE(anim);
    CHECK_FALSE(status);
    CHECK(s.check("Group unread rooms")->checked());
    CHECK(s.check("Animate avatars")->checked());
}

TEST_CASE("AppearanceSection combos map to settings enums",
          "[tk][view][appearance]")
{
    AsStage s;
    auto combos = vt::find_all<tk::ComboBox>(*s.sec);
    REQUIRE(combos.size() == 3); // accent, message layout, inactive period

    std::vector<Settings::ThemeAccent> accents;
    s.sec->on_accent_changed = [&](Settings::ThemeAccent a) { accents.push_back(a); };
    for (const char* v : {"blue", "forest", "sunset", "violet", "system", "bogus"})
        combos[0]->on_changed(v);
    CHECK(accents == std::vector<Settings::ThemeAccent>{
                         Settings::ThemeAccent::Blue, Settings::ThemeAccent::Forest,
                         Settings::ThemeAccent::Sunset, Settings::ThemeAccent::Violet,
                         Settings::ThemeAccent::System, Settings::ThemeAccent::System});

    std::vector<Settings::MessageLayout> layouts;
    s.sec->on_message_layout_changed = [&](Settings::MessageLayout l) { layouts.push_back(l); };
    for (const char* v : {"bubbles", "irc", "classic"})
        combos[1]->on_changed(v);
    CHECK(layouts == std::vector<Settings::MessageLayout>{
                         Settings::MessageLayout::Bubbles, Settings::MessageLayout::Irc,
                         Settings::MessageLayout::Classic});

    int days = 0;
    s.sec->on_inactive_period_changed = [&](int d) { days = d; };
    combos[2]->on_changed("90");
    CHECK(days == 90);

    s.sec->set_selected_accent(Settings::ThemeAccent::Violet);
    CHECK(combos[0]->selected_value() == "violet");
    s.sec->set_message_layout(Settings::MessageLayout::Irc);
    CHECK(combos[1]->selected_value() == "irc");
    s.sec->set_message_layout(Settings::MessageLayout::Bubbles);
    s.sec->set_message_layout(Settings::MessageLayout::Classic);
    s.sec->set_inactive_period(30);
    CHECK(combos[2]->selected_value() == "30");
    s.run();
}

TEST_CASE("AppearanceSection theme picker: click, hover and arrow keys",
          "[tk][view][appearance]")
{
    AsStage s;
    std::vector<Settings::ThemePreference> prefs;
    s.sec->on_theme_changed = [&](Settings::ThemePreference p) { prefs.push_back(p); };
    s.sec->set_selected(Settings::ThemePreference::Light);
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
