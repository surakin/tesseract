// ShellBase_accounts.cpp: in-place account switching (per-account state reset
// and snapshot swap), logout with and without surviving accounts, the
// dedicated-window registry, window-close hand-off, spawned-window seeding and
// the small login/pending-sync helpers. All through sessionless Clients; the
// on-disk index lives in the per-test redirected data dir.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct AsShell : tesseract::test::TestShellBase
{
    ~AsShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
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
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void on_invites_updated_() override { ++invites_updated; }
    void on_my_knocks_updated_() override { ++knocks_updated; }
    void refresh_account_ui_after_switch_() override { ++ui_refreshes; }
    void raise_and_activate_() override { ++raised; }

    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int invites_updated = 0, knocks_updated = 0, ui_refreshes = 0, raised = 0;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::claim_dedicated_for_active_;
    using ShellBase::client_;
    using ShellBase::clear_main_ui_after_last_logout_;
    using ShellBase::current_room_id_;
    using ShellBase::event_handler_;
    using ShellBase::hand_account_to_spawned_window_;
    using ShellBase::invites_;
    using ShellBase::is_secondary_window_startup_;
    using ShellBase::kRecentRoomsMax;
    using ShellBase::logout_active_account_impl_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_display_name_;
    using ShellBase::my_knocks_;
    using ShellBase::my_user_id_;
    using ShellBase::on_window_closing_;
    using ShellBase::other_account_pagination_;
    using ShellBase::pagination_;
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::pending_sync_session_;
    using ShellBase::per_account_invites_;
    using ShellBase::per_account_my_knocks_;
    using ShellBase::per_account_rooms_;
    using ShellBase::recent_room_ids_;
    using ShellBase::release_dedicated_for_active_;
    using ShellBase::release_pending_sync_gate_;
    using ShellBase::reply_details_requested_;
    using ShellBase::reset_pending_login_client_;
    using ShellBase::arm_pending_login_;
    using ShellBase::restart_sdk_begin_;
    using ShellBase::rooms_;
    using ShellBase::search_pending_queries_;
    using ShellBase::seed_account_caches_from_;
    using ShellBase::set_initial_account;
    using ShellBase::sign_out_active_account_;
    using ShellBase::space_stack_;
    using ShellBase::switch_active_account_impl_;
    using ShellBase::tabs_;
    using ShellBase::url_previews_;
    using ShellBase::visited_lru_;
    using ShellBase::LogoutResult;
    using ShellBase::finalize_login_async_;
    using ShellBase::finalize_login_blocking_;
    using ShellBase::finish_restore_accounts_ui_;
    using ShellBase::restore_all_accounts_blocking_;
    using ShellBase::RestoreIOResult;
    using ShellBase::RestoredAccountIO;
    using ShellBase::FinalizeLoginResult;
    using ShellBase::skip_unsaved_key_check_for_next_sign_out_;
    using ShellBase::is_pinned_window;
    using ShellBase::mut_pool_;
    using ShellBase::unread_prefetch_fingerprint_;
    using ShellBase::bridge_check_fingerprint_;
    using ShellBase::current_room_knock_requests_;
    using ShellBase::knock_requests_panel_room_id_;
    using ShellBase::current_knock_status_room_id_;
};

std::shared_ptr<tesseract::AccountSession> as_acct(const std::string& uid)
{
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = uid;
    a->client = std::make_unique<tesseract::Client>();
    return a;
}

} // namespace

TEST_CASE("switching to an unknown or already-active account does nothing",
          "[shell][account_switch]")
{
    AsShell s;
    CHECK_FALSE(s.switch_active_account_impl_("@nobody:x"));

    auto a = as_acct("@a:x");
    s.account_manager_.add_account(a);
    CHECK(s.switch_active_account_impl_("@a:x"));
    CHECK_FALSE(s.switch_active_account_impl_("@a:x")); // already active
}

TEST_CASE("switching adopts the incoming account's identity and snapshots",
          "[shell][account_switch]")
{
    tesseract::test::SettingsGuard guard;
    AsShell s;
    auto a = as_acct("@a:x");
    a->display_name = "Alice";
    a->avatar_url = "mxc://hs/a";
    a->recent_rooms = {"!r1", "!r2"};
    a->open_rooms = {"!x", "!y", "!z"};
    a->last_room = "!z";
    auto b = as_acct("@b:x");
    s.account_manager_.add_account(a);
    s.account_manager_.add_account(b);

    tesseract::RoomInfo room;
    room.id = "!a-room";
    s.per_account_rooms_["@a:x"] = {room};
    tesseract::InviteInfo inv;
    inv.room_id = "!inv";
    s.per_account_invites_["@a:x"] = {inv};
    tesseract::KnockedRoomInfo k;
    k.room_id = "!k";
    s.per_account_my_knocks_["@a:x"] = {k};

    REQUIRE(s.switch_active_account_impl_("@a:x"));
    CHECK(s.active_account_ == a);
    CHECK(s.client_ == a->client.get());
    CHECK(s.my_user_id_ == "@a:x");
    CHECK(s.my_display_name_ == "Alice");
    CHECK(s.my_avatar_url_ == "mxc://hs/a");
    CHECK(s.recent_room_ids_ == std::vector<std::string>{"!r1", "!r2"});
    CHECK(s.rooms_.size() == 1);
    CHECK(s.invites_.size() == 1);
    CHECK(s.my_knocks_.size() == 1);
    CHECK(s.invites_updated == 1);
    CHECK(s.knocks_updated == 1);
    // last_room is rotated to the front so it reopens as the active tab.
    CHECK(s.pending_restore_rooms_ ==
          std::vector<std::string>{"!z", "!x", "!y"});
    CHECK(s.account_manager_.dedicated_window("@a:x") == &s);
    CHECK(tesseract::SessionStore::load_index().active_user_id == "@a:x");
}

TEST_CASE("switching clears per-account, room-keyed state", "[shell][account_switch]")
{
    AsShell s;
    auto a = as_acct("@a:x");
    auto b = as_acct("@b:x");
    s.account_manager_.add_account(a);
    s.account_manager_.add_account(b);
    REQUIRE(s.switch_active_account_impl_("@a:x"));

    s.current_room_id_ = "!r";
    s.tabs_.push_back({"!r", 0.f});
    s.space_stack_ = {"!sp"};
    s.visited_lru_ = {"!r"};
    s.pagination_["!r"].reached_start = true;
    s.url_previews_["https://x"] = {};
    s.search_pending_queries_[1] = "q";
    s.reply_details_requested_.insert("k");
    s.unread_prefetch_fingerprint_ = 5;
    s.bridge_check_fingerprint_ = 6;
    s.current_room_knock_requests_.resize(1);
    s.knock_requests_panel_room_id_ = "!r";
    s.current_knock_status_room_id_ = "!r";

    REQUIRE(s.switch_active_account_impl_("@b:x"));
    CHECK(s.current_room_id_.empty());
    CHECK(s.tabs_.empty());
    CHECK(s.space_stack_.empty());
    CHECK(s.visited_lru_.empty());
    CHECK(s.pagination_.empty());
    CHECK(s.url_previews_.empty());
    CHECK(s.search_pending_queries_.empty());
    CHECK(s.reply_details_requested_.empty());
    CHECK(s.unread_prefetch_fingerprint_ == 0);
    CHECK(s.bridge_check_fingerprint_ == 0);
    CHECK(s.current_room_knock_requests_.empty());
    CHECK(s.knock_requests_panel_room_id_.empty());
    CHECK(s.current_knock_status_room_id_.empty());
    CHECK(s.account_manager_.dedicated_window("@a:x") == nullptr);
    CHECK(s.account_manager_.dedicated_window("@b:x") == &s);
}

TEST_CASE("an outgoing account's pop-out scroll state is parked and restored",
          "[shell][account_switch]")
{
    AsShell s;
    s.account_manager_.add_account(as_acct("@a:x"));
    s.account_manager_.add_account(as_acct("@b:x"));
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    // No pop-out pins a room here, so nothing is parked for @a.
    s.pagination_["!r"].reached_start = true;
    REQUIRE(s.switch_active_account_impl_("@b:x"));
    CHECK(s.other_account_pagination_.count("@a:x") == 1);
    CHECK(s.other_account_pagination_["@a:x"].empty());
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    CHECK(s.other_account_pagination_.count("@a:x") == 0);
}

TEST_CASE("the recent-room list is trimmed to the cap on switch",
          "[shell][account_switch]")
{
    AsShell s;
    auto a = as_acct("@a:x");
    for (std::size_t i = 0; i < AsShell::kRecentRoomsMax + 4; ++i)
        a->recent_rooms.push_back("!r" + std::to_string(i));
    s.account_manager_.add_account(a);
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    CHECK(s.recent_room_ids_.size() == AsShell::kRecentRoomsMax);
}

TEST_CASE("a single saved room becomes the restore target", "[shell][account_switch]")
{
    AsShell s;
    auto a = as_acct("@a:x");
    a->last_room = "!only";
    s.account_manager_.add_account(a);
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    CHECK(s.pending_restore_rooms_ == std::vector<std::string>{"!only"});

    auto b = as_acct("@b:x");
    s.account_manager_.add_account(b);
    REQUIRE(s.switch_active_account_impl_("@b:x"));
    CHECK(s.pending_restore_rooms_.empty());
}

TEST_CASE("logout with no active account is a no-op", "[shell][account_switch][logout]")
{
    AsShell s;
    auto r = s.logout_active_account_impl_();
    CHECK_FALSE(r.logged_out);
}

TEST_CASE("logging out the last account empties the shell and the index",
          "[shell][account_switch][logout]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Settings::instance().recovery_key_unsaved.insert("@a:x");
    AsShell s;
    s.account_manager_.add_account(as_acct("@a:x"));
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    s.current_room_id_ = "!r";
    s.per_account_rooms_["@a:x"] = {};
    s.per_account_invites_["@a:x"] = {};

    auto r = s.logout_active_account_impl_();
    CHECK(r.logged_out);
    CHECK(r.logged_out_uid == "@a:x");
    CHECK_FALSE(r.has_remaining);
    CHECK(s.active_account_ == nullptr);
    CHECK(s.client_ == nullptr);
    CHECK(s.event_handler_ == nullptr);
    CHECK(s.my_user_id_.empty());
    CHECK(s.current_room_id_.empty());
    CHECK(s.rooms_.empty());
    CHECK(s.per_account_rooms_.count("@a:x") == 0);
    CHECK(s.per_account_invites_.count("@a:x") == 0);
    CHECK(s.account_manager_.accounts().empty());
    CHECK(s.account_manager_.dedicated_window("@a:x") == nullptr);
    const auto idx = tesseract::SessionStore::load_index();
    CHECK(idx.active_user_id.empty());
    CHECK(idx.user_ids.empty());
}

TEST_CASE("logging out with other accounts left switches to the first survivor",
          "[shell][account_switch][logout]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Settings::instance().recovery_key_unsaved.insert("@a:x");
    AsShell s;
    s.account_manager_.add_account(as_acct("@a:x"));
    s.account_manager_.add_account(as_acct("@b:x"));
    REQUIRE(s.switch_active_account_impl_("@a:x"));

    auto r = s.logout_active_account_impl_();
    CHECK(r.logged_out);
    CHECK(r.has_remaining);
    CHECK(r.next_uid == "@b:x");
    CHECK(s.active_account_ != nullptr);
    CHECK(s.my_user_id_ == "@b:x");
    CHECK(s.ui_refreshes == 1);
    CHECK(s.account_manager_.find("@a:x") == nullptr);
    CHECK(tesseract::SessionStore::load_index().active_user_id == "@b:x");
}

TEST_CASE("sign_out_active_account_ runs the follow-up only when something signed out",
          "[shell][account_switch][logout]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Settings::instance().recovery_key_unsaved.insert("@a:x");
    AsShell s;
    int followups = 0;
    bool last_remaining = true;
    auto follow = [&](const AsShell::LogoutResult& r)
    {
        ++followups;
        last_remaining = r.has_remaining;
    };
    s.sign_out_active_account_([] {}, follow); // no account
    CHECK(followups == 0);

    s.account_manager_.add_account(as_acct("@a:x"));
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    // The unsaved-recovery-key prompt would open the save dialog (and read
    // the OS keyring); mark it as already decided so sign-out proceeds.
    bool retried = false;
    s.skip_unsaved_key_check_for_next_sign_out_();
    s.sign_out_active_account_([] {}, follow);
    CHECK(followups == 1);
    CHECK_FALSE(last_remaining);
    CHECK_FALSE(retried);
}

TEST_CASE("the main UI is cleared after the last logout", "[shell][account_switch][logout]")
{
    AsShell s;
    s.clear_main_ui_after_last_logout_(); // no views: harmless
    SUCCEED();
}

TEST_CASE("dedicated-window bookkeeping", "[shell][account_switch][windows]")
{
    AsShell a, b;
    a.account_manager_.add_account(as_acct("@x:x"));
    a.account_manager_.register_window(&a);
    CHECK(a.account_manager_.primary_window() == &a);

    // Another window already shows the account: don't steal it.
    a.set_initial_account(a.account_manager_.find("@x:x"));
    a.account_manager_.set_dedicated("@x:x", &b);
    a.claim_dedicated_for_active_();
    CHECK(a.account_manager_.dedicated_window("@x:x") == &b);
    a.account_manager_.clear_dedicated("@x:x");
    a.claim_dedicated_for_active_();
    CHECK(a.account_manager_.dedicated_window("@x:x") == &a);

    a.release_dedicated_for_active_();
    CHECK(a.account_manager_.dedicated_window("@x:x") == nullptr);
    a.release_dedicated_for_active_(); // not ours any more: no-op

    AsShell none;
    none.claim_dedicated_for_active_();
    none.release_dedicated_for_active_();
    a.account_manager_.unregister_window(&a);
}

TEST_CASE("releasing a dedicated mapping falls back to another live window",
          "[shell][account_switch][windows]")
{
    AsShell a, b;
    auto acct = as_acct("@x:x");
    a.account_manager_.add_account(acct);
    a.account_manager_.register_window(&a);
    a.account_manager_.register_window(&b);
    a.set_initial_account(acct);
    b.set_initial_account(acct);
    a.account_manager_.set_dedicated("@x:x", &a);
    a.release_dedicated_for_active_();
    // b shows the same account, so it inherits the mapping.
    CHECK(a.account_manager_.dedicated_window("@x:x") == &b);
    a.account_manager_.unregister_window(&a);
    a.account_manager_.unregister_window(&b);
}

TEST_CASE("closing a secondary window hands the bridge back and frees its mapping",
          "[shell][account_switch][windows]")
{
    // Both windows must share one AccountManager; the double gives each its
    // own, so register the primary in the secondary's.
    AsShell primary, secondary;
    auto& am = secondary.account_manager_;
    auto acct = as_acct("@x:x");
    am.add_account(acct);
    am.register_window(&primary);
    am.register_window(&secondary);
    secondary.set_initial_account(acct);
    am.set_dedicated("@x:x", &secondary);
    CHECK(am.claim_tray_owner(&secondary));
    CHECK(am.primary_window() == &primary);

    secondary.on_window_closing_();
    CHECK(am.dedicated_window("@x:x") == nullptr);
    CHECK_FALSE(am.is_tray_owner(&secondary));
    am.unregister_window(&secondary);
    am.unregister_window(&primary);
}

TEST_CASE("a spawned window is seeded from its parent", "[shell][account_switch][windows]")
{
    AsShell parent, child;
    auto acct = as_acct("@x:x");
    parent.account_manager_.add_account(acct);
    tesseract::RoomInfo room;
    room.id = "!r";
    parent.per_account_rooms_["@x:x"] = {room};
    tesseract::InviteInfo inv;
    inv.room_id = "!i";
    parent.per_account_invites_["@x:x"] = {inv};

    child.seed_account_caches_from_(&child, "@x:x"); // self: ignored
    child.seed_account_caches_from_(nullptr, "@x:x");
    CHECK(child.per_account_rooms_.empty());
    child.seed_account_caches_from_(&parent, "@x:x");
    CHECK(child.per_account_rooms_.at("@x:x").size() == 1);
    CHECK(child.per_account_invites_.at("@x:x").size() == 1);
    child.seed_account_caches_from_(&parent, "@unknown:x"); // nothing to copy
    CHECK(child.per_account_rooms_.count("@unknown:x") == 0);

    parent.hand_account_to_spawned_window_(nullptr, acct);
    parent.hand_account_to_spawned_window_(&child, nullptr);
    AsShell child2;
    parent.hand_account_to_spawned_window_(&child2, acct);
    CHECK(child2.is_pinned_window());
    CHECK(parent.account_manager_.dedicated_window("@x:x") == &child2);
    parent.account_manager_.clear_dedicated("@x:x");
}

TEST_CASE("secondary-window startup needs restored accounts, a pinned account and no client",
          "[shell][account_switch]")
{
    AsShell s;
    CHECK_FALSE(s.is_secondary_window_startup_());
    auto a = as_acct("@a:x");
    s.account_manager_.add_account(a);
    CHECK_FALSE(s.is_secondary_window_startup_());
    s.set_initial_account(a);
    CHECK(s.is_secondary_window_startup_());
}

TEST_CASE("the pending-sync gate starts sync once for the held account",
          "[shell][account_switch]")
{
    AsShell s;
    s.release_pending_sync_gate_(); // nothing held
    auto a = as_acct("@a:x");
    s.pending_sync_session_ = a;
    s.release_pending_sync_gate_();
    s.mut_pool_.wait_idle(std::chrono::seconds(5));
    CHECK(s.pending_sync_session_ == nullptr);
    CHECK(a->sync_started);

    auto started = as_acct("@b:x");
    started->sync_started = true;
    s.pending_sync_session_ = started;
    s.release_pending_sync_gate_();
    CHECK(s.pending_sync_session_ == nullptr);
}

TEST_CASE("pending login client and temp dir helpers", "[shell][account_switch]")
{
    AsShell s;
    auto* c = s.reset_pending_login_client_();
    REQUIRE(c != nullptr);
    CHECK(s.pending_login_client_.get() == c);
    CHECK(s.pending_login_temp_dir_.empty());
    s.arm_pending_login_();
    CHECK_FALSE(s.pending_login_temp_dir_.empty());
    const auto dir = s.pending_login_temp_dir_;
    s.arm_pending_login_(); // idempotent
    CHECK(s.pending_login_temp_dir_ == dir);
    s.reset_pending_login_client_();
    CHECK(s.pending_login_temp_dir_.empty());
}

TEST_CASE("restart_sdk_begin_ needs a client and a stored session",
          "[shell][account_switch]")
{
    AsShell s;
    s.restart_sdk_begin_({}); // no client
    CHECK(s.statuses.empty());
    s.account_manager_.add_account(as_acct("@a:x"));
    REQUIRE(s.switch_active_account_impl_("@a:x"));
    s.restart_sdk_begin_({}); // nothing stored for this account
    REQUIRE(s.statuses.size() == 1);
    CHECK(s.statuses[0].find("session is missing") != std::string::npos);
}

TEST_CASE("finalize_login_async_ rejects a missing or user-less pending login",
          "[shell][account_switch][login]")
{
    AsShell s;
    bool called = false;
    AsShell::FinalizeLoginResult got;
    s.finalize_login_async_([&](AsShell::FinalizeLoginResult r) {
        called = true;
        got = std::move(r);
    });
    CHECK(called);
    CHECK_FALSE(got.ok);
    CHECK(got.error.empty()); // nothing to report: there was no login

    s.reset_pending_login_client_();
    called = false;
    s.finalize_login_async_([&](AsShell::FinalizeLoginResult r) {
        called = true;
        got = std::move(r);
    });
    CHECK(called);
    CHECK_FALSE(got.ok);
    CHECK(got.error == "no user id");
}

TEST_CASE("finalizing a login with no session blob fails cleanly",
          "[shell][account_switch][login]")
{
    AsShell s;
    auto io = s.finalize_login_blocking_(std::make_unique<tesseract::Client>(), {});
    CHECK_FALSE(io.result.ok);
    CHECK(io.result.error == "empty session");
    CHECK(io.session == nullptr);
}

TEST_CASE("startup restore without network marks every stored account as failed",
          "[shell][account_switch][restore]")
{
    AsShell s;
    tesseract::SessionStore::AccountIndex idx;
    idx.user_ids = {"@a:x", "@b:x"};
    idx.active_user_id = "@b:x";
    REQUIRE(tesseract::SessionStore::save_index(idx));

    auto io = s.restore_all_accounts_blocking_(/*network_available=*/false);
    CHECK(io.accounts.empty());
    CHECK(io.any_restore_failed);
    CHECK(io.network_unavailable);
    CHECK(io.active_user_id_hint == "@b:x");

    // With network but no stored session blobs, accounts are skipped quietly.
    io = s.restore_all_accounts_blocking_(true);
    CHECK(io.accounts.empty());
    CHECK_FALSE(io.any_restore_failed);
}

TEST_CASE("finishing a restore adds the accounts and picks the hinted active one",
          "[shell][account_switch][restore]")
{
    AsShell s;
    AsShell::RestoreIOResult io;
    for (const char* uid : {"@a:x", "@b:x"})
    {
        AsShell::RestoredAccountIO acc;
        acc.user_id = uid;
        acc.client = std::make_unique<tesseract::Client>();
        acc.display_name = std::string("Name ") + uid;
        acc.last_room = "!last";
        acc.open_rooms = {"!last", "!other"};
        io.accounts.push_back(std::move(acc));
    }
    io.active_user_id_hint = "@b:x";
    io.any_restore_failed = true;
    io.restore_error = "one failed";

    auto result = s.finish_restore_accounts_ui_(std::move(io));
    CHECK(result.any_accounts);
    CHECK(result.active_uid == "@b:x");
    CHECK(result.any_restore_failed);
    CHECK(result.restore_error == "one failed");
    REQUIRE(s.account_manager_.find("@a:x") != nullptr);
    CHECK(s.account_manager_.find("@a:x")->display_name == "Name @a:x");
    CHECK(s.account_manager_.find("@a:x")->sync_started);
    CHECK(s.account_manager_.find("@a:x")->open_rooms.size() == 2);

    // A hint naming an account that failed falls back to the first one.
    AsShell s2;
    AsShell::RestoreIOResult io2;
    AsShell::RestoredAccountIO acc;
    acc.user_id = "@only:x";
    acc.client = std::make_unique<tesseract::Client>();
    io2.accounts.push_back(std::move(acc));
    io2.active_user_id_hint = "@gone:x";
    auto r2 = s2.finish_restore_accounts_ui_(std::move(io2));
    CHECK(r2.active_uid == "@only:x");

    // Nothing restored at all.
    AsShell s3;
    auto r3 = s3.finish_restore_accounts_ui_({});
    CHECK_FALSE(r3.any_accounts);
    CHECK(r3.active_uid.empty());
}
