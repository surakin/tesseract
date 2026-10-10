// ShellBase_rooms.cpp: room-list ingestion (push_rooms_ / invites / knocks /
// list state), the async room-action completion routing (join, leave, create,
// knock, invite, moderation, space children), their commands, join/create
// outcome reporting into AddRoomView, and refresh_room_list_.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk_test_surface.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/JoinRoomView.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using RaKind = ShellBase::RoomActionKind;

namespace
{

struct RaShell : tesseract::test::TestShellBase
{
    ~RaShell() override
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
    void on_rooms_updated_() override { ++rooms_updated; }
    void on_invites_updated_() override { ++invites_updated; }
    void on_my_knocks_updated_() override { ++knocks_updated; }
    void request_relayout_() override { ++relayouts; }

    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int rooms_updated = 0, invites_updated = 0, knocks_updated = 0,
        relayouts = 0;

    using ShellBase::accept_invite_async_;
    using ShellBase::accept_knock_request_async_;
    using ShellBase::active_account_;
    using ShellBase::block_invite_async_;
    using ShellBase::bridge_check_fingerprint_;
    using ShellBase::client_;
    using ShellBase::confirm_leave_room_;
    using ShellBase::create_room_command_;
    using ShellBase::current_room_id_;
    using ShellBase::current_room_knock_requests_;
    using ShellBase::decline_invite_async_;
    using ShellBase::find_invite_;
    using ShellBase::find_my_knock_;
    using ShellBase::handle_knock_requests_updated_ui_;
    using ShellBase::handle_room_action_complete_ui_;
    using ShellBase::handle_upload_complete_ui_;
    using ShellBase::handle_upload_progress_ui_;
    using ShellBase::hidden_rooms_;
    using ShellBase::invite_user_command_;
    using ShellBase::invite_users_;
    using ShellBase::invites_;
    using ShellBase::join_room_command_;
    using ShellBase::join_room_lookup_gen_;
    using ShellBase::knock_requests_panel_account_;
    using ShellBase::knock_requests_panel_room_id_;
    using ShellBase::knock_room_command_;
    using ShellBase::known_users_room_set_hash_;
    using ShellBase::kRestoreGateMaxTicks;
    using ShellBase::pool_;
    using ShellBase::ModerationAction;
    using ShellBase::last_room_list_state_;
    using ShellBase::leave_room_command_;
    using ShellBase::leave_space_navigate_back_;
    using ShellBase::lookup_room_command_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::moderate_member_;
    using ShellBase::my_knocks_;
    using ShellBase::my_user_id_;
    using ShellBase::next_room_action_id_;
    using ShellBase::pending_event_scroll_after_join_;
    using ShellBase::pending_invites_;
    using ShellBase::pending_join_via_;
    using ShellBase::pending_matrix_link_;
    using ShellBase::pending_moderations_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::pending_room_actions_;
    using ShellBase::per_account_invites_;
    using ShellBase::per_account_my_knocks_;
    using ShellBase::per_account_rooms_;
    using ShellBase::push_invites_;
    using ShellBase::push_my_knocks_;
    using ShellBase::push_room_list_state_;
    using ShellBase::push_rooms_;
    using ShellBase::refresh_room_list_;
    using ShellBase::restore_gate_ticks_;
    using ShellBase::retract_knock_command_;
    using ShellBase::room_search_text_;
    using ShellBase::rooms_;
    using ShellBase::set_room_favourite_;
    using ShellBase::set_room_low_priority_;
    using ShellBase::set_room_notification_mode_;
    using ShellBase::space_back_command_;
    using ShellBase::space_children_cache_;
    using ShellBase::space_nav_frames_;
    using ShellBase::space_root_shown_id_;
    using ShellBase::space_stack_;
    using ShellBase::subscribe_knock_requests_panel_;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::unread_prefetch_fingerprint_;
    using ShellBase::unsubscribe_knock_requests_panel_;
    using ShellBase::on_create_room_outcome_ui_;
    using ShellBase::on_join_room_outcome_ui_;
    using ShellBase::on_knock_requests_panel_updated_;
};

tesseract::RoomInfo ra_room(const std::string& id, bool space = false)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = "N" + id;
    r.is_space = space;
    return r;
}

struct RaFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    RaShell s;
    RaFx()
    {
        s.client_ = &client;
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.my_user_id_ = "@me:x";
    }
};

struct RaAppFx : RaFx
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    RaAppFx() { s.main_app_ = app.get(); }
};

} // namespace

// ── push_* ingestion ─────────────────────────────────────────────────────────

TEST_CASE("push_rooms_ for another account only updates its cache",
          "[shell][rooms]")
{
    RaFx f;
    f.s.rooms_ = {ra_room("!mine")};
    f.s.push_rooms_("@other:x", {ra_room("!theirs")});
    CHECK(f.s.per_account_rooms_.at("@other:x").size() == 1);
    CHECK(f.s.rooms_.size() == 1);
    CHECK(f.s.rooms_[0].id == "!mine");
    CHECK(f.s.rooms_updated == 0);
}

TEST_CASE("push_rooms_ for the active account replaces the list and drops "
          "shadowing hidden rooms",
          "[shell][rooms]")
{
    RaFx f;
    f.s.hidden_rooms_["!old"] = ra_room("!old");
    f.s.hidden_rooms_["!still-hidden"] = ra_room("!still-hidden");
    f.s.push_rooms_("@me:x", {ra_room("!a"), ra_room("!old")});
    CHECK(f.s.rooms_.size() == 2);
    CHECK(f.s.hidden_rooms_.count("!old") == 0);
    CHECK(f.s.hidden_rooms_.count("!still-hidden") == 1);
    CHECK(f.s.per_account_rooms_.at("@me:x").size() == 2);
    CHECK(f.s.rooms_updated == 1);
    CHECK(f.s.known_users_room_set_hash_ != 0);
}

TEST_CASE("push_rooms_ warms bridge status and unread rooms once per room set",
          "[shell][rooms]")
{
    RaFx f;
    auto fav = ra_room("!fav");
    fav.is_favorite = true;
    f.s.push_rooms_("@me:x", {fav, ra_room("!b")});
    const auto bridge_fp = f.s.bridge_check_fingerprint_;
    const auto unread_fp = f.s.unread_prefetch_fingerprint_;
    CHECK(bridge_fp != 0);
    CHECK(unread_fp != 0);

    f.s.push_rooms_("@me:x", {fav, ra_room("!b")}); // same set: unchanged
    CHECK(f.s.bridge_check_fingerprint_ == bridge_fp);
    CHECK(f.s.unread_prefetch_fingerprint_ == unread_fp);

    f.s.push_rooms_("@me:x", {fav, ra_room("!b"), ra_room("!c")});
    CHECK(f.s.bridge_check_fingerprint_ != bridge_fp);
}

TEST_CASE("push_rooms_ holds back warming while a layout restore is pending",
          "[shell][rooms]")
{
    RaFx f;
    f.s.pending_restore_rooms_ = {"!saved"};
    f.s.push_rooms_("@me:x", {ra_room("!a")});
    CHECK(f.s.restore_gate_ticks_ == 1);
    CHECK(f.s.bridge_check_fingerprint_ == 0);

    // After enough ticks the gate gives up.
    for (int i = 0; i < RaShell::kRestoreGateMaxTicks; ++i)
        f.s.push_rooms_("@me:x", {ra_room("!a")});
    CHECK(f.s.bridge_check_fingerprint_ != 0);
}

TEST_CASE("push_rooms_ replays a matrix link that arrived before login",
          "[shell][rooms]")
{
    RaFx f;
    f.s.pending_matrix_link_ = "https://matrix.to/#/!room:x";
    f.s.push_rooms_("@me:x", {ra_room("!a")});
    CHECK(f.s.pending_matrix_link_.empty());
}

TEST_CASE("push_rooms_ reveals a just-created space once sync reports it",
          "[shell][rooms]")
{
    RaAppFx f;
    f.s.current_room_id_ = "!space";
    f.s.push_rooms_("@me:x", {ra_room("!space", true)});
    CHECK(f.s.space_root_shown_id_ == "!space");
}

TEST_CASE("invites: push, find, accept, decline, block", "[shell][rooms][invites]")
{
    RaFx f;
    tesseract::InviteInfo a;
    a.room_id = "!a";
    a.is_direct = true;
    a.inviter_avatar_url = "mxc://hs/inviter";
    tesseract::InviteInfo b;
    b.room_id = "!b";
    b.room_avatar_url = "mxc://hs/room";
    f.s.push_invites_("@other:x", {a});
    CHECK(f.s.invites_.empty());
    CHECK(f.s.invites_updated == 0);

    f.s.push_invites_("@me:x", {a, b});
    CHECK(f.s.invites_.size() == 2);
    CHECK(f.s.invites_updated == 1);
    REQUIRE(f.s.find_invite_("!a") != nullptr);
    CHECK(f.s.find_invite_("!zz") == nullptr);

    f.s.accept_invite_async_("");
    CHECK(f.s.pending_room_actions_.empty());
    f.s.accept_invite_async_("!a");
    REQUIRE(f.s.pending_room_actions_.size() == 1);
    CHECK(f.s.pending_room_actions_.begin()->second.kind == RaKind::Accept);

    f.s.decline_invite_async_("!a");
    CHECK(f.s.invites_.size() == 1);
    f.s.block_invite_async_("!b", "@x:x");
    CHECK(f.s.invites_.empty());
    CHECK(f.s.invites_updated == 3);

    f.s.client_ = nullptr;
    f.s.decline_invite_async_("!b");
    f.s.block_invite_async_("!b", "@x:x");
    f.s.accept_invite_async_("!b");
    CHECK(f.s.invites_updated == 3);
}

TEST_CASE("invite avatars follow the MSC4278 setting", "[shell][rooms][invites]")
{
    RaFx f;
    tesseract::Settings::instance().invite_avatars = false;
    tesseract::InviteInfo a;
    a.room_id = "!a";
    a.room_avatar_url = "mxc://hs/room";
    f.s.push_invites_("@me:x", {a});
    tesseract::Settings::instance().invite_avatars = true;
    f.s.push_invites_("@me:x", {a});
    SUCCEED();
}

TEST_CASE("knocks: push and find", "[shell][rooms][knock]")
{
    RaFx f;
    tesseract::KnockedRoomInfo k;
    k.room_id = "!k";
    f.s.push_my_knocks_("@other:x", {k});
    CHECK(f.s.my_knocks_.empty());
    f.s.push_my_knocks_("@me:x", {k});
    CHECK(f.s.knocks_updated == 1);
    CHECK(f.s.find_my_knock_("!k") != nullptr);
    CHECK(f.s.find_my_knock_("!no") == nullptr);
}

TEST_CASE("knock commands register pending actions", "[shell][rooms][knock]")
{
    RaFx f;
    f.s.knock_room_command_("", "why", {});
    CHECK(f.s.pending_room_actions_.empty());
    f.s.pending_join_via_["#alias:x"] = {"hs.example"};
    f.s.knock_room_command_("#alias:x", "please", {});
    REQUIRE(f.s.pending_room_actions_.size() == 1);
    CHECK(f.s.pending_room_actions_.begin()->second.kind == RaKind::Knock);

    f.s.accept_knock_request_async_("", "@u:x");
    f.s.accept_knock_request_async_("!r", "");
    CHECK(f.s.pending_room_actions_.size() == 1);
    f.s.accept_knock_request_async_("!r", "@u:x");
    CHECK(f.s.pending_room_actions_.size() == 2);

    f.s.retract_knock_command_("!r"); // same as leaving
    CHECK(f.s.pending_room_actions_.size() == 3);
}

TEST_CASE("knock request panel subscription is idempotent and releasable",
          "[shell][rooms][knock]")
{
    RaFx f;
    f.s.subscribe_knock_requests_panel_("", nullptr);
    CHECK(f.s.knock_requests_panel_room_id_.empty());
    f.s.subscribe_knock_requests_panel_("!r", nullptr);
    CHECK(f.s.knock_requests_panel_room_id_ == "!r");
    f.s.current_room_knock_requests_.resize(2);
    f.s.subscribe_knock_requests_panel_("!r", nullptr); // already subscribed
    CHECK(f.s.current_room_knock_requests_.size() == 2);
    f.s.subscribe_knock_requests_panel_("!r2", nullptr); // moves
    CHECK(f.s.knock_requests_panel_room_id_ == "!r2");
    CHECK(f.s.current_room_knock_requests_.empty());

    // A poke for the subscribed room refreshes the list (empty offline).
    f.s.current_room_knock_requests_.resize(1);
    f.s.handle_knock_requests_updated_ui_("!other"); // stale: ignored
    CHECK(f.s.current_room_knock_requests_.size() == 1);
    f.s.handle_knock_requests_updated_ui_("!r2");
    CHECK(f.s.current_room_knock_requests_.empty());

    f.s.unsubscribe_knock_requests_panel_();
    CHECK(f.s.knock_requests_panel_room_id_.empty());
    f.s.unsubscribe_knock_requests_panel_(); // already idle
}

TEST_CASE("room list state remembers the latest state", "[shell][rooms]")
{
    RaFx f;
    tesseract::Settings::instance().check_for_updates = false;
    f.s.push_room_list_state_(tesseract::RoomListState::Running);
    CHECK(f.s.last_room_list_state_ == tesseract::RoomListState::Running);
    f.s.client_ = nullptr;
    f.s.push_room_list_state_(tesseract::RoomListState::Recovering);
    CHECK(f.s.last_room_list_state_ == tesseract::RoomListState::Recovering);
}

TEST_CASE("uploads report failures on the status line", "[shell][rooms]")
{
    RaFx f;
    f.s.handle_upload_progress_ui_(1, 5, 10);
    f.s.handle_upload_complete_ui_(1, true, "");
    CHECK(f.s.statuses.empty());
    f.s.handle_upload_complete_ui_(2, false, "");
    f.s.handle_upload_complete_ui_(3, false, "too big");
    REQUIRE(f.s.statuses.size() == 2);
    CHECK(f.s.statuses[0] == "Upload failed");
    CHECK(f.s.statuses[1] == "Upload failed: too big");
}

// ── commands that register pending work ─────────────────────────────────────

TEST_CASE("simple room setting commands tolerate a missing client",
          "[shell][rooms][commands]")
{
    RaFx f;
    f.s.set_room_notification_mode_("!r", "mute", nullptr);
    f.s.set_room_favourite_("!r", true, nullptr);
    f.s.set_room_low_priority_("!r", true, nullptr);
    f.s.client_ = nullptr;
    f.s.set_room_notification_mode_("!r", "mute", nullptr);
    f.s.set_room_favourite_("!r", true, nullptr);
    f.s.set_room_low_priority_("!r", true, nullptr);
    SUCCEED();
}

TEST_CASE("leave tags spaces separately from rooms", "[shell][rooms][commands]")
{
    RaFx f;
    f.s.rooms_ = {ra_room("!sp", true), ra_room("!r")};
    f.s.mark_room_index_dirty_();
    f.s.leave_room_command_("", nullptr);
    CHECK(f.s.pending_room_actions_.empty());
    f.s.leave_room_command_("!sp", nullptr);
    f.s.leave_room_command_("!r", nullptr);
    REQUIRE(f.s.pending_room_actions_.size() == 2);
    int spaces = 0, rooms = 0;
    for (auto& [id, a] : f.s.pending_room_actions_)
    {
        spaces += a.kind == RaKind::LeaveSpace;
        rooms += a.kind == RaKind::Leave;
    }
    CHECK(spaces == 1);
    CHECK(rooms == 1);
}

TEST_CASE("join registers a pending action and uses stashed via hints",
          "[shell][rooms][commands]")
{
    RaFx f;
    f.s.join_room_command_("", {}, nullptr);
    CHECK(f.s.pending_room_actions_.empty());
    f.s.pending_join_via_["#a:x"] = {"hs"};
    f.s.join_room_command_("#a:x", {}, nullptr);
    REQUIRE(f.s.pending_room_actions_.size() == 1);
    CHECK(f.s.pending_room_actions_.begin()->second.kind == RaKind::Join);
    CHECK(f.s.pending_room_actions_.begin()->second.room_id == "#a:x");
    f.s.client_ = nullptr;
    f.s.join_room_command_("#b:x", {"hs"}, nullptr);
    CHECK(f.s.pending_room_actions_.size() == 1);
}

TEST_CASE("create_room_command_ needs a client and flips the form to Creating",
          "[shell][rooms][commands]")
{
    RaAppFx f;
    f.app->add_room_view()->open(tesseract::views::AddRoomView::Tab::Create);
    f.s.create_room_command_({});
    REQUIRE(f.s.pending_room_actions_.size() == 1);
    CHECK(f.s.pending_room_actions_.begin()->second.kind == RaKind::Create);
    CHECK(f.app->add_room_view()->create_view()->state() ==
          tesseract::views::CreateRoomView::State::Creating);

    f.s.client_ = nullptr;
    f.s.create_room_command_({});
    CHECK(f.s.pending_room_actions_.size() == 1);
}

TEST_CASE("invite / moderation / bulk invite register tracked requests",
          "[shell][rooms][commands]")
{
    RaFx f;
    f.s.invite_user_command_("", "@u:x", "", nullptr);
    f.s.invite_user_command_("!r", "", "", nullptr);
    CHECK(f.s.pending_invites_.empty());
    f.s.invite_user_command_("!r", "@u:x", "hi", nullptr);
    CHECK(f.s.pending_invites_.size() == 1);

    using MA = RaShell::ModerationAction;
    f.s.moderate_member_(MA::Kick, "", "@u:x", "U", "", {}, nullptr);
    CHECK(f.s.pending_moderations_.empty());
    f.s.moderate_member_(MA::Kick, "!r", "@u:x", "U", "", {}, nullptr);
    f.s.moderate_member_(MA::Ban, "!r", "@u:x", "U", "spam", {}, nullptr);
    f.s.moderate_member_(MA::Unban, "!r", "@u:x", "U", "", {}, nullptr);
    CHECK(f.s.pending_moderations_.size() == 3);

    std::vector<std::string> results;
    f.s.invite_users_("!r", {"@a:x", "@b:x"},
                      [&](const std::string& u, bool ok, const std::string&)
                      { results.push_back(u + (ok ? "+" : "-")); },
                      nullptr);
    CHECK(f.s.pending_invites_.size() == 3);
    f.s.invite_users_("", {"@a:x"}, {}, nullptr);
    CHECK(f.s.pending_invites_.size() == 3);

    // Completing each request reaches its callback.
    for (auto it = f.s.pending_invites_.begin(); it != f.s.pending_invites_.end();)
    {
        auto id = it->first;
        ++it;
        f.s.handle_room_action_complete_ui_(id, false, "", "denied");
    }
    CHECK(results.size() == 2);
    CHECK(f.s.pending_invites_.empty());
}

TEST_CASE("confirm_leave_room_ asks first and leaves on confirmation",
          "[shell][rooms][commands]")
{
    RaAppFx f;
    f.s.rooms_ = {ra_room("!r")};
    f.s.mark_room_index_dirty_();
    f.s.confirm_leave_room_("");
    f.s.confirm_leave_room_("!r");
    CHECK(f.app->confirm_dialog()->is_open());
    CHECK(f.s.pending_room_actions_.empty()); // nothing until confirmed
}

TEST_CASE("lookup_room_command_ shows the loading state and applies a failure",
          "[shell][rooms][commands]")
{
    RaAppFx f;
    auto* ar = f.app->add_room_view();
    ar->open(tesseract::views::AddRoomView::Tab::Join);
    f.s.lookup_room_command_("#a:x");
    CHECK(ar->join_view()->state() == tesseract::views::JoinRoomView::State::Loading);
    CHECK(f.s.join_room_lookup_gen_ == 1);
    f.s.pool_.wait_idle(std::chrono::seconds(5)); // offline: failed summary
    CHECK(ar->join_view()->state() == tesseract::views::JoinRoomView::State::Error);

    // A newer lookup supersedes an older one's late result.
    f.s.lookup_room_command_("#b:x");
    f.s.lookup_room_command_("#c:x");
    f.s.pool_.wait_idle(std::chrono::seconds(5));
    CHECK(f.s.join_room_lookup_gen_ == 3);
}

// ── completion routing ───────────────────────────────────────────────────────

TEST_CASE("invite completion goes to the per-request callback or the status line",
          "[shell][rooms][complete]")
{
    RaFx f;
    bool got_ok = true;
    std::string got_msg;
    f.s.pending_invites_[1] = {"!r", "@u:x",
                               [&](bool ok, const std::string& m)
                               {
                                   got_ok = ok;
                                   got_msg = m;
                               }};
    f.s.handle_room_action_complete_ui_(1, false, "", "nope");
    CHECK_FALSE(got_ok);
    CHECK(got_msg == "nope");
    CHECK(f.s.statuses.empty());

    f.s.pending_invites_[2] = {"!r", "@u:x", nullptr};
    f.s.handle_room_action_complete_ui_(2, false, "", "");
    f.s.pending_invites_[3] = {"!r", "@u:x", nullptr};
    f.s.handle_room_action_complete_ui_(3, false, "", "denied");
    f.s.pending_invites_[4] = {"!r", "@u:x", nullptr};
    f.s.handle_room_action_complete_ui_(4, true, "", "");
    REQUIRE(f.s.statuses.size() == 2);
    CHECK(f.s.statuses[0] == "Couldn't invite @u:x");
    CHECK(f.s.statuses[1] == "Couldn't invite @u:x: denied");
}

TEST_CASE("moderation completion reports each failing action",
          "[shell][rooms][complete]")
{
    using MA = RaShell::ModerationAction;
    RaFx f;
    std::vector<bool> done;
    auto cb = [&](bool ok) { done.push_back(ok); };
    f.s.pending_moderations_[1] = {"@u:x", "", MA::Kick, cb};
    f.s.pending_moderations_[2] = {"@u:x", "Uma", MA::Ban, cb};
    f.s.pending_moderations_[3] = {"@u:x", "Uma", MA::Unban, cb};
    f.s.pending_moderations_[4] = {"@u:x", "Uma", MA::Kick, cb};
    f.s.handle_room_action_complete_ui_(1, false, "", "");
    f.s.handle_room_action_complete_ui_(2, false, "", "no power");
    f.s.handle_room_action_complete_ui_(3, false, "", "");
    f.s.handle_room_action_complete_ui_(4, true, "", "");
    REQUIRE(f.s.statuses.size() == 3);
    CHECK(f.s.statuses[0] == "Couldn't kick @u:x");
    CHECK(f.s.statuses[1] == "Couldn't ban Uma: no power");
    CHECK(f.s.statuses[2] == "Couldn't unban Uma");
    CHECK(done == std::vector<bool>{false, false, false, true});
}

TEST_CASE("unknown completion ids are ignored", "[shell][rooms][complete]")
{
    RaFx f;
    f.s.handle_room_action_complete_ui_(404, true, "!x", "");
    CHECK(f.s.statuses.empty());
}

TEST_CASE("failed room actions name the action and clean up",
          "[shell][rooms][complete]")
{
    RaFx f;
    struct Case
    {
        RaKind kind;
        const char* verb;
    };
    const Case cases[] = {
        {RaKind::Accept, "accept invite"},
        {RaKind::Join, "join room"},
        {RaKind::Leave, "leave room"},
        {RaKind::LeaveSpace, "leave space"},
        {RaKind::Create, "create room"},
        {RaKind::Knock, "send join request"},
        {RaKind::AcceptKnock, "accept join request"},
        {RaKind::AddSpaceChild, "add room to space"},
        {RaKind::RemoveSpaceChild, "remove room from space"},
    };
    std::uint64_t id = 100;
    for (const auto& c : cases)
    {
        f.s.statuses.clear();
        f.s.pending_room_actions_[id] = {"!r", c.kind, "!space"};
        f.s.pending_event_scroll_after_join_["!r"] = "$e";
        f.s.handle_room_action_complete_ui_(id, false, "", "boom");
        REQUIRE(f.s.statuses.size() == 1);
        CHECK(f.s.statuses[0] == std::string("Couldn't ") + c.verb + ": boom");
        CHECK(f.s.pending_room_actions_.count(id) == 0);
        CHECK(f.s.pending_event_scroll_after_join_.count("!r") ==
              (c.kind == RaKind::Join ? 0u : 1u));
        ++id;
    }
    // Without a message there is no suffix.
    f.s.statuses.clear();
    f.s.pending_room_actions_[id] = {"!r", RaKind::Leave, ""};
    f.s.handle_room_action_complete_ui_(id, false, "", "");
    CHECK(f.s.statuses[0] == "Couldn't leave room");
}

TEST_CASE("a failed AddSpaceChild reverts the optimistic cache entry",
          "[shell][rooms][complete]")
{
    RaFx f;
    f.s.space_children_cache_["!space"] = {"!keep", "!added"};
    f.s.pending_room_actions_[1] = {"!added", RaKind::AddSpaceChild, "!space"};
    f.s.handle_room_action_complete_ui_(1, false, "", "x");
    CHECK(f.s.space_children_cache_["!space"] ==
          std::vector<std::string>{"!keep"});
}

TEST_CASE("successful accept selects the room", "[shell][rooms][complete]")
{
    RaFx f;
    f.s.pending_room_actions_[1] = {"!a", RaKind::Accept, ""};
    f.s.handle_room_action_complete_ui_(1, true, "", "");
    CHECK(f.s.current_room_id_ == "!a");
    CHECK(f.s.tabs_.size() == 1);
}

TEST_CASE("successful join navigates and honours a pending permalink scroll",
          "[shell][rooms][complete]")
{
    RaFx f;
    f.s.pending_event_scroll_after_join_["!joined"] = "$e";
    f.s.pending_room_actions_[1] = {"#alias:x", RaKind::Join, ""};
    f.s.handle_room_action_complete_ui_(1, true, "!joined", "");
    CHECK(f.s.current_room_id_ == "!joined");
    CHECK(f.s.pending_event_scroll_after_join_.empty());

    // No resolved room id: reports the join but does not navigate.
    f.s.pending_room_actions_[2] = {"!other", RaKind::Join, ""};
    f.s.handle_room_action_complete_ui_(2, true, "", "");
    CHECK(f.s.current_room_id_ == "!joined");
}

TEST_CASE("successful create navigates to the new room", "[shell][rooms][complete]")
{
    RaFx f;
    f.s.pending_room_actions_[1] = {"", RaKind::Create, ""};
    f.s.handle_room_action_complete_ui_(1, true, "!new", "");
    CHECK(f.s.current_room_id_ == "!new");
    f.s.pending_room_actions_[2] = {"", RaKind::Create, ""};
    f.s.handle_room_action_complete_ui_(2, true, "", "");
    CHECK(f.s.current_room_id_ == "!new");
}

TEST_CASE("leaving a room closes its tab, or deselects the last one",
          "[shell][rooms][complete]")
{
    RaFx f;
    f.s.tab_select_room("!a");
    f.s.tabs_.push_back({"!b", 0.f});
    f.s.pending_room_actions_[1] = {"!b", RaKind::Leave, ""};
    f.s.handle_room_action_complete_ui_(1, true, "", "");
    CHECK(f.s.tabs_.size() == 1);

    f.s.pending_room_actions_[2] = {"!a", RaKind::Leave, ""};
    f.s.handle_room_action_complete_ui_(2, true, "", "");
    CHECK(f.s.current_room_id_.empty());
    CHECK(f.s.relayouts >= 1);
}

TEST_CASE("leaving a space steps back out of it", "[shell][rooms][complete]")
{
    RaAppFx f;
    f.s.rooms_ = {ra_room("!space", true)};
    f.s.mark_room_index_dirty_();
    f.s.space_stack_ = {"!space"};
    f.s.current_room_id_ = "!space";
    f.s.pending_room_actions_[1] = {"!space", RaKind::LeaveSpace, ""};
    f.s.handle_room_action_complete_ui_(1, true, "", "");
    CHECK(f.s.space_stack_.empty());
    CHECK(f.s.current_room_id_.empty());
    CHECK(f.s.space_root_shown_id_.empty());
}

TEST_CASE("successful knock confirms and the others complete quietly",
          "[shell][rooms][complete]")
{
    RaFx f;
    f.s.pending_room_actions_[1] = {"!k", RaKind::Knock, ""};
    f.s.handle_room_action_complete_ui_(1, true, "", "");
    REQUIRE(f.s.statuses.size() == 1);
    CHECK(f.s.statuses[0] == "Request sent");
    f.s.pending_room_actions_[2] = {"!k", RaKind::AcceptKnock, ""};
    f.s.pending_room_actions_[3] = {"!k", RaKind::AddSpaceChild, "!s"};
    f.s.pending_room_actions_[4] = {"!k", RaKind::RemoveSpaceChild, "!s"};
    f.s.handle_room_action_complete_ui_(2, true, "", "");
    f.s.handle_room_action_complete_ui_(3, true, "", "");
    f.s.handle_room_action_complete_ui_(4, true, "", "");
    CHECK(f.s.statuses.size() == 1);
    CHECK(f.s.pending_room_actions_.empty());
}

TEST_CASE("leave_space_navigate_back_ pops only a matching stack top",
          "[shell][rooms][complete]")
{
    RaFx f;
    f.s.space_stack_ = {"!outer", "!inner"};
    f.s.leave_space_navigate_back_("!other");
    CHECK(f.s.space_stack_.size() == 2);
    f.s.leave_space_navigate_back_("!inner");
    CHECK(f.s.space_stack_ == std::vector<std::string>{"!outer"});
}

// ── join / create outcome → AddRoomView ─────────────────────────────────────

TEST_CASE("join outcome closes the dialog on success and shows the error on failure",
          "[shell][rooms][outcome]")
{
    using AR = tesseract::views::AddRoomView;
    RaAppFx f;
    f.s.on_join_room_outcome_ui_(false, "!r", "x"); // closed dialog: no-op
    auto* ar = f.app->add_room_view();

    ar->open(AR::Tab::Join);
    f.s.on_join_room_outcome_ui_(false, "!r", "denied");
    CHECK(ar->is_open());
    CHECK(ar->join_view()->state() ==
          tesseract::views::JoinRoomView::State::Error);
    f.s.on_join_room_outcome_ui_(false, "!r", "");
    f.s.on_join_room_outcome_ui_(true, "!r", "");
    CHECK_FALSE(ar->is_open());

    ar->open(AR::Tab::Directory);
    f.s.on_join_room_outcome_ui_(false, "!r", "denied");
    f.s.on_join_room_outcome_ui_(false, "!r", "");
    CHECK(ar->is_open());
    f.s.on_join_room_outcome_ui_(true, "!r", "");
    CHECK_FALSE(ar->is_open());

    ar->open(AR::Tab::Create); // neither Join nor Directory: untouched
    f.s.on_join_room_outcome_ui_(true, "!r", "");
    CHECK(ar->is_open());
}

TEST_CASE("create outcome closes the dialog or shows the error",
          "[shell][rooms][outcome]")
{
    using AR = tesseract::views::AddRoomView;
    RaAppFx f;
    auto* ar = f.app->add_room_view();
    f.s.on_create_room_outcome_ui_(true, "!r", "");
    ar->open(AR::Tab::Create);
    f.s.on_create_room_outcome_ui_(false, "", "taken");
    CHECK(ar->is_open());
    CHECK(ar->create_view()->state() ==
          tesseract::views::CreateRoomView::State::Error);
    f.s.on_create_room_outcome_ui_(false, "", "");
    f.s.on_create_room_outcome_ui_(true, "!r", "");
    CHECK_FALSE(ar->is_open());

    ar->open(AR::Tab::Join);
    f.s.on_create_room_outcome_ui_(true, "!r", ""); // wrong tab: ignored
    CHECK(ar->is_open());
}

// ── room list refresh ───────────────────────────────────────────────────────

TEST_CASE("refresh_room_list_ shows the root rooms, hiding space children",
          "[shell][rooms][list]")
{
    RaAppFx f;
    f.s.refresh_room_list_(); // fine without rooms
    f.s.rooms_ = {ra_room("!space", true), ra_room("!child"), ra_room("!loose")};
    f.s.space_children_cache_["!space"] = {"!child"};
    f.s.mark_room_index_dirty_();
    f.s.current_room_id_ = "!loose";
    f.s.refresh_room_list_();
    std::vector<std::string> ids;
    for (const auto& r : f.app->room_list_view()->rooms())
        ids.push_back(r.id);
    CHECK(std::find(ids.begin(), ids.end(), "!child") == ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "!loose") != ids.end());
    CHECK(f.app->room_list_view()->selected_room_id() == "!loose");
}

TEST_CASE("refresh_room_list_ inside a space lists only its children",
          "[shell][rooms][list]")
{
    RaAppFx f;
    f.s.rooms_ = {ra_room("!space", true), ra_room("!child"), ra_room("!loose")};
    f.s.space_children_cache_["!space"] = {"!child"};
    f.s.mark_room_index_dirty_();
    f.s.space_stack_ = {"!space"};
    f.s.refresh_room_list_();
    REQUIRE(f.app->room_list_view()->rooms().size() == 1);
    CHECK(f.app->room_list_view()->rooms()[0].id == "!child");

    f.s.space_back_command_();
    CHECK(f.s.space_stack_.empty());
    CHECK(f.app->room_list_view()->rooms().size() >= 2);
}

TEST_CASE("an active room search lists every room unfiltered",
          "[shell][rooms][list]")
{
    RaAppFx f;
    f.s.rooms_ = {ra_room("!space", true), ra_room("!child"), ra_room("!loose")};
    f.s.space_children_cache_["!space"] = {"!child"};
    f.s.mark_room_index_dirty_();
    f.s.room_search_text_ = "x";
    f.s.refresh_room_list_();
    CHECK(f.app->room_list_view()->rooms().size() == 3);
}

TEST_CASE("space_back_command_ re-asserts a space that is still the open room",
          "[shell][rooms][list]")
{
    RaAppFx f;
    f.s.rooms_ = {ra_room("!space", true)};
    f.s.mark_room_index_dirty_();
    f.s.current_room_id_ = "!space";
    f.s.space_stack_ = {"!space"};
    f.s.space_back_command_();
    CHECK(f.s.space_stack_.empty());
    CHECK(f.s.space_root_shown_id_ == "!space");
}

TEST_CASE("knock panel refresh pushes the list into the panel", 
          "[shell][rooms][knock]")
{
    RaAppFx f;
    tesseract::KnockRequestInfo k;
    k.user_id = "@u:x";
    f.s.current_room_knock_requests_ = {k};
    f.s.on_knock_requests_panel_updated_();
    SUCCEED();
    RaShell none;
    none.on_knock_requests_panel_updated_(); // no app: no-op
}
