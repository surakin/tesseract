// ShellBase_settings.cpp: the global-preference handlers (persist to Settings,
// push to every logged-in client), bridge overrides, skin tone, theme and the
// low-power-mode plumbing.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/power_monitor.h>
#include <tesseract/settings.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using tesseract::Settings;

namespace
{

struct SettingsShell : tesseract::test::TestShellBase
{
    ~SettingsShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    // Queue delayed posts: media_sweep_timer_ reschedules itself and would
    // recurse forever if run inline.
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    std::vector<std::function<void()>> delayed;

    void request_relayout_() override { ++relayouts; }
    int relayouts = 0;

    tk::ThemeMode scheme = tk::ThemeMode::Light;
    std::optional<tk::Color> accent;
    int theme_applies = 0;
    tk::ThemeMode last_mode_seen = tk::ThemeMode::Light;
    bool low_power_ui = false;
    int low_power_ui_calls = 0;

    tk::ThemeMode os_color_scheme_() const override { return scheme; }
    std::optional<tk::Color> os_accent_color_() const override { return accent; }
    void apply_theme_ui_(const tk::Theme&) override { ++theme_applies; }
    void on_low_power_mode_ui_(bool a) override
    {
        low_power_ui = a;
        ++low_power_ui_calls;
    }

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::apply_bridge_overrides_;
    using ShellBase::apply_low_power_mode_;
    using ShellBase::apply_theme_to_secondary_windows_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::current_scale_;
    using ShellBase::current_theme_;
    using ShellBase::emoji_skin_tone_;
    using ShellBase::handle_setting_changed_;
    using ShellBase::open_settings_from_login_;
    using ShellBase::take_login_recheck_;
    using ShellBase::take_settings_from_login_;

    // The login view's client is only swapped while Settings is open.
    void rebind_login_client_ui_() override { ++login_rebinds; }
    int login_rebinds = 0;
    using ShellBase::last_system_accent_;
    using ShellBase::low_power_active;
    using ShellBase::low_power_available;
    using ShellBase::my_user_id_;
    using ShellBase::on_system_accent_changed_;
    using ShellBase::per_account_rooms_;
    using ShellBase::power_policy_;
    using ShellBase::rooms_;
    using ShellBase::set_bridge_override_;
    using ShellBase::set_current_scale_;
    using ShellBase::set_emoji_skin_tone_;
    using ShellBase::set_power_monitor_;
    using ShellBase::settle_emoji_skin_tone_;
};

// Write `value` into Settings the way the settings view does, then report the
// key to the shell.
void changed(SettingsShell& s, tesseract::views::SettingKey key)
{
    s.handle_setting_changed_(key);
}

void set_theme(SettingsShell& s, Settings::ThemePreference pref)
{
    Settings::instance().theme_pref = pref;
    changed(s, tesseract::views::SettingKey::Theme);
}

void set_accent(SettingsShell& s, Settings::ThemeAccent accent)
{
    Settings::instance().theme_accent = accent;
    changed(s, tesseract::views::SettingKey::Accent);
}

void set_low_power(SettingsShell& s, Settings::LowPowerPreference pref)
{
    Settings::instance().low_power_pref = pref;
    changed(s, tesseract::views::SettingKey::LowPower);
}

struct FakePower : tesseract::IPowerMonitor
{
    bool saver = false;
    bool battery_discharging = false;
    bool battery = true;
    bool os_power_saver_active() const override { return saver; }
    bool on_battery_discharging() const override { return battery_discharging; }
    bool has_battery() const override { return battery; }
};

std::shared_ptr<tesseract::AccountSession> settings_handlers_account(const std::string& uid)
{
    auto s = std::make_shared<tesseract::AccountSession>();
    s->user_id = uid;
    return s;
}

tesseract::RoomInfo settings_handlers_room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    return r;
}

} // namespace

TEST_CASE("settings handlers persist plain toggles", "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    auto& st = Settings::instance();

    using K = tesseract::views::SettingKey;
    st.developer_mode = true;
    st.send_maps_urls_as_location = false;
    st.index_messages_for_search = true;
    st.show_room_join_leave_events = true;
    st.msc2545_legacy_compat = false;
    st.send_bundled_url_previews = true;
    st.fetch_url_previews_directly = true;
    for (K k : {K::DeveloperMode, K::SendMapsAsLocation, K::IndexMessages,
                K::ShowMembershipEvents, K::Msc2545Compat, K::BundledUrlPreviews})
        changed(s, k);

    // Every key is persisted: flip the in-memory values, reload from disk.
    st.developer_mode = false;
    st.send_maps_urls_as_location = true;
    st.index_messages_for_search = false;
    st.show_room_join_leave_events = false;
    st.msc2545_legacy_compat = true;
    st.send_bundled_url_previews = false;
    st.fetch_url_previews_directly = false;
    st.load_from_disk(tesseract::config_dir());
    CHECK(st.developer_mode);
    CHECK_FALSE(st.send_maps_urls_as_location);
    CHECK(st.index_messages_for_search);
    CHECK(st.show_room_join_leave_events);
    CHECK_FALSE(st.msc2545_legacy_compat);
    CHECK(st.send_bundled_url_previews);
    CHECK(st.fetch_url_previews_directly);
}

TEST_CASE("toggles that need a client push to clients when present",
          "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    tesseract::Client client;
    s.client_ = &client;
    s.current_room_id_ = "!r:x";
    using K = tesseract::views::SettingKey;
    CHECK_NOTHROW(changed(s, K::ShowMembershipEvents));
    CHECK_NOTHROW(changed(s, K::Msc2545Compat));

    auto sess = settings_handlers_account("@a:x");
    sess->client = std::make_unique<tesseract::Client>();
    s.account_manager_.add_account(sess);
    CHECK_NOTHROW(changed(s, K::IndexMessages));
    CHECK_NOTHROW(changed(s, K::BundledUrlPreviews));
}

TEST_CASE("sender status, layout and density changes relayout",
          "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    auto& st = Settings::instance();
    using K = tesseract::views::SettingKey;

    st.show_sender_status_in_timeline = true;
    changed(s, K::ShowSenderStatus);
    CHECK(s.relayouts == 1);

    st.show_sender_timezone_in_timeline = true;
    changed(s, K::ShowSenderTimezone);
    CHECK(s.relayouts == 2);

    st.message_layout = st.message_layout == Settings::MessageLayout::Bubbles
                            ? Settings::MessageLayout::Classic
                            : Settings::MessageLayout::Bubbles;
    changed(s, K::MessageLayout);
    CHECK(s.relayouts == 3);

    using RD = Settings::RoomListDensity;
    st.room_list_density = RD::Compact;
    changed(s, K::RoomListDensity);
    CHECK(s.relayouts == 4);
    st.room_list_density = RD::Normal;
}

TEST_CASE("timeline column width and anchor changes relayout",
          "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    auto& st = Settings::instance();
    using K = tesseract::views::SettingKey;

    st.timeline_column_pct = 60;
    changed(s, K::TimelineColumnWidth);
    CHECK(s.relayouts == 1);

    st.timeline_column_anchor = Settings::TimelineColumnAnchor::Center;
    changed(s, K::TimelineColumnAnchor);
    CHECK(s.relayouts == 2);

    st.timeline_column_pct = 50;
    st.timeline_column_anchor = Settings::TimelineColumnAnchor::Left;
}

TEST_CASE("presence toggle persists and resolves polling", "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    Settings::instance().send_presence = false;
    changed(s, tesseract::views::SettingKey::SendPresence);
    Settings::instance().load_from_disk(tesseract::config_dir());
    CHECK_FALSE(Settings::instance().send_presence);
}

TEST_CASE("bridge override is recorded, deduped and applied to rooms",
          "[shell][settings_handlers][bridge]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    // No account / empty id: ignored.
    s.set_bridge_override_("!a:x", true);
    auto sess = settings_handlers_account("@me:x");
    s.active_account_ = sess;
    s.my_user_id_ = "@me:x";
    s.set_bridge_override_("", true);
    CHECK(sess->bridge_not_bridged_overrides.empty());

    s.rooms_ = {settings_handlers_room("!a:x"), settings_handlers_room("!b:x")};
    s.per_account_rooms_["@me:x"] = s.rooms_;

    s.set_bridge_override_("!a:x", true);
    REQUIRE(sess->bridge_not_bridged_overrides.size() == 1);
    CHECK(s.rooms_[0].bridge_overridden);
    CHECK_FALSE(s.rooms_[1].bridge_overridden);
    CHECK(s.per_account_rooms_["@me:x"][0].bridge_overridden);

    // Same value again: no change.
    s.set_bridge_override_("!a:x", true);
    CHECK(sess->bridge_not_bridged_overrides.size() == 1);

    // Clearing removes it and the flag.
    s.set_bridge_override_("!a:x", false);
    CHECK(sess->bridge_not_bridged_overrides.empty());
    // apply_bridge_overrides_ skips work for an empty list, so the stale flag
    // on the cached room is cleared only when other overrides remain.
    s.set_bridge_override_("!b:x", true);
    s.set_bridge_override_("!a:x", false);
    CHECK(s.rooms_[1].bridge_overridden);
    CHECK_FALSE(s.rooms_[0].bridge_overridden);
}

TEST_CASE("apply_bridge_overrides_ flags exactly the listed rooms",
          "[shell][settings_handlers][bridge]")
{
    SettingsShell s;
    std::vector<tesseract::RoomInfo> rooms{settings_handlers_room("!a:x"), settings_handlers_room("!b:x"),
                                           settings_handlers_room("!c:x")};
    s.apply_bridge_overrides_(rooms, {});
    CHECK_FALSE(rooms[0].bridge_overridden);
    s.apply_bridge_overrides_(rooms, {"!b:x"});
    CHECK_FALSE(rooms[0].bridge_overridden);
    CHECK(rooms[1].bridge_overridden);
    CHECK_FALSE(rooms[2].bridge_overridden);
}

TEST_CASE("emoji skin tone follows the active account", "[shell][settings_handlers]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    using Tone = tesseract::emoji::SkinTone;
    CHECK(s.emoji_skin_tone_() == Tone::None);
    s.set_emoji_skin_tone_(Tone::Dark); // no account: ignored

    auto sess = settings_handlers_account("@me:x");
    s.active_account_ = sess;
    CHECK(s.emoji_skin_tone_() == Tone::None);
    s.set_emoji_skin_tone_(Tone::Dark);
    CHECK(s.emoji_skin_tone_() == Tone::Dark);
    CHECK(sess->emoji_skin_tone == Tone::Dark);
}

TEST_CASE("theme preference and accent are persisted and re-applied",
          "[shell][settings_handlers][theme]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    set_theme(s, Settings::ThemePreference::Dark);
    CHECK(Settings::instance().theme_pref == Settings::ThemePreference::Dark);
    CHECK(s.theme_applies == 1);
    CHECK(s.current_theme_.mode == tk::ThemeMode::Dark);

    set_theme(s, Settings::ThemePreference::Light);
    CHECK(s.current_theme_.mode == tk::ThemeMode::Light);

    // "System" asks the OS.
    s.scheme = tk::ThemeMode::Dark;
    set_theme(s, Settings::ThemePreference::System);
    CHECK(s.current_theme_.mode == tk::ThemeMode::Dark);

    set_accent(s, Settings::ThemeAccent::Forest);
    CHECK(Settings::instance().theme_accent == Settings::ThemeAccent::Forest);
    CHECK_FALSE(s.last_system_accent_.has_value());
    CHECK(s.theme_applies == 4);
}

TEST_CASE("system accent changes re-theme only when the OS colour moved",
          "[shell][settings_handlers][theme]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    // Not on the System accent: ignored.
    set_accent(s, Settings::ThemeAccent::Blue);
    const int base = s.theme_applies;
    s.on_system_accent_changed_();
    CHECK(s.theme_applies == base);

    s.accent = tk::Color{10, 20, 30, 255};
    set_accent(s, Settings::ThemeAccent::System);
    REQUIRE(s.last_system_accent_.has_value());
    const int after = s.theme_applies;
    s.on_system_accent_changed_(); // same colour: no re-apply
    CHECK(s.theme_applies == after);
    s.accent = tk::Color{200, 100, 50, 255};
    s.on_system_accent_changed_();
    CHECK(s.theme_applies == after + 1);
}

TEST_CASE("display scale changes flush caches and relayout once",
          "[shell][settings_handlers]")
{
    SettingsShell s;
    s.set_current_scale_(1.0f); // within epsilon of the default: nothing
    CHECK(s.relayouts == 0);
    s.set_current_scale_(2.0f);
    CHECK(s.current_scale_ == 2.0f);
    CHECK(s.relayouts == 1);
    s.set_current_scale_(2.001f);
    CHECK(s.relayouts == 1);
}

TEST_CASE("low power mode is unavailable without a battery",
          "[shell][settings_handlers][power]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    auto pm = std::make_unique<FakePower>();
    pm->battery = false;
    s.set_power_monitor_(std::move(pm));
    CHECK_FALSE(s.low_power_available());
    set_low_power(s, Settings::LowPowerPreference::On);
    CHECK_FALSE(s.low_power_active());
    s.set_power_monitor_(nullptr); // ignored
}

TEST_CASE("low power preference On/Off drives the policy and the UI",
          "[shell][settings_handlers][power]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    Settings::instance().low_power_pref = Settings::LowPowerPreference::Off;
    s.set_power_monitor_(std::make_unique<FakePower>());
    REQUIRE(s.low_power_available());
    CHECK_FALSE(s.low_power_active());

    set_low_power(s, Settings::LowPowerPreference::On);
    CHECK(Settings::instance().low_power_pref == Settings::LowPowerPreference::On);
    CHECK(s.low_power_active());
    CHECK(s.low_power_ui);

    set_low_power(s, Settings::LowPowerPreference::Off);
    CHECK_FALSE(s.low_power_active());
    CHECK_FALSE(s.low_power_ui);
}

TEST_CASE("a persisted On preference is applied at startup",
          "[shell][settings_handlers][power]")
{
    tesseract::test::SettingsGuard guard;
    Settings::instance().low_power_pref = Settings::LowPowerPreference::On;
    SettingsShell s;
    s.set_power_monitor_(std::make_unique<FakePower>());
    CHECK(s.low_power_active());
}

TEST_CASE("apply_low_power_mode_ pushes to every account client",
          "[shell][settings_handlers][power]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    auto sess = settings_handlers_account("@a:x");
    sess->client = std::make_unique<tesseract::Client>();
    s.account_manager_.add_account(sess);
    CHECK_NOTHROW(s.apply_low_power_mode_(true));
    CHECK_NOTHROW(s.apply_low_power_mode_(false));
}

TEST_CASE("a proxy change from the login screen swaps the client and defers the re-check",
          "[shell][settings_handlers][login]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    s.open_settings_from_login_();
    Settings::instance().proxy_mode = Settings::ProxyMode::None;
    changed(s, tesseract::views::SettingKey::Proxy);

    // The login view is only handed the new client (no reset, so its homeserver
    // field keeps its hands off the focus while the user types in Settings).
    CHECK(s.login_rebinds == 1);

    // Restore the process-wide SDK proxy this test touched (still signed out,
    // so it is applied live).
    Settings::instance().proxy_mode = Settings::ProxyMode::System;
    changed(s, tesseract::views::SettingKey::Proxy);

    // Leaving Settings is when the homeserver check is re-run, exactly once.
    REQUIRE(s.take_settings_from_login_());
    CHECK(s.take_login_recheck_());
    CHECK_FALSE(s.take_login_recheck_());
}

TEST_CASE("a proxy change while signed in never touches the login view",
          "[shell][settings_handlers][login]")
{
    tesseract::test::SettingsGuard guard;
    SettingsShell s;
    Settings::instance().proxy_mode = Settings::ProxyMode::None;
    changed(s, tesseract::views::SettingKey::Proxy);
    CHECK(s.login_rebinds == 0);
    CHECK_FALSE(s.take_login_recheck_());
    Settings::instance().proxy_mode = Settings::ProxyMode::System;
}
