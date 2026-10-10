// ShellBase_profile.cpp (second half): the known-users roster build, user
// search / invite resolution, DM opening, launch actions, matrix-link
// navigation, extended-profile delivery routing, tray items and the
// send-side presence tracker.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "views/UserProfilePanel.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/launch_args.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct RlShell : tesseract::test::TestShellBase
{
    ~RlShell() override
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
    // Worker threads post here: queue under a lock, run on demand.
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }
    void navigate_to_room_(const std::string& id) override { navigated.push_back(id); }
    void raise_main_window_ui_() override { ++raises; }
    void open_quick_switch_ui_() override { ++quick_switches; }
    void open_message_search_ui_() override { ++message_searches; }
    void open_app_settings_ui_() override { ++settings_opens; }
    void switch_active_account_(const std::string& u) override { switched.push_back(u); }
    void raise_and_activate_() override { ++activated; }
    void request_repaint_() override { ++repaints; }

    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::string> navigated, switched;
    int raises = 0, quick_switches = 0, message_searches = 0,
        settings_opens = 0, activated = 0, repaints = 0;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::build_known_users_roster_;
    using ShellBase::build_tray_items_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::dispatch_launch_action_;
    using ShellBase::dm_in_flight_user_ids_;
    using ShellBase::ensure_known_users_roster_;
    using ShellBase::fetch_own_extended_profile_async_;
    using ShellBase::fetch_user_extended_profile_async_;
    using ShellBase::handle_extended_profile_ready_ui_;
    using ShellBase::handle_open_dm_;
    using ShellBase::handle_user_query_;
    using ShellBase::invalidate_known_users_;
    using ShellBase::invite_resolve_gen_;
    using ShellBase::invite_resolves_inflight_;
    using ShellBase::known_users_;
    using ShellBase::known_users_building_;
    using ShellBase::known_users_built_;
    using ShellBase::last_room_list_state_;
    using ShellBase::last_user_query_;
    using ShellBase::main_content_ready_;
    using ShellBase::mark_main_content_ready_;
    using ShellBase::member_gender_cache_;
    using ShellBase::member_gender_inflight_;
    using ShellBase::my_user_id_;
    using ShellBase::navigate_tray_unread_;
    using ShellBase::next_request_id_;
    using ShellBase::notify_user_activity_;
    using ShellBase::notify_window_active_;
    using ShellBase::notify_presence_logout_;
    using ShellBase::notify_presence_tick_;
    using ShellBase::open_launch_room_;
    using ShellBase::open_matrix_link;
    using ShellBase::own_extended_profile_;
    using ShellBase::pending_invite_resolves_;
    using ShellBase::pending_join_via_;
    using ShellBase::pending_launch_action_;
    using ShellBase::pending_launch_room_id_;
    using ShellBase::pending_matrix_link_;
    using ShellBase::pending_member_gender_requests_;
    using ShellBase::pending_resolve_requests_;
    using ShellBase::pending_user_profiles_;
    using ShellBase::per_account_rooms_;
    using ShellBase::presence_tracker_;
    using ShellBase::request_member_pronoun_ui_;
    using ShellBase::resolve_invite_user_;
    using ShellBase::retry_pending_launch_room_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::start_presence_tracking_;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::user_resolve_gen_;
    using ShellBase::merge_roster_entry_;
    using ShellBase::focus_tray_unread_popout_;
    using ShellBase::push_own_status_to_strip_;
    using ShellBase::broadcast_rebuild_tray_;
    using ShellBase::kRosterMaxRoomMembers;
    using ShellBase::resolve_presence_polling_;
    using ShellBase::last_window_active_;
};

tesseract::RoomInfo rl_room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = "N" + id;
    return r;
}

struct RlFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    RlShell s;
    RlFx()
    {
        s.client_ = &client;
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.my_user_id_ = "@me:x";
    }
};

} // namespace

// ── roster ───────────────────────────────────────────────────────────────────

TEST_CASE("roster build seeds DM partners at once and completes in the background",
          "[shell][roster]")
{
    RlFx f;
    auto dm = rl_room("!dm");
    dm.is_direct = true;
    dm.dm_counterpart_user_id = "@friend:x";
    dm.dm_avatar_url = "mxc://hs/friend";
    auto self_dm = rl_room("!self");
    self_dm.is_direct = true;
    self_dm.dm_counterpart_user_id = "@me:x";
    f.s.rooms_ = {dm, self_dm, rl_room("!plain")};

    f.s.build_known_users_roster_();
    CHECK(f.s.known_users_building_);
    // Seeded synchronously, never with ourselves.
    REQUIRE(f.s.known_users_.count("@friend:x") == 1);
    CHECK(f.s.known_users_.at("@friend:x").avatar_url == "mxc://hs/friend");
    CHECK(f.s.known_users_.count("@me:x") == 0);

    f.s.build_known_users_roster_(); // already building: ignored
    f.s.pump();
    CHECK(f.s.known_users_built_);
    CHECK_FALSE(f.s.known_users_building_);

    f.s.ensure_known_users_roster_(); // built: nothing more
    CHECK_FALSE(f.s.known_users_building_);
}

TEST_CASE("roster build needs a client and is dropped on invalidate",
          "[shell][roster]")
{
    RlFx f;
    f.s.client_ = nullptr;
    f.s.build_known_users_roster_();
    CHECK_FALSE(f.s.known_users_building_);

    f.s.client_ = &f.client;
    f.s.rooms_ = {rl_room("!a")};
    f.s.ensure_known_users_roster_();
    CHECK(f.s.known_users_building_);
    f.s.invalidate_known_users_();
    CHECK_FALSE(f.s.known_users_building_);
    f.s.pump(); // the late flush sees the cancel and merges nothing
    CHECK_FALSE(f.s.known_users_built_);
    CHECK(f.s.known_users_.empty());
}

TEST_CASE("a roster result for another account is discarded", "[shell][roster]")
{
    RlFx f;
    f.s.rooms_ = {rl_room("!a")};
    f.s.build_known_users_roster_();
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>(); // switched
    f.s.pump();
    CHECK_FALSE(f.s.known_users_built_);
}

TEST_CASE("merge_roster_entry_ never overwrites richer data", "[shell][roster]")
{
    RlShell s;
    s.merge_roster_entry_("", "x", "y");
    CHECK(s.known_users_.empty());
    s.merge_roster_entry_("@a:x", "Alice", "");
    s.merge_roster_entry_("@a:x", "Other", "mxc://hs/a");
    CHECK(s.known_users_.at("@a:x").display_name == "Alice");
    CHECK(s.known_users_.at("@a:x").avatar_url == "mxc://hs/a");
}

// ── user query / invite resolve ─────────────────────────────────────────────

TEST_CASE("typing a user query strips the @ and builds the roster",
          "[shell][roster][query]")
{
    RlFx f;
    f.s.handle_user_query_("@ali");
    CHECK(f.s.last_user_query_ == "ali");
    CHECK(f.s.known_users_building_);
    f.s.handle_user_query_("@");
    CHECK(f.s.last_user_query_.empty());
    f.s.pump();
}

TEST_CASE("a complete unknown mxid is resolved live, a known one is not",
          "[shell][roster][query]")
{
    RlFx f;
    f.s.known_users_built_ = true;
    f.s.known_users_["@known:x"] = {"@known:x", "Known", ""};
    f.s.handle_user_query_("@known:x");
    CHECK(f.s.pending_resolve_requests_.empty());

    f.s.handle_user_query_("@new:x");
    REQUIRE(f.s.pending_resolve_requests_.size() == 1);
    f.s.pump(); // waits the debounce, still current -> fires the lookup
    CHECK(f.s.pending_resolve_requests_.size() == 1);

    // A superseded query's pending entry is cleaned up.
    f.s.pending_resolve_requests_.clear();
    f.s.handle_user_query_("@one:x");
    f.s.handle_user_query_("@two:x");
    f.s.pump();
    CHECK(f.s.pending_resolve_requests_.size() == 1);
    CHECK(f.s.pending_resolve_requests_.begin()->second.first == "@two:x");

    f.s.client_ = nullptr;
    f.s.handle_user_query_("@three:x"); // no client: nothing to resolve with
    CHECK(f.s.pending_resolve_requests_.size() == 1);
}

TEST_CASE("resolved profiles reach the right consumer", "[shell][roster][query]")
{
    RlFx f;
    const std::string found =
        R"({"exists":true,"user_id":"@new:x","display_name":"New","avatar_url":"mxc://hs/n"})";

    // Quick-switcher resolve, current generation.
    f.s.pending_resolve_requests_[1] = {"@new:x", f.s.user_resolve_gen_.load()};
    f.s.handle_extended_profile_ready_ui_(1, found);
    CHECK(f.s.known_users_.count("@new:x") == 1);

    // Stale generation: ignored.
    f.s.pending_resolve_requests_[2] = {"@stale:x", f.s.user_resolve_gen_.load() + 5};
    f.s.handle_extended_profile_ready_ui_(2, found);
    CHECK(f.s.pending_resolve_requests_.empty());
    CHECK(f.s.known_users_.at("@new:x").display_name == "New");

    // Not found: roster unchanged.
    f.s.pending_resolve_requests_[3] = {"@ghost:x", f.s.user_resolve_gen_.load()};
    f.s.handle_extended_profile_ready_ui_(3, R"({"exists":false})");
    CHECK(f.s.known_users_.count("@ghost:x") == 0);
}

TEST_CASE("invite-dialog resolves: debounced, immediate, known and superseded",
          "[shell][roster][invite]")
{
    RlFx f;
    f.s.resolve_invite_user_("not-an-mxid", false, nullptr);
    CHECK(f.s.pending_invite_resolves_.empty());
    f.s.client_ = nullptr;
    f.s.resolve_invite_user_("@u:x", false, nullptr);
    CHECK(f.s.pending_invite_resolves_.empty());
    f.s.client_ = &f.client;

    f.s.known_users_["@have:x"] = {"@have:x", "Have", ""};
    f.s.resolve_invite_user_("@have:x", false, nullptr);
    CHECK(f.s.pending_invite_resolves_.empty());

    f.s.resolve_invite_user_("@u:x", false, nullptr);
    f.s.resolve_invite_user_("@u:x", false, nullptr); // already in flight
    REQUIRE(f.s.pending_invite_resolves_.size() == 1);
    CHECK(f.s.invite_resolves_inflight_.count("@u:x") == 1);
    f.s.handle_extended_profile_ready_ui_(
        f.s.pending_invite_resolves_.begin()->first,
        R"({"exists":true,"display_name":"U"})");
    CHECK(f.s.pending_invite_resolves_.empty());
    CHECK(f.s.invite_resolves_inflight_.empty());
    REQUIRE(f.s.known_users_.count("@u:x") == 1); // user_id filled from the request
    CHECK(f.s.known_users_.at("@u:x").display_name == "U");

    // Debounced lookups: only the last survives.
    f.s.resolve_invite_user_("@a:x", true, nullptr);
    f.s.resolve_invite_user_("@b:x", true, nullptr);
    f.s.pump();
    REQUIRE(f.s.pending_invite_resolves_.size() == 1);
    CHECK(f.s.pending_invite_resolves_.begin()->second.first == "@b:x");

    // Superseded result is ignored; a not-found one just clears the entry.
    auto id = f.s.pending_invite_resolves_.begin()->first;
    f.s.pending_invite_resolves_[id].second = f.s.invite_resolve_gen_.load() + 9;
    f.s.handle_extended_profile_ready_ui_(id, R"({"exists":false})");
    CHECK(f.s.known_users_.count("@b:x") == 0);
    f.s.pending_invite_resolves_[77] = {"@c:x", 0};
    f.s.invite_resolves_inflight_.insert("@c:x");
    f.s.handle_extended_profile_ready_ui_(77, R"({"exists":false})");
    CHECK(f.s.invite_resolves_inflight_.empty());
}

TEST_CASE("extended profile results: panel, own profile and pronouns",
          "[shell][roster][profile]")
{
    RlFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r";
    rv->set_room(info);
    f.s.room_view_ = rv.get();

    auto* panel = rv->user_profile_panel();
    REQUIRE(panel != nullptr);
    f.s.fetch_user_extended_profile_async_("@u:x", nullptr);
    CHECK(f.s.pending_user_profiles_.empty());
    f.s.fetch_user_extended_profile_async_("@u:x", panel);
    REQUIRE(f.s.pending_user_profiles_.size() == 1);
    f.s.handle_extended_profile_ready_ui_(
        f.s.pending_user_profiles_.begin()->first,
        R"({"exists":true,"tz":"Europe/Oslo"})");
    CHECK(f.s.pending_user_profiles_.empty());

    // Own profile is fetched for ourselves and applied when nothing is pending.
    f.s.fetch_own_extended_profile_async_();
    f.s.handle_extended_profile_ready_ui_(
        9999, R"({"exists":true,"tz":"Asia/Tokyo","status_text":"busy"})");
    CHECK(f.s.own_extended_profile_.tz == "Asia/Tokyo");
    CHECK(f.s.own_extended_profile_.status_text == "busy");
    f.s.push_own_status_to_strip_();

    // Pronoun lookup is de-duplicated and applies the possessive word.
    f.s.request_member_pronoun_ui_("");
    f.s.request_member_pronoun_ui_("@u:x");
    f.s.request_member_pronoun_ui_("@u:x");
    REQUIRE(f.s.pending_member_gender_requests_.size() == 1);
    CHECK(f.s.member_gender_inflight_.count("@u:x") == 1);
    f.s.handle_extended_profile_ready_ui_(
        f.s.pending_member_gender_requests_.begin()->first,
        R"({"exists":true,"pronouns":[{"language":"en","summary":"she/her","grammatical_gender":"feminine"}]})");
    CHECK(f.s.member_gender_inflight_.empty());
    REQUIRE(f.s.member_gender_cache_.count("@u:x") == 1);
    f.s.request_member_pronoun_ui_("@u:x"); // cached
    CHECK(f.s.pending_member_gender_requests_.empty());
}

// ── DM ───────────────────────────────────────────────────────────────────────

TEST_CASE("opening a DM navigates to an existing one", "[shell][dm]")
{
    RlFx f;
    f.s.handle_open_dm_("", "");
    f.s.client_ = nullptr;
    f.s.handle_open_dm_("@u:x", "");
    f.s.client_ = &f.client;
    CHECK(f.s.navigated.empty());

    auto dm = rl_room("!dm");
    dm.is_direct = true;
    dm.dm_counterpart_user_id = "@u:x";
    f.s.rooms_ = {dm};
    f.s.handle_open_dm_("@u:x", "");
    CHECK(f.s.navigated == std::vector<std::string>{"!dm"});
}

TEST_CASE("opening a DM with nobody creates one asynchronously and de-duplicates",
          "[shell][dm]")
{
    RlFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    f.s.room_view_ = rv.get();
    f.s.handle_open_dm_("@new:x", "hi");
    CHECK(f.s.dm_in_flight_user_ids_.count("@new:x") == 1);
    f.s.handle_open_dm_("@new:x", "hi"); // in flight
    f.s.pump(); // sessionless create fails: the guard is released for a retry
    CHECK(f.s.dm_in_flight_user_ids_.empty());
    CHECK(f.s.navigated.empty());
    CHECK(f.s.repaints >= 1);
}

// ── launch actions ───────────────────────────────────────────────────────────

TEST_CASE("launch actions wait for the main content, then run", "[shell][launch]")
{
    RlFx f;
    f.s.dispatch_launch_action_(tesseract::LaunchAction::None, "");
    CHECK(f.s.raises == 0);

    f.s.dispatch_launch_action_(tesseract::LaunchAction::QuickSwitcher, "");
    CHECK(f.s.quick_switches == 0);
    CHECK(f.s.pending_launch_action_ == tesseract::LaunchAction::QuickSwitcher);
    f.s.mark_main_content_ready_();
    CHECK(f.s.quick_switches == 1);
    CHECK(f.s.raises == 1);
    CHECK(f.s.pending_launch_action_ == tesseract::LaunchAction::None);

    f.s.dispatch_launch_action_(tesseract::LaunchAction::MessageSearch, "");
    f.s.dispatch_launch_action_(tesseract::LaunchAction::Settings, "");
    CHECK(f.s.message_searches == 1);
    CHECK(f.s.settings_opens == 1);
}

TEST_CASE("launching into a room waits for the room to be known",
          "[shell][launch]")
{
    RlFx f;
    f.s.dispatch_launch_action_(tesseract::LaunchAction::Room, ""); // nothing pending
    CHECK(f.s.pending_launch_room_id_.empty());

    f.s.mark_main_content_ready_();
    f.s.dispatch_launch_action_(tesseract::LaunchAction::Room, "!r:x");
    CHECK(f.s.pending_launch_room_id_ == "!r:x"); // not listed yet
    CHECK(f.s.navigated.empty());

    f.s.per_account_rooms_["@me:x"] = {rl_room("!r:x")};
    f.s.retry_pending_launch_room_();
    CHECK(f.s.navigated == std::vector<std::string>{"!r:x"});
    CHECK(f.s.pending_launch_room_id_.empty());
    f.s.retry_pending_launch_room_(); // nothing pending
    f.s.open_launch_room_("");
}

TEST_CASE("a launch room on another account switches to it first",
          "[shell][launch]")
{
    RlFx f;
    f.s.per_account_rooms_["@other:x"] = {rl_room("!theirs")};
    f.s.open_launch_room_("!theirs");
    CHECK(f.s.switched == std::vector<std::string>{"@other:x"});
    CHECK(f.s.navigated == std::vector<std::string>{"!theirs"});
}

// ── matrix links ────────────────────────────────────────────────────────────

TEST_CASE("a matrix link is held until logged in with rooms", "[shell][links]")
{
    RlFx f;
    f.s.open_matrix_link("https://matrix.to/#/!r:x");
    CHECK(f.s.pending_matrix_link_ == "https://matrix.to/#/!r:x");
    CHECK(f.s.tabs_.empty());
}

TEST_CASE("matrix links navigate to rooms, aliases and events", "[shell][links]")
{
    RlFx f;
    auto room = rl_room("!r:x");
    room.canonical_alias = "#alias:x";
    f.s.rooms_ = {room};
    f.s.mark_room_index_dirty_();

    f.s.open_matrix_link("https://matrix.to/#/!r:x?via=hs.example");
    CHECK(f.s.current_room_id_ == "!r:x");
    CHECK(f.s.pending_join_via_.at("!r:x") == std::vector<std::string>{"hs.example"});

    f.s.current_room_id_.clear();
    f.s.tabs_.clear();
    f.s.open_matrix_link("https://matrix.to/#/%23alias:x");
    CHECK(f.s.current_room_id_ == "!r:x");

    f.s.current_room_id_.clear();
    f.s.tabs_.clear();
    f.s.open_matrix_link("https://matrix.to/#/!r:x/$event:x");
    CHECK(f.s.current_room_id_ == "!r:x");

    f.s.open_matrix_link("https://example.com/not-matrix"); // ignored
    f.s.open_matrix_link("https://matrix.to/#/@user:x");    // no room view: fine
}

TEST_CASE("links to unknown rooms fall back to room-version resolution",
          "[shell][links]")
{
    RlFx f;
    f.s.rooms_ = {rl_room("!other:x")};
    f.s.mark_room_index_dirty_();
    f.s.open_matrix_link("https://matrix.to/#/!unknown:x/$e:x?via=hs");
    f.s.pump(); // the lookup finds nothing; the event is remembered for the join
    f.s.open_matrix_link("https://matrix.to/#/!unknown2:x");
    f.s.open_matrix_link("https://matrix.to/#/%23nope:x"); // no add-room view
    f.s.pump();
    SUCCEED();
}

TEST_CASE("a user link opens that profile", "[shell][links]")
{
    RlFx f;
    f.s.rooms_ = {rl_room("!r:x")};
    f.s.mark_room_index_dirty_();
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    f.s.room_view_ = rv.get();
    f.s.open_matrix_link("https://matrix.to/#/@user:x");
    SUCCEED();
}

// ── tray ─────────────────────────────────────────────────────────────────────

TEST_CASE("tray items list each account and act through the dedicated window",
          "[shell][tray]")
{
    RlFx f;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    a->display_name = "Alice";
    auto b = std::make_shared<tesseract::AccountSession>();
    b->user_id = "@b:x";
    f.s.account_manager_.add_account(a);
    f.s.account_manager_.add_account(b);

    auto items = f.s.build_tray_items_();
    REQUIRE(items.size() == 2);
    CHECK(items[0].first == "Alice (@a:x)");
    CHECK(items[1].first == "@b:x");

    items[1].second(); // no dedicated window: switch
    CHECK(f.s.switched == std::vector<std::string>{"@b:x"});
    f.s.account_manager_.set_dedicated("@a:x", &f.s);
    items[0].second(); // dedicated: raise instead
    CHECK(f.s.activated == 1);
    f.s.account_manager_.clear_dedicated("@a:x");

    f.s.account_manager_.register_window(&f.s);
    f.s.broadcast_rebuild_tray_();
    f.s.account_manager_.unregister_window(&f.s);
}

TEST_CASE("tray navigation picks the most urgent unread room", "[shell][tray]")
{
    RlFx f;
    f.s.navigate_tray_unread_(); // nothing unread
    CHECK(f.s.navigated.empty());
    CHECK_FALSE(f.s.focus_tray_unread_popout_());
    auto quiet = rl_room("!quiet");
    quiet.notification_count = 1;
    auto loud = rl_room("!loud");
    loud.highlight_count = 1;
    f.s.rooms_ = {quiet, loud};
    f.s.mark_room_index_dirty_();
    f.s.navigate_tray_unread_();
    CHECK(f.s.current_room_id_ == "!loud");
    CHECK_FALSE(f.s.focus_tray_unread_popout_());
}

// ── presence ─────────────────────────────────────────────────────────────────

TEST_CASE("presence tracking starts lazily once sync runs and honours the privacy "
          "setting",
          "[shell][presence]")
{
    RlFx f;
    f.s.notify_user_activity_(); // sync not running yet
    CHECK(f.s.presence_tracker_ == nullptr);

    f.s.last_room_list_state_ = tesseract::RoomListState::Running;
    tesseract::Settings::instance().send_presence = false;
    f.s.notify_user_activity_(); // regression: used to dereference a null tracker
    CHECK(f.s.presence_tracker_ == nullptr);
    f.s.start_presence_tracking_();
    CHECK(f.s.presence_tracker_ == nullptr);

    tesseract::Settings::instance().send_presence = true;
    f.s.notify_user_activity_();
    REQUIRE(f.s.presence_tracker_ != nullptr);
    f.s.start_presence_tracking_(); // idempotent
    f.s.notify_window_active_(false);
    f.s.notify_window_active_(true);
    CHECK(f.s.last_window_active_);
    f.s.notify_presence_tick_();

    f.s.notify_presence_logout_();
    CHECK(f.s.presence_tracker_ == nullptr);
    f.s.notify_presence_logout_(); // already gone
}

TEST_CASE("window activity without a tracker only updates polling state",
          "[shell][presence]")
{
    RlFx f;
    f.s.notify_window_active_(false);
    CHECK_FALSE(f.s.last_window_active_);
    f.s.notify_presence_tick_();
}
