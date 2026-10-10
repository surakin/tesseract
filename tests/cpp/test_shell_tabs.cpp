// ShellBase_navigation.cpp: tab strip bookkeeping (open / select / navigate /
// close / restore), what a room switch resets (media group, lazy-media set,
// recents, warm LRU, preview override), room-layout persistence, hidden-room
// lookup, window title composition and the default tab-bar rebuild.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/RoomHeader.h"
#include "app/RoomPane.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct TabShell : tesseract::test::TestShellBase
{
    ~TabShell() override
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
    void on_tab_state_changed_ui_() override
    {
        ++tab_state_changes;
        if (run_base_tab_state)
            ShellBase::on_tab_state_changed_ui_();
    }
    void apply_window_title_ui_(const std::string& t) override
    {
        titles.push_back(t);
    }
    void on_recent_room_visited_(const tesseract::RoomInfo& r) override
    {
        recent_visits.push_back(r.id);
    }
    void navigate_to_room_(const std::string& id) override { navigated = id; }

    std::vector<std::function<void()>> delayed;
    std::vector<std::string> titles, recent_visits;
    std::string navigated;
    int tab_state_changes = 0;
    bool run_base_tab_state = false;

    using ShellBase::account_data_dirty_;
    using ShellBase::active_account_;
    using ShellBase::active_media_group_;
    using ShellBase::active_tab_idx_;
    using ShellBase::after_active_room_changed_;
    using ShellBase::app_settings_open_;
    using ShellBase::client_;
    using ShellBase::compose_window_title_;
    using ShellBase::current_room_id_;
    using ShellBase::hidden_rooms_;
    using ShellBase::kNavHistoryMax;
    using ShellBase::kRecentRoomsMax;
    using ShellBase::main_app_;
    using ShellBase::main_room_pane_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::media_group_for_room_;
    using ShellBase::media_prepped_event_ids_;
    using ShellBase::on_pin_requested;
    using ShellBase::on_room_selected_;
    using ShellBase::on_unpin_requested;
    using ShellBase::open_room_version_;
    using ShellBase::pending_event_scroll_after_join_;
    using ShellBase::pending_preview_overrides_;
    using ShellBase::pending_restore_popouts_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::persist_room_layout_pref_;
    using ShellBase::recent_room_ids_;
    using ShellBase::refresh_window_title_;
    using ShellBase::room_nav_history_;
    using ShellBase::room_by_id_;
    using ShellBase::room_open_in_tab;
    using ShellBase::room_open_in_window;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::set_app_settings_open_;
    using ShellBase::space_stack_;
    using ShellBase::tab_close;
    using ShellBase::tab_navigate_room;
    using ShellBase::tab_open_room;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::touch_visited_room_;
    using ShellBase::try_restore_tab_session_;
    using ShellBase::update_call_btn_visibility_;
    using ShellBase::visited_lru_;
    using ShellBase::pagination_;
    using ShellBase::pool_;
    using ShellBase::mut_pool_;
    using ShellBase::my_user_id_;

    std::vector<std::string> tab_ids() const
    {
        std::vector<std::string> v;
        for (const auto& t : tabs_)
            v.push_back(t.room_id);
        return v;
    }
};

tesseract::RoomInfo tab_room(const std::string& id, bool space = false)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = "Name " + id;
    r.is_space = space;
    return r;
}

using TabIds = std::vector<std::string>;

} // namespace

TEST_CASE("tab_open_room bootstraps from the current room and appends",
          "[shell][tabs]")
{
    TabShell s;
    s.tab_open_room("");
    CHECK(s.tabs_.empty());

    s.current_room_id_ = "!a";
    s.tab_open_room("!b");
    CHECK(s.tab_ids() == TabIds{"!a", "!b"});
    CHECK(s.active_tab_idx_ == 1);
    CHECK(s.current_room_id_ == "!b");
    CHECK(s.tab_state_changes == 1);

    s.tab_open_room("!c");
    CHECK(s.tab_ids() == TabIds{"!a", "!b", "!c"});

    // Opening an already-open tab just activates it.
    s.tab_open_room("!a");
    CHECK(s.tab_ids() == TabIds{"!a", "!b", "!c"});
    CHECK(s.active_tab_idx_ == 0);
    CHECK(s.current_room_id_ == "!a");
}

TEST_CASE("tab_open_room from nothing creates a single tab", "[shell][tabs]")
{
    TabShell s;
    s.tab_open_room("!a");
    CHECK(s.tab_ids() == TabIds{"!a"});
    CHECK(s.current_room_id_ == "!a");
}

TEST_CASE("tab_select_room replaces the active tab and switches to existing ones",
          "[shell][tabs]")
{
    TabShell s;
    s.tab_select_room("");
    CHECK(s.tabs_.empty());

    s.tab_select_room("!a");
    CHECK(s.tab_ids() == TabIds{"!a"});
    s.tab_select_room("!b"); // replaces in place
    CHECK(s.tab_ids() == TabIds{"!b"});
    CHECK(s.current_room_id_ == "!b");

    s.tab_open_room("!c");
    CHECK(s.tab_ids() == TabIds{"!b", "!c"});
    const int changes = s.tab_state_changes;
    s.tab_select_room("!c"); // already active: nothing
    CHECK(s.tab_state_changes == changes);
    s.tab_select_room("!b");
    CHECK(s.active_tab_idx_ == 0);
    CHECK(s.current_room_id_ == "!b");
    s.tab_select_room("!z"); // replaces the active (first) tab
    CHECK(s.tab_ids() == TabIds{"!z", "!c"});
}

TEST_CASE("tab_navigate_room picks select or open by tab count",
          "[shell][tabs]")
{
    TabShell s;
    s.tab_navigate_room("");
    s.tab_navigate_room("!a"); // no tabs: select
    CHECK(s.tab_ids() == TabIds{"!a"});
    s.tab_navigate_room("!b"); // one tab: replaces it
    CHECK(s.tab_ids() == TabIds{"!b"});

    s.tab_open_room("!c");
    s.tab_navigate_room("!d"); // several tabs: opens a new one
    CHECK(s.tab_ids() == TabIds{"!b", "!c", "!d"});
    s.tab_navigate_room("!b"); // existing: activate
    CHECK(s.active_tab_idx_ == 0);
}

TEST_CASE("tab_close keeps a sensible active tab", "[shell][tabs]")
{
    TabShell s;
    s.tab_close("!nope");
    s.tab_select_room("!a");
    s.tab_open_room("!b");
    s.tab_open_room("!c");
    s.tab_open_room("!d");
    REQUIRE(s.tab_ids() == TabIds{"!a", "!b", "!c", "!d"});
    REQUIRE(s.active_tab_idx_ == 3);

    s.tab_close("!a"); // before the active one: index shifts
    CHECK(s.tab_ids() == TabIds{"!b", "!c", "!d"});
    CHECK(s.active_tab_idx_ == 2);
    CHECK(s.current_room_id_ == "!d");

    s.tab_close("!c"); // between: still on !d
    CHECK(s.active_tab_idx_ == 1);
    CHECK(s.current_room_id_ == "!d");

    s.tab_close("!d"); // closing the active tab falls back to its left neighbour
    CHECK(s.tab_ids() == TabIds{"!b"});
    CHECK(s.current_room_id_ == "!b");

    s.tab_close("!b"); // last tab: back to the empty state
    CHECK(s.tabs_.empty());
    CHECK(s.current_room_id_.empty());
    CHECK(s.active_tab_idx_ == 0);
}

TEST_CASE("closing the first active tab selects the new first", "[shell][tabs]")
{
    TabShell s;
    s.tab_select_room("!a");
    s.tab_open_room("!b");
    s.tab_select_room("!a");
    REQUIRE(s.active_tab_idx_ == 0);
    s.tab_close("!a");
    CHECK(s.tab_ids() == TabIds{"!b"});
    CHECK(s.active_tab_idx_ == 0);
    CHECK(s.current_room_id_ == "!b");
}

TEST_CASE("room_open_in_tab / room_open_in_window", "[shell][tabs]")
{
    TabShell s;
    s.tab_select_room("!a");
    CHECK(s.room_open_in_tab("!a"));
    CHECK_FALSE(s.room_open_in_tab("!b"));
    CHECK_FALSE(s.room_open_in_window("!a"));
}

TEST_CASE("try_restore_tab_session_ reopens only known non-space rooms",
          "[shell][tabs]")
{
    TabShell s;
    s.rooms_ = {tab_room("!a"), tab_room("!b"), tab_room("!s", true)};
    CHECK_FALSE(s.try_restore_tab_session_({"!gone", "!s"}, ""));
    CHECK(s.tabs_.empty());

    CHECK(s.try_restore_tab_session_({"!a", "!gone", "!s", "!b"}, "!b"));
    CHECK(s.tab_ids() == TabIds{"!a", "!b"});
    CHECK(s.active_tab_idx_ == 1);
    CHECK(s.current_room_id_ == "!b");

    CHECK(s.try_restore_tab_session_({"!a", "!b"}, "!unknown"));
    CHECK(s.active_tab_idx_ == 0);
    CHECK(s.try_restore_tab_session_({"!a", "!b"}, ""));
    CHECK(s.current_room_id_ == "!a");

    // Unknown pop-out rooms stay pending for the next rooms update.
    s.pending_restore_popouts_ = {"!notyet"};
    CHECK(s.try_restore_tab_session_({"!a"}, ""));
    CHECK(s.pending_restore_popouts_ == TabIds{"!notyet"});
}

TEST_CASE("a room switch resets per-room media state and records recents",
          "[shell][tabs][switch]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.rooms_ = {tab_room("!a"), tab_room("!b")};
    s.mark_room_index_dirty_();

    s.media_prepped_event_ids_ = {"$x"};
    s.current_room_id_ = "!a";
    s.after_active_room_changed_();
    CHECK(s.media_prepped_event_ids_.empty());
    CHECK(s.active_media_group_ == TabShell::media_group_for_room_("!a"));
    CHECK(s.recent_room_ids_ == TabIds{"!a"});
    CHECK(s.recent_visits == TabIds{"!a"});
    CHECK(s.visited_lru_.front() == "!a");
    CHECK(s.account_data_dirty_);
    CHECK(s.pending_preview_overrides_.size() == 1);

    s.current_room_id_ = "!b";
    s.after_active_room_changed_();
    CHECK(s.active_media_group_ == TabShell::media_group_for_room_("!b"));
    CHECK(s.recent_room_ids_ == TabIds{"!b", "!a"});

    s.current_room_id_ = "!a"; // revisit moves to the front without duplicating
    s.after_active_room_changed_();
    CHECK(s.recent_room_ids_ == TabIds{"!a", "!b"});
}

TEST_CASE("recent rooms are capped", "[shell][tabs][switch]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    for (std::size_t i = 0; i < TabShell::kRecentRoomsMax + 3; ++i)
    {
        s.current_room_id_ = "!r" + std::to_string(i);
        s.after_active_room_changed_();
    }
    CHECK(s.recent_room_ids_.size() == TabShell::kRecentRoomsMax);
    CHECK(s.recent_room_ids_.front() ==
          "!r" + std::to_string(TabShell::kRecentRoomsMax + 2));
}

TEST_CASE("navigation history is capped", "[shell][tabs][switch]")
{
    TabShell s;
    for (std::size_t i = 0; i < TabShell::kNavHistoryMax + 5; ++i)
    {
        s.current_room_id_ = "!r" + std::to_string(i);
        s.after_active_room_changed_();
    }
    CHECK(s.room_nav_history_.size() == TabShell::kNavHistoryMax);
    CHECK(s.room_nav_history_.front() == "!r5");
}

TEST_CASE("switching to a space shows its root instead of loading a timeline",
          "[shell][tabs][switch]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.rooms_ = {tab_room("!space", true)};
    s.mark_room_index_dirty_();
    s.current_room_id_ = "!space";
    s.after_active_room_changed_();
    // A space returns before the media-group / recents bookkeeping.
    CHECK(s.recent_room_ids_.empty());
    CHECK(s.active_media_group_ == 0);
}

TEST_CASE("closing the last tab still persists the empty layout",
          "[shell][tabs][switch]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.current_room_id_.clear();
    s.after_active_room_changed_();
    CHECK(s.account_data_dirty_);
    CHECK(s.recent_room_ids_.empty());
}

TEST_CASE("call button visibility needs server support, a non-bridged room and "
          "permission",
          "[shell][tabs]")
{
    tesseract::Client client; // sessionless: cannot start calls
    TabShell s;
    s.update_call_btn_visibility_(nullptr, "!r"); // null header: no-op

    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    auto* header = rv->header();
    REQUIRE(header != nullptr);
    s.client_ = &client;
    s.server_info_.supports_calls = true;
    s.update_call_btn_visibility_(header, "!r");
    CHECK_FALSE(header->show_call_btn()); // no permission, not in a call

    s.client_ = nullptr;
    s.server_info_.supports_calls = false;
    s.update_call_btn_visibility_(header, "!r");
    CHECK_FALSE(header->show_call_btn());
}

TEST_CASE("pin and unpin need a client and a room", "[shell][tabs]")
{
    TabShell s;
    s.on_pin_requested("$e");
    s.on_unpin_requested("$e");
    tesseract::Client client;
    s.client_ = &client;
    s.on_pin_requested("$e"); // no current room
    s.on_unpin_requested("");
    s.current_room_id_ = "!r";
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    s.on_pin_requested("");
    s.on_pin_requested("$e"); // fails (no session) and logs
    s.on_unpin_requested("$e");
    SUCCEED();
}

TEST_CASE("room layout persistence writes tabs, recents and bridge overrides",
          "[shell][tabs][persist]")
{
    tesseract::Client client;
    TabShell s;
    s.persist_room_layout_pref_(false); // no client: nothing
    s.persist_room_layout_pref_(true);

    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.tab_select_room("!a");
    s.tab_open_room("!b");
    s.recent_room_ids_ = {"!b", "!a"};
    s.account_data_dirty_ = true;

    s.persist_room_layout_pref_(false);
    CHECK_FALSE(s.account_data_dirty_);
    const std::string json = s.active_account_->prefs_json;
    CHECK(json.find("!a") != std::string::npos);
    CHECK(json.find("!b") != std::string::npos);
    CHECK(s.active_account_->recent_rooms == TabIds{"!b", "!a"});

    // Blocking save is skipped when nothing is dirty.
    s.active_account_->prefs_json.clear();
    s.persist_room_layout_pref_(true);
    CHECK(s.active_account_->prefs_json.empty());
    s.account_data_dirty_ = true;
    s.persist_room_layout_pref_(true);
    CHECK_FALSE(s.account_data_dirty_);
    CHECK_FALSE(s.active_account_->prefs_json.empty());
}

TEST_CASE("an unrestored saved layout is not overwritten by the empty tab strip",
          "[shell][tabs][persist]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.pending_restore_rooms_ = {"!saved1", "!saved2"};
    s.persist_room_layout_pref_(false);
    CHECK(s.active_account_->prefs_json.find("!saved1") != std::string::npos);
    CHECK(s.active_account_->prefs_json.find("!saved2") != std::string::npos);
}

TEST_CASE("room_by_id_ falls back to hidden rooms and tracks index changes",
          "[shell][tabs]")
{
    TabShell s;
    CHECK(s.room_by_id_("!a") == nullptr);
    s.rooms_ = {tab_room("!a")};
    s.mark_room_index_dirty_();
    REQUIRE(s.room_by_id_("!a") != nullptr);
    CHECK(s.room_by_id_("!a")->name == "Name !a");

    s.hidden_rooms_["!old"] = tab_room("!old");
    REQUIRE(s.room_by_id_("!old") != nullptr);
    CHECK(s.room_by_id_("!old")->id == "!old");

    s.rooms_.clear(); // stale index entry must not be dereferenced
    CHECK(s.room_by_id_("!a") == nullptr);
}

TEST_CASE("open_room_version_ reveals a listed room or defers to a join",
          "[shell][tabs][version]")
{
    TabShell s;
    s.open_room_version_("", {}, "", false);
    CHECK(s.tabs_.empty());

    s.rooms_ = {tab_room("!a")};
    s.mark_room_index_dirty_();
    s.open_room_version_("!a", {}, "", false);
    CHECK(s.tab_ids() == TabIds{"!a"});

    // Unknown room with no session: nothing to do.
    s.open_room_version_("!unknown", {}, "$e", true);
    CHECK(s.pending_event_scroll_after_join_.empty());

    // With a session the lookup is async; a failed lookup remembers the
    // event for the post-join scroll.
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    s.client_ = s.active_account_->client.get();
    s.open_room_version_("!unknown", {}, "$e", false);
    s.pool_.wait_idle(std::chrono::seconds(5));
    CHECK(s.pending_event_scroll_after_join_.at("!unknown") == "$e");
}

TEST_CASE("on_room_selected_ drills into spaces and opens ordinary rooms",
          "[shell][tabs][select]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.rooms_ = {tab_room("!space", true), tab_room("!a")};
    s.mark_room_index_dirty_();

    s.on_room_selected_("");
    CHECK(s.current_room_id_.empty());

    s.on_room_selected_("!space");
    CHECK(s.space_stack_ == TabIds{"!space"});
    CHECK(s.current_room_id_.empty());

    s.on_room_selected_("!a");
    CHECK(s.current_room_id_ == "!a");
    REQUIRE_FALSE(s.titles.empty());
    CHECK(s.titles.back() == "Tesseract - Name !a");
}

TEST_CASE("on_room_selected_ loads the room into the view and subscribes",
          "[shell][tabs][select]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    s.rooms_ = {tab_room("!a")};
    s.mark_room_index_dirty_();
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    s.room_view_ = rv.get();
    s.pagination_["!a"].initial_fill_done = true;
    s.on_room_selected_("!a");
    s.mut_pool_.wait_idle(std::chrono::seconds(5));
    CHECK(s.current_room_id_ == "!a");
    CHECK(rv->compose_bar() != nullptr);
}

TEST_CASE("window title follows the room and the settings page",
          "[shell][tabs][title]")
{
    TabShell s;
    CHECK(s.compose_window_title_() == "Tesseract");
    s.rooms_ = {tab_room("!a")};
    s.mark_room_index_dirty_();
    s.current_room_id_ = "!a";
    CHECK(s.compose_window_title_() == "Tesseract - Name !a");

    s.set_app_settings_open_(true);
    CHECK(s.compose_window_title_() == "Tesseract");
    CHECK(s.titles.back() == "Tesseract");
    const auto n = s.titles.size();
    s.set_app_settings_open_(true); // unchanged: no refresh
    CHECK(s.titles.size() == n);
    s.set_app_settings_open_(false);
    CHECK(s.titles.back() == "Tesseract - Name !a");

    tesseract::RoomInfo unnamed;
    unnamed.id = "!u";
    s.rooms_.push_back(unnamed);
    s.mark_room_index_dirty_();
    s.current_room_id_ = "!u";
    CHECK(s.compose_window_title_() == "Tesseract");
}

TEST_CASE("the default tab-state hook rebuilds the tab bar and selects the "
          "active room",
          "[shell][tabs][bar]")
{
    tesseract::Client client;
    TabShell s;
    s.run_base_tab_state = true;
    s.client_ = &client;
    s.rooms_ = {tab_room("!a"), tab_room("!b")};
    s.mark_room_index_dirty_();
    auto surface = TestSurface::create(900, 700);
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    s.main_app_ = app.get();

    s.tab_select_room("!a");
    CHECK(s.current_room_id_ == "!a");
    s.tab_open_room("!b");
    CHECK(s.current_room_id_ == "!b");
    CHECK(s.tabs_.size() == 2);
    CHECK_FALSE(s.titles.empty());
}

TEST_CASE("touch_visited_room_ keeps the most recent first and unique",
          "[shell][tabs]")
{
    TabShell s;
    s.touch_visited_room_("");
    CHECK(s.visited_lru_.empty());
    s.touch_visited_room_("!a");
    s.touch_visited_room_("!b");
    s.touch_visited_room_("!a");
    CHECK(s.visited_lru_ == TabIds{"!a", "!b"});
}

TEST_CASE("tab operations also reset the thread panel through the main pane",
          "[shell][tabs][pane]")
{
    tesseract::Client client;
    TabShell s;
    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    s.room_view_ = rv.get();
    s.main_room_pane_ = std::make_unique<tesseract::RoomPane>(
        tesseract::RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}},
        "");
    s.main_room_pane_->attach({.room_view = rv.get()});
    s.rooms_ = {tab_room("!a"), tab_room("!b"), tab_room("!c")};
    s.mark_room_index_dirty_();

    s.tab_select_room("!a");
    s.tab_open_room("!b");
    s.tab_open_room("!c");
    CHECK(s.main_room_pane_->room_id() == "!c");
    s.tab_open_room("!a"); // existing tab
    CHECK(s.main_room_pane_->room_id() == "!a");
    s.tab_select_room("!b");
    s.tab_navigate_room("!c");
    CHECK(s.main_room_pane_->room_id() == "!c");
    s.tab_close("!a");
    s.tab_close("!c");
    s.tab_close("!b"); // the last tab: empty state
    CHECK(s.tabs_.empty());
    CHECK(s.current_room_id_.empty());
    CHECK(s.try_restore_tab_session_({"!a", "!b"}, "!b"));
    CHECK(s.main_room_pane_->room_id() == "!b");
}
