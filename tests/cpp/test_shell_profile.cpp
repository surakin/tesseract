// ShellBase_profile.cpp: unread/tray aggregation, presence, own-profile and
// extended-profile handling, account prefs sync, the known-users roster and the
// user context menu.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ProfileShell : tesseract::test::TestShellBase
{
    ~ProfileShell() override
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
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    std::vector<std::function<void()>> delayed;

    void on_rooms_updated_() override { ++rooms_updated; }
    int rooms_updated = 0;

    void on_dock_badge_changed_(uint64_t c) override { dock.push_back(c); }
    void on_account_badges_changed_(bool o) override { others.push_back(o); }
    void on_tray_unread_changed_(bool u, bool h) override
    {
        tray.emplace_back(u, h);
    }
    void on_profile_field_result_ui_(const std::string& k, bool ok,
                                     const std::string&) override
    {
        field_results.emplace_back(k, ok);
    }
    void refresh_user_strip_() override { ++strip_refreshes; }

    std::vector<uint64_t> dock;
    std::vector<bool> others;
    std::vector<std::pair<bool, bool>> tray;
    std::vector<std::pair<std::string, bool>> field_results;
    int strip_refreshes = 0;

    using ShellBase::account_has_unread_for;
    using ShellBase::active_account_;
    using ShellBase::best_unread_room_;
    using ShellBase::build_user_menu_items_;
    using ShellBase::client_;
    using ShellBase::compute_dock_notification_count_;
    using ShellBase::current_room_id_;
    using ShellBase::filter_known_users_;
    using ShellBase::find_existing_dm_;
    using ShellBase::handle_account_prefs_updated_ui_;
    using ShellBase::handle_extended_profile_ready_ui_;
    using ShellBase::handle_own_profile_changed_ui_;
    using ShellBase::handle_presence_changed_ui_;
    using ShellBase::handle_profile_field_change_;
    using ShellBase::handle_profile_field_result_ui_;
    using ShellBase::invalidate_known_users_;
    using ShellBase::known_users_;
    using ShellBase::known_users_built_;
    using ShellBase::member_gender_cache_;
    using ShellBase::member_gender_inflight_;
    using ShellBase::merge_resolved_user_;
    using ShellBase::merge_roster_entry_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_display_name_;
    using ShellBase::my_user_id_;
    using ShellBase::notify_tray_unread_;
    using ShellBase::other_accounts_have_unread;
    using ShellBase::own_extended_profile_;
    using ShellBase::pending_member_gender_requests_;
    using ShellBase::pending_profile_field_writes_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::per_account_rooms_;
    using ShellBase::presence_for_;
    using ShellBase::recent_room_ids_;
    using ShellBase::request_member_pronoun_ui_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::user_presence_;
};

tesseract::RoomInfo profile_room(const std::string& id, std::uint32_t notif = 0,
                                 std::uint32_t high = 0, std::uint64_t ts = 0)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    r.notification_count = notif;
    r.highlight_count = high;
    r.last_activity_ts = ts;
    return r;
}

std::shared_ptr<tesseract::AccountSession> profile_account(const std::string& uid)
{
    auto s = std::make_shared<tesseract::AccountSession>();
    s->user_id = uid;
    return s;
}

} // namespace

TEST_CASE("per-account unread aggregation", "[shell][profile][unread]")
{
    ProfileShell s;
    s.my_user_id_ = "@me:x";
    s.per_account_rooms_["@me:x"] = {profile_room("!a", 3)};
    s.per_account_rooms_["@other:x"] = {profile_room("!b", 0)};
    CHECK_FALSE(s.other_accounts_have_unread());
    CHECK(s.account_has_unread_for("@me:x"));
    CHECK_FALSE(s.account_has_unread_for("@other:x"));
    CHECK_FALSE(s.account_has_unread_for("@nobody:x"));

    s.per_account_rooms_["@other:x"] = {profile_room("!b", 2)};
    CHECK(s.other_accounts_have_unread());
    CHECK(s.compute_dock_notification_count_() == 5);
}

TEST_CASE("notify_tray_unread_ fires each hook only when its value changes",
          "[shell][profile][unread]")
{
    ProfileShell s;
    s.my_user_id_ = "@me:x";
    s.per_account_rooms_["@me:x"] = {profile_room("!a", 0)};
    s.notify_tray_unread_();
    // The first call publishes the initial badge (0) once; nothing else moves.
    CHECK(s.dock == std::vector<uint64_t>{0});
    CHECK(s.others.empty());
    CHECK(s.tray.empty());

    s.per_account_rooms_["@me:x"] = {profile_room("!a", 4, 1)};
    s.per_account_rooms_["@other:x"] = {profile_room("!b", 1)};
    s.notify_tray_unread_();
    CHECK(s.dock == std::vector<uint64_t>{0, 5});
    CHECK(s.others == std::vector<bool>{true});
    REQUIRE(s.tray.size() == 1);
    CHECK(s.tray[0] == std::make_pair(true, true));

    // Same state again: silent.
    s.notify_tray_unread_();
    CHECK(s.dock.size() == 2);
    CHECK(s.others.size() == 1);
    CHECK(s.tray.size() == 1);

    // Count changes but unread/highlight booleans don't: only the badge fires.
    s.per_account_rooms_["@me:x"] = {profile_room("!a", 9, 1)};
    s.notify_tray_unread_();
    CHECK(s.dock.size() == 3);
    CHECK(s.tray.size() == 1);
}

TEST_CASE("best_unread_room_ prefers highlights, then recency",
          "[shell][profile][unread]")
{
    ProfileShell s;
    CHECK(s.best_unread_room_() == nullptr);
    s.rooms_ = {profile_room("!read"), profile_room("!old", 2, 0, 10),
                profile_room("!new", 1, 0, 50)};
    REQUIRE(s.best_unread_room_() != nullptr);
    CHECK(s.best_unread_room_()->id == "!new");
    s.rooms_.push_back(profile_room("!mention", 1, 1, 1));
    CHECK(s.best_unread_room_()->id == "!mention");
    s.rooms_.push_back(profile_room("!mention2", 1, 1, 99));
    CHECK(s.best_unread_room_()->id == "!mention2");
}

TEST_CASE("find_existing_dm_ looks in the active room list", "[shell][profile]")
{
    ProfileShell s;
    auto dm = profile_room("!dm:x");
    dm.is_direct = true;
    dm.dm_counterpart_user_id = "@friend:x";
    s.rooms_ = {profile_room("!plain:x"), dm};
    CHECK(s.find_existing_dm_("@friend:x") == "!dm:x");
    CHECK(s.find_existing_dm_("@stranger:x").empty());
}

TEST_CASE("presence updates only repaint on a real change",
          "[shell][profile][presence]")
{
    ProfileShell s;
    CHECK(s.presence_for_("@a:x") == tesseract::PresenceState::Offline);
    s.handle_presence_changed_ui_("@a:x", tesseract::PresenceState::Online);
    CHECK(s.rooms_updated == 1);
    CHECK(s.presence_for_("@a:x") == tesseract::PresenceState::Online);
    s.handle_presence_changed_ui_("@a:x", tesseract::PresenceState::Online);
    CHECK(s.rooms_updated == 1);
    s.handle_presence_changed_ui_("@a:x", tesseract::PresenceState::Unavailable);
    CHECK(s.rooms_updated == 2);
    CHECK(s.presence_for_("@a:x") == tesseract::PresenceState::Unavailable);
}

TEST_CASE("profile field writes are tracked and applied on success",
          "[shell][profile][fields]")
{
    tesseract::Client client;
    ProfileShell s;
    // No client: nothing queued.
    s.handle_profile_field_change_("m.tz", "\"UTC\"");
    CHECK(s.pending_profile_field_writes_.empty());

    s.client_ = &client;
    s.handle_profile_field_change_("m.tz", "\"Europe/Oslo\"");
    REQUIRE(s.pending_profile_field_writes_.size() == 1);
    const auto id = s.pending_profile_field_writes_.begin()->first;

    // A failed write reports but does not touch the cache.
    s.handle_profile_field_result_ui_(id, "m.tz", false, "denied");
    CHECK(s.own_extended_profile_.tz.empty());
    CHECK(s.pending_profile_field_writes_.empty());
    REQUIRE(s.field_results.size() == 1);
    CHECK_FALSE(s.field_results[0].second);

    s.handle_profile_field_change_("m.tz", "\"Europe/Oslo\"");
    const auto id2 = s.pending_profile_field_writes_.begin()->first;
    s.handle_profile_field_result_ui_(id2, "m.tz", true, "");
    CHECK(s.own_extended_profile_.tz == "Europe/Oslo");

    // A result for an unknown request id still reports to the UI.
    s.handle_profile_field_result_ui_(9999, "m.tz", true, "");
    CHECK(s.field_results.size() == 3);
}

TEST_CASE("own extended profile is replaced only by a valid fetch",
          "[shell][profile][fields]")
{
    ProfileShell s;
    s.own_extended_profile_.pronouns = {{"en", "she/her", "feminine"}};
    s.handle_extended_profile_ready_ui_(
        1, R"({"exists":false})"); // stale-keeping: pronouns non-empty
    CHECK(s.own_extended_profile_.pronouns.size() == 1);

    s.handle_extended_profile_ready_ui_(
        2, R"({"exists":true,"tz":"UTC","biography":"hi","status_emoji":"x",
               "status_text":"busy","call_joined_ts":5})");
    CHECK(s.own_extended_profile_.tz == "UTC");
    CHECK(s.own_extended_profile_.biography == "hi");
    CHECK(s.own_extended_profile_.status_text == "busy");
    CHECK(s.own_extended_profile_.call_joined_ts == 5);
    CHECK(s.own_extended_profile_.pronouns.empty());
}

TEST_CASE("member pronoun lookups are deduplicated and resolve to a word",
          "[shell][profile][pronoun]")
{
    tesseract::Client client;
    ProfileShell s;
    s.request_member_pronoun_ui_("@a:x"); // no client
    CHECK(s.pending_member_gender_requests_.empty());

    s.client_ = &client;
    s.request_member_pronoun_ui_("");
    CHECK(s.pending_member_gender_requests_.empty());
    s.request_member_pronoun_ui_("@a:x");
    s.request_member_pronoun_ui_("@a:x"); // already in flight
    REQUIRE(s.pending_member_gender_requests_.size() == 1);
    const auto id = s.pending_member_gender_requests_.begin()->first;

    s.handle_extended_profile_ready_ui_(
        id, R"({"exists":true,"pronouns":[{"language":"en","summary":"she/her","grammatical_gender":"feminine"}]})");
    CHECK(s.member_gender_cache_["@a:x"] == "her");
    CHECK(s.pending_member_gender_requests_.empty());
    CHECK(s.member_gender_inflight_.empty());

    // Cached: no new request.
    s.request_member_pronoun_ui_("@a:x");
    CHECK(s.pending_member_gender_requests_.empty());

    // A profile without pronouns falls back to the neutral word.
    s.request_member_pronoun_ui_("@b:x");
    const auto id2 = s.pending_member_gender_requests_.begin()->first;
    s.handle_extended_profile_ready_ui_(id2, R"({"exists":true})");
    CHECK(s.member_gender_cache_["@b:x"] == "their");
}

TEST_CASE("own profile changes update the strip only for the active account",
          "[shell][profile]")
{
    ProfileShell s;
    auto sess = profile_account("@me:x");
    s.active_account_ = sess;
    s.my_display_name_ = "Old";

    s.handle_own_profile_changed_ui_("@someone-else:x", "Nope", std::nullopt);
    CHECK(s.my_display_name_ == "Old");
    CHECK(s.strip_refreshes == 0);

    s.handle_own_profile_changed_ui_("@me:x", "New", std::nullopt);
    CHECK(s.my_display_name_ == "New");
    CHECK(sess->display_name == "New");
    CHECK(s.strip_refreshes == 1);

    // Unchanged values don't refresh again.
    s.handle_own_profile_changed_ui_("@me:x", "New", std::nullopt);
    CHECK(s.strip_refreshes == 1);

    s.handle_own_profile_changed_ui_("@me:x", std::nullopt, "mxc://x/av");
    CHECK(s.my_avatar_url_ == "mxc://x/av");
    CHECK(sess->avatar_url == "mxc://x/av");
    CHECK(s.strip_refreshes == 2);
}

TEST_CASE("account prefs from another device seed recents and open rooms",
          "[shell][profile][prefs]")
{
    tesseract::test::SettingsGuard guard;
    ProfileShell s;
    auto sess = profile_account("@me:x");
    s.active_account_ = sess;
    s.my_user_id_ = "@me:x";

    const std::string json =
        R"({"last_room":"!b:x","open_rooms":["!a:x","!b:x"],"recent_rooms":["!b:x","!a:x"]})";

    // Another account's prefs are ignored.
    const std::string before = sess->prefs_json;
    s.handle_account_prefs_updated_ui_("@other:x", json);
    CHECK(s.pending_restore_rooms_.empty());
    CHECK(sess->prefs_json == before);

    s.handle_account_prefs_updated_ui_("@me:x", json);
    CHECK(sess->prefs_json == json);
    CHECK(s.recent_room_ids_ == std::vector<std::string>{"!b:x", "!a:x"});
    // The active tab is rotated to the front of the restore list.
    CHECK(s.pending_restore_rooms_ == std::vector<std::string>{"!b:x", "!a:x"});
}

TEST_CASE("local recents and restore state win over a later sync",
          "[shell][profile][prefs]")
{
    tesseract::test::SettingsGuard guard;
    ProfileShell s;
    auto sess = profile_account("@me:x");
    s.active_account_ = sess;
    s.my_user_id_ = "@me:x";
    s.recent_room_ids_ = {"!local:x"};
    s.current_room_id_ = "!open:x";
    s.handle_account_prefs_updated_ui_(
        "@me:x", R"({"open_rooms":["!a:x"],"recent_rooms":["!remote:x"]})");
    CHECK(s.recent_room_ids_ == std::vector<std::string>{"!local:x"});
    CHECK(s.pending_restore_rooms_.empty());
}

TEST_CASE("synced bridge overrides are applied to cached rooms",
          "[shell][profile][prefs]")
{
    tesseract::test::SettingsGuard guard;
    ProfileShell s;
    auto sess = profile_account("@me:x");
    s.active_account_ = sess;
    s.my_user_id_ = "@me:x";
    s.rooms_ = {profile_room("!a:x"), profile_room("!b:x")};
    s.per_account_rooms_["@me:x"] = s.rooms_;
    s.handle_account_prefs_updated_ui_(
        "@me:x", R"({"bridge_overrides":["!b:x"]})");
    CHECK(sess->bridge_not_bridged_overrides ==
          std::vector<std::string>{"!b:x"});
    CHECK_FALSE(s.rooms_[0].bridge_overridden);
    CHECK(s.rooms_[1].bridge_overridden);
    CHECK(s.per_account_rooms_["@me:x"][1].bridge_overridden);
}

TEST_CASE("the known-users roster merges without clobbering and filters by "
          "name or id",
          "[shell][profile][roster]")
{
    ProfileShell s;
    s.merge_roster_entry_("", "Nobody", "");
    CHECK(s.known_users_.empty());
    s.merge_roster_entry_("@zed:x", "Zed", "");
    s.merge_roster_entry_("@zed:x", "Other Name", "mxc://x/z");
    s.merge_roster_entry_("@amy:x", "", "");
    s.merge_roster_entry_("@bob:x", "bob the builder", "");
    CHECK(s.known_users_["@zed:x"].display_name == "Zed");
    CHECK(s.known_users_["@zed:x"].avatar_url == "mxc://x/z");

    auto all = s.filter_known_users_("");
    REQUIRE(all.size() == 3);
    // Sorted case-insensitively by display name, falling back to the id.
    CHECK(all[0].user_id == "@amy:x");
    CHECK(all[1].user_id == "@bob:x");
    CHECK(all[2].user_id == "@zed:x");

    auto by_name = s.filter_known_users_("BUILD");
    REQUIRE(by_name.size() == 1);
    CHECK(by_name[0].user_id == "@bob:x");
    CHECK(s.filter_known_users_("zed:x").size() == 1);
    CHECK(s.filter_known_users_("nomatch").empty());
}

TEST_CASE("a live-resolved user overwrites the roster entry; invalidation clears it",
          "[shell][profile][roster]")
{
    ProfileShell s;
    s.merge_roster_entry_("@a:x", "Stale", "");
    tesseract::UserProfile p;
    p.user_id = "@a:x";
    p.display_name = "Fresh";
    s.merge_resolved_user_(p);
    CHECK(s.known_users_["@a:x"].display_name == "Fresh");

    s.known_users_built_ = true;
    s.invalidate_known_users_();
    CHECK(s.known_users_.empty());
    CHECK_FALSE(s.known_users_built_);
}

TEST_CASE("the user context menu lists the canonical items in order",
          "[shell][profile][menu]")
{
    ProfileShell s;
    s.my_user_id_ = "@me:x";
    int quit = 0;
    auto noop = [] {};
    auto items = s.build_user_menu_items_(noop, noop, nullptr, noop,
                                          [&] { ++quit; }, nullptr);
    // Settings, Add Account, Log Out, separator, Quit (no QR, no verify, no
    // main_app_ so no keyboard shortcuts entry).
    REQUIRE(items.size() == 5);
    CHECK(items[2].label.find("@me:x") != std::string::npos);
    CHECK(items[3].label.empty());
    CHECK(items[3].callback == nullptr);
    items[4].callback();
    CHECK(quit == 1);

    s.my_display_name_ = "Me Myself";
    s.server_info_.supports_qr_grant = true;
    auto more = s.build_user_menu_items_(noop, noop, noop, noop, noop, noop);
    REQUIRE(more.size() == 7); // + verify + QR
    bool found_name = false;
    for (const auto& it : more)
    {
        found_name |= it.label.find("Me Myself") != std::string::npos;
    }
    CHECK(found_name);
}
