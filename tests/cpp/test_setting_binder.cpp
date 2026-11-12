#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/settings/SettingBinder.h"

#include <string>

using tesseract::Settings;
using tesseract::views::SettingBinder;
using tesseract::views::SettingKey;

namespace
{

// Restores the singleton fields the tests touch.
struct BinderSettingsGuard
{
    Settings& s = Settings::instance();
    bool presence = s.send_presence;
    Settings::MessageLayout layout = s.message_layout;
    std::string camera = s.camera_device_id;
    ~BinderSettingsGuard()
    {
        s.send_presence = presence;
        s.message_layout = layout;
        s.camera_device_id = camera;
    }
};

} // namespace

TEST_CASE("SettingBinder: checkbox writes the field and reports the key once", "[setting_binder]")
{
    BinderSettingsGuard guard;
    vt::Stage st;
    auto cb = tk::create_root_widget<tk::CheckButton>(&st.host, "x", false);
    SettingBinder b;
    int fires = 0;
    SettingKey last{};
    b.on_changed = [&](SettingKey k)
    {
        ++fires;
        last = k;
    };
    bool local_v = false;
    b.bind(cb.get(), &Settings::send_presence, SettingKey::SendPresence,
           [&](bool v)
           {
               local_v = v;
           });

    Settings::instance().send_presence = false;
    cb->on_change(true);
    CHECK(Settings::instance().send_presence);
    CHECK(fires == 1);
    CHECK(last == SettingKey::SendPresence);
    CHECK(local_v);

    cb->on_change(true); // equal to the stored value: no report
    CHECK(fires == 1);
}

TEST_CASE("SettingBinder: reload pushes fields into controls silently", "[setting_binder]")
{
    BinderSettingsGuard guard;
    vt::Stage st;
    auto cb = tk::create_root_widget<tk::CheckButton>(&st.host, "x", false);
    SettingBinder b;
    int fires = 0;
    b.on_changed = [&](SettingKey)
    {
        ++fires;
    };
    b.bind(cb.get(), &Settings::send_presence, SettingKey::SendPresence);

    Settings::instance().send_presence = true;
    b.reload();
    CHECK(cb->checked());
    CHECK(fires == 0);
}

TEST_CASE("SettingBinder: combo maps ids to enum values", "[setting_binder]")
{
    BinderSettingsGuard guard;
    vt::Stage st;
    auto combo = tk::create_root_widget<tk::ComboBox>(&st.host);
    combo->set_options({{"Classic", "classic"}, {"Bubbles", "bubbles"}});
    SettingBinder b;
    int fires = 0;
    b.on_changed = [&](SettingKey)
    {
        ++fires;
    };
    b.bind<Settings::MessageLayout>(combo.get(), &Settings::message_layout,
                                    SettingKey::MessageLayout,
                                    {{Settings::MessageLayout::Classic, "classic"},
                                     {Settings::MessageLayout::Bubbles, "bubbles"}});

    Settings::instance().message_layout = Settings::MessageLayout::Classic;
    combo->on_changed("bubbles");
    CHECK(Settings::instance().message_layout == Settings::MessageLayout::Bubbles);
    CHECK(fires == 1);

    combo->on_changed("bogus"); // unknown id ignored
    CHECK(fires == 1);

    Settings::instance().message_layout = Settings::MessageLayout::Classic;
    b.reload();
    CHECK(combo->selected_value() == "classic");
    CHECK(fires == 1);
}

TEST_CASE("SettingBinder: string combo and custom loader", "[setting_binder]")
{
    BinderSettingsGuard guard;
    vt::Stage st;
    auto combo = tk::create_root_widget<tk::ComboBox>(&st.host);
    SettingBinder b;
    SettingKey last = SettingKey::Theme;
    b.on_changed = [&](SettingKey k)
    {
        last = k;
    };
    b.bind(combo.get(), &Settings::camera_device_id, SettingKey::Camera);
    int loads = 0;
    b.bind_custom(
        [&]
        {
            ++loads;
        });

    combo->on_changed("cam0");
    CHECK(Settings::instance().camera_device_id == "cam0");
    CHECK(last == SettingKey::Camera);

    b.reload();
    CHECK(loads == 1);
    CHECK(combo->selected_value() == "cam0");

    last = SettingKey::Theme;
    b.changed(SettingKey::Proxy);
    CHECK(last == SettingKey::Proxy);
}

TEST_CASE("SettingBinder: slider writes an int field and reloads silently", "[setting_binder]")
{
    struct ColumnGuard
    {
        Settings& s = Settings::instance();
        int pct = s.timeline_column_pct;
        ~ColumnGuard()
        {
            s.timeline_column_pct = pct;
        }
    } guard;
    vt::Stage st;
    auto slider = tk::create_root_widget<tk::Slider>(&st.host, 30.0, 100.0, 1.0);
    SettingBinder b;
    int fires = 0;
    SettingKey last{};
    b.on_changed = [&](SettingKey k)
    {
        ++fires;
        last = k;
    };
    b.bind(slider.get(), &Settings::timeline_column_pct, SettingKey::TimelineColumnWidth);

    Settings::instance().timeline_column_pct = 50;
    // A drag writes the field live but reports (save + relayout) only on release.
    slider->on_changed(60.0);
    slider->on_changed(64.0);
    CHECK(Settings::instance().timeline_column_pct == 64);
    CHECK(fires == 0);
    slider->on_committed(64.0);
    CHECK(fires == 1);
    CHECK(last == SettingKey::TimelineColumnWidth);

    slider->on_committed(64.0); // nothing changed since: no second report
    CHECK(fires == 1);

    Settings::instance().timeline_column_pct = 80;
    b.reload();
    CHECK(slider->value() == 80.0);
    CHECK(fires == 1);
}
