// ShellBase_windows.cpp: pop-out window lifecycle (open, reuse, close,
// settings bookkeeping), subscription ref-counting, and the secondary-window
// fan-out of timeline / media / room-info events, using a minimal RoomWindowBase
// that owns a real RoomView and RoomPane.

#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::RoomWindowBase;
using tesseract::ShellBase;
using tesseract::views::MessageRowData;

namespace
{

struct PoWindow : RoomWindowBase
{
    PoWindow(ShellBase* shell, std::string room_id)
        : RoomWindowBase(shell, room_id)
    {
        view = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
        tesseract::RoomInfo info;
        info.id = room_id;
        info.name = "Pop " + room_id;
        view->set_room(info);
        room_view_ = view.get();
        init_pane_(nullptr);
        pane_->attach({.room_view = view.get()});
        finish_init_();
    }
    void bring_to_front() override { ++fronted; }
    void close_window() override {}
    void request_relayout() override { ++relayouts; }
    void apply_theme(const tk::Theme&) override { ++themes; }
    void apply_scale_change(float) override {}
    bool is_visible() const override { return visible; }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> v;
        for (const auto& m : view->message_list()->messages())
            v.push_back(m.event_id);
        return v;
    }

    std::unique_ptr<tesseract::views::RoomView> view;
    int fronted = 0, relayouts = 0, themes = 0;
    bool visible = true;

protected:
    void surface_repaint_() override { ++repaints; }
    int repaints = 0;
};

struct PoShell : tesseract::test::TestShellBase
{
    ~PoShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        // Windows reference this shell: tear them down first.
        owned_secondary_windows_.clear();
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
    void prep_row_media_(const tesseract::Event&, bool) override {}
    RoomWindowBase* create_secondary_room_window_(const std::string& id) override
    {
        if (!create_windows)
            return nullptr;
        return new PoWindow(this, id);
    }
    void repaint_pickers_() override { ++picker_repaints; }
    bool is_main_window_visible_() const override { return main_visible; }

    std::vector<std::function<void()>> delayed;
    bool create_windows = true;
    bool main_visible = true;
    int picker_repaints = 0;

    PoWindow* window(const std::string& room, const std::string& uid = "")
    {
        for (auto& w : owned_secondary_windows_)
            if (w->room_id() == room &&
                (uid.empty() || w->owner_user_id() == uid))
                return static_cast<PoWindow*>(w.get());
        return nullptr;
    }

    using ShellBase::active_account_;
    using ShellBase::acquire_room_subscription_;
    using ShellBase::active_account_popouts_;
    using ShellBase::active_popout_media_groups_;
    using ShellBase::any_window_visible_;
    using ShellBase::client_;
    using ShellBase::close_all_popouts_;
    using ShellBase::close_popouts_for_account_;
    using ShellBase::current_room_id_;
    using ShellBase::current_theme_;
    using ShellBase::EventAccountScope;
    using ShellBase::focus_secondary_window_;
    using ShellBase::for_each_room_view_;
    using ShellBase::handle_message_inserted_ui_;
    using ShellBase::handle_message_removed_ui_;
    using ShellBase::handle_message_updated_ui_;
    using ShellBase::handle_messages_appended_ui_;
    using ShellBase::handle_messages_prepended_ui_;
    using ShellBase::handle_messages_updated_batch_ui_;
    using ShellBase::handle_timeline_reset_ui_;
    using ShellBase::my_user_id_;
    using ShellBase::notify_secondary_media_ready_;
    using ShellBase::open_room_in_new_window;
    using ShellBase::other_account_pagination_;
    using ShellBase::owned_secondary_windows_;
    using ShellBase::per_account_rooms_;
    using ShellBase::reply_pane_for_;
    using ShellBase::release_room_subscription_;
    using ShellBase::room_pinned_by_popout_;
    using ShellBase::room_subscription_refs_;
    using ShellBase::room_view_for_room_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::secondary_windows_;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::update_secondary_room_infos_;
    using ShellBase::update_video_playback_suspension_;
    using ShellBase::video_playback_suspended_;
    using ShellBase::dispatch_gif_to_secondary_windows_;
    using ShellBase::dispatch_gif_failed_to_secondary_windows_;
    using ShellBase::cached_gif_source_bytes_;
    using ShellBase::pending_popout_owner_;
    using ShellBase::pending_restore_popouts_;
    using ShellBase::build_rows_;
};

std::shared_ptr<tesseract::AccountSession> po_acct(const std::string& uid)
{
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = uid;
    a->client = std::make_unique<tesseract::Client>();
    return a;
}

struct PoFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    PoShell s;
    PoFx()
    {
        tesseract::Settings::instance().popout_windows.clear();
        s.client_ = &client;
        s.active_account_ = po_acct("@me:x");
        s.my_user_id_ = "@me:x";
        s.current_room_id_ = "!main";
    }
};

std::unique_ptr<tesseract::Event> po_ev(const std::string& id,
                                        const std::string& reply_to = "")
{
    auto e = std::make_unique<tesseract::Event>();
    e->event_id = id;
    e->type = tesseract::EventType::Text;
    e->body = "body " + id;
    e->in_reply_to_id = reply_to;
    return e;
}

tesseract::EventList po_list(std::initializer_list<const char*> ids)
{
    tesseract::EventList l;
    for (auto* i : ids)
        l.push_back(po_ev(i));
    return l;
}

} // namespace

TEST_CASE("opening a pop-out registers it, themes it and records it in settings",
          "[shell][popout]")
{
    PoFx f;
    f.s.open_room_in_new_window("");
    CHECK(f.s.owned_secondary_windows_.empty());

    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    REQUIRE(w != nullptr);
    CHECK(w->owner_user_id() == "@me:x");
    CHECK(w->themes == 1);
    CHECK(f.s.secondary_windows_.count("!a:x") == 1);
    CHECK(f.s.active_popout_media_groups_.size() == 1);
    REQUIRE(tesseract::Settings::instance().popout_windows.size() == 1);
    CHECK(tesseract::Settings::instance().popout_windows[0].room_id == "!a:x");
    CHECK(tesseract::Settings::instance().popout_windows[0].user_id == "@me:x");
    CHECK(f.s.room_subscription_refs_.size() == 1);
    CHECK(f.s.room_pinned_by_popout_("!a:x"));
    CHECK_FALSE(f.s.room_pinned_by_popout_("!b:x"));
}

TEST_CASE("opening an already-open room raises its window instead of duplicating",
          "[shell][popout]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    f.s.open_room_in_new_window("!a:x");
    CHECK(f.s.owned_secondary_windows_.size() == 1);
    CHECK(w->fronted == 1);
    CHECK(f.s.focus_secondary_window_("!a:x"));
    CHECK(w->fronted == 2);
    CHECK_FALSE(f.s.focus_secondary_window_("!nope:x"));
    CHECK(tesseract::Settings::instance().popout_windows.size() == 1);
}

TEST_CASE("popping out a room that is open in a tab closes the tab first",
          "[shell][popout]")
{
    PoFx f;
    f.s.tab_select_room("!a:x");
    f.s.tabs_.push_back({"!b:x", 0.f});
    f.s.open_room_in_new_window("!b:x");
    CHECK(f.s.tabs_.size() == 1);
    CHECK(f.s.tabs_[0].room_id == "!a:x");
}

TEST_CASE("a pre-migration settings entry is claimed rather than duplicated",
          "[shell][popout]")
{
    PoFx f;
    tesseract::Settings::PopoutEntry old;
    old.room_id = "!a:x";
    tesseract::Settings::instance().popout_windows.push_back(old);
    f.s.open_room_in_new_window("!a:x");
    REQUIRE(tesseract::Settings::instance().popout_windows.size() == 1);
    CHECK(tesseract::Settings::instance().popout_windows[0].user_id == "@me:x");
}

TEST_CASE("a shell that cannot create windows leaves no trace", "[shell][popout]")
{
    PoFx f;
    f.s.create_windows = false;
    f.s.open_room_in_new_window("!a:x");
    CHECK(f.s.owned_secondary_windows_.empty());
    CHECK(tesseract::Settings::instance().popout_windows.empty());
}

TEST_CASE("pop-outs of two accounts for the same room coexist", "[shell][popout]")
{
    PoFx f;
    auto other = po_acct("@other:x");
    f.s.open_room_in_new_window("!a:x");
    f.s.open_room_in_new_window("!a:x", other);
    CHECK(f.s.owned_secondary_windows_.size() == 2);
    CHECK(f.s.window("!a:x", "@other:x") != nullptr);
    CHECK(f.s.active_account_popouts_().size() == 1);
    CHECK(f.s.room_subscription_refs_.size() == 2);

    // Closing one keeps the room's media group for the other.
    f.s.close_popouts_for_account_("@other:x");
    CHECK(f.s.owned_secondary_windows_.size() == 1);
    CHECK(f.s.active_popout_media_groups_.size() == 1);
    CHECK(f.s.room_subscription_refs_.size() == 1);
}

TEST_CASE("closing all pop-outs forgets them in settings", "[shell][popout]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    f.s.open_room_in_new_window("!b:x");
    f.s.pending_restore_popouts_ = {"!c:x"};
    CHECK(tesseract::Settings::instance().popout_windows.size() == 2);
    f.s.close_all_popouts_();
    CHECK(f.s.owned_secondary_windows_.empty());
    CHECK(f.s.secondary_windows_.empty());
    CHECK(f.s.pending_restore_popouts_.empty());
    CHECK(tesseract::Settings::instance().popout_windows.empty());
    CHECK(f.s.room_subscription_refs_.empty());
}

TEST_CASE("subscription refs count windows and release at zero", "[shell][popout]")
{
    PoFx f;
    auto sess = f.s.active_account_;
    f.s.acquire_room_subscription_(nullptr, "!r");
    f.s.release_room_subscription_(nullptr, "!r");
    f.s.release_room_subscription_(sess, "!never"); // unknown: no-op

    f.s.acquire_room_subscription_(sess, "!r");
    f.s.acquire_room_subscription_(sess, "!r");
    CHECK(f.s.room_subscription_refs_.size() == 1);
    f.s.release_room_subscription_(sess, "!r");
    CHECK(f.s.room_subscription_refs_.size() == 1);
    f.s.release_room_subscription_(sess, "!r");
    CHECK(f.s.room_subscription_refs_.empty());

    // The main window already holds the active room: no extra subscribe, and
    // a non-active account's parked scroll state goes with its last ref.
    f.s.current_room_id_ = "!r";
    f.s.acquire_room_subscription_(sess, "!r");
    f.s.release_room_subscription_(sess, "!r");
    auto other = po_acct("@other:x");
    f.s.other_account_pagination_["@other:x"]["!r"].reached_start = true;
    f.s.acquire_room_subscription_(other, "!r");
    f.s.release_room_subscription_(other, "!r");
    CHECK(f.s.other_account_pagination_["@other:x"].count("!r") == 0);
}

TEST_CASE("a timeline reset for a pop-out-only room fills just the pop-out",
          "[shell][popout][dispatch]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    f.s.handle_timeline_reset_ui_("!a:x", po_list({"$1", "$2"}));
    CHECK(w->ids() == std::vector<std::string>{"$1", "$2"});
}

TEST_CASE("live edits fan out to the pop-out", "[shell][popout][dispatch]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    f.s.handle_timeline_reset_ui_("!a:x", po_list({"$1", "$3"}));
    f.s.handle_message_inserted_ui_("!a:x", 1, po_ev("$2"));
    CHECK(w->ids() == std::vector<std::string>{"$1", "$2", "$3"});

    auto edited = po_ev("$2");
    edited->body = "edited";
    f.s.handle_message_updated_ui_("!a:x", 1, std::move(edited));
    CHECK(w->view->message_list()->messages()[1].body == "edited");

    f.s.handle_message_removed_ui_("!a:x", 0);
    CHECK(w->ids() == std::vector<std::string>{"$2", "$3"});

    f.s.handle_messages_appended_ui_("!a:x", po_list({"$4"}));
    CHECK(w->ids().back() == "$4");
    f.s.handle_messages_prepended_ui_("!a:x", po_list({"$0"}));
    CHECK(w->ids().front() == "$0");

    tesseract::EventList batch;
    auto b = po_ev("$2");
    b->body = "batch";
    batch.push_back(std::move(b));
    f.s.handle_messages_updated_batch_ui_("!a:x", {2}, std::move(batch));
    CHECK(w->view->message_list()->messages()[2].body == "batch");
}

TEST_CASE("a reply event triggers the pop-out's reply lookup path",
          "[shell][popout][dispatch]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    f.s.handle_timeline_reset_ui_("!a:x", po_list({"$1"}));
    f.s.handle_message_inserted_ui_("!a:x", 1, po_ev("$r", "$1"));
    f.s.handle_message_updated_ui_("!a:x", 1, po_ev("$r", "$1"));
    f.s.handle_messages_appended_ui_("!a:x", [] {
        tesseract::EventList l;
        l.push_back(po_ev("$r2", "$1"));
        return l;
    }());
    CHECK(w->ids().size() >= 3);
}

TEST_CASE("another account's events never touch this account's pop-out",
          "[shell][popout][dispatch]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    auto* w = f.s.window("!a:x");
    {
        PoShell::EventAccountScope scope(f.s, "@other:x");
        f.s.handle_timeline_reset_ui_("!a:x", po_list({"$x"}));
        f.s.handle_messages_appended_ui_("!a:x", po_list({"$y"}));
    }
    CHECK(w->ids().empty());
}

TEST_CASE("media readiness refreshes each pop-out per kind", "[shell][popout][media]")
{
    using K = tk::MediaKind;
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    f.s.open_room_in_new_window("!b:x");
    auto* a = f.s.window("!a:x");
    const int base = a->relayouts;
    for (auto k : {K::MediaImage, K::MediaThumbnail, K::Sticker, K::Reaction,
                   K::Tile, K::RoomAvatar, K::UserAvatar})
        f.s.notify_secondary_media_ready_("mxc://hs/x", k);
    CHECK(a->relayouts == base + 7);
    CHECK(f.s.picker_repaints == 1); // sticker only
}

TEST_CASE("room-info updates reach the pop-out from its own account's list",
          "[shell][popout]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    tesseract::RoomInfo info;
    info.id = "!a:x";
    info.name = "Renamed";
    f.s.rooms_ = {info};
    f.s.per_account_rooms_["@me:x"] = {info};
    f.s.update_secondary_room_infos_();
    SUCCEED();
}

TEST_CASE("window visibility drives video suspension and the animation gate",
          "[shell][popout]")
{
    PoFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    f.s.room_view_ = rv.get();
    CHECK(f.s.any_window_visible_());
    f.s.update_video_playback_suspension_(); // unchanged
    CHECK_FALSE(f.s.video_playback_suspended_);

    f.s.main_visible = false;
    CHECK_FALSE(f.s.any_window_visible_());
    f.s.update_video_playback_suspension_();
    CHECK(f.s.video_playback_suspended_);

    f.s.open_room_in_new_window("!a:x");
    CHECK(f.s.any_window_visible_()); // the pop-out is still on screen
    f.s.window("!a:x")->visible = false;
    CHECK_FALSE(f.s.any_window_visible_());

    f.s.main_visible = true;
    f.s.update_video_playback_suspension_();
    CHECK_FALSE(f.s.video_playback_suspended_);
}

TEST_CASE("room views are enumerated and looked up across windows",
          "[shell][popout]")
{
    PoFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    f.s.room_view_ = rv.get();
    f.s.open_room_in_new_window("!a:x");
    int n = 0;
    f.s.for_each_room_view_([&](tesseract::views::RoomView&) { ++n; });
    CHECK(n == 2);
    CHECK(f.s.room_view_for_room_("!main") == rv.get());
    CHECK(f.s.room_view_for_room_("!a:x") == f.s.window("!a:x")->room_view());
    CHECK(f.s.room_view_for_room_("!none") == nullptr);
    CHECK(f.s.reply_pane_for_("!a:x") == nullptr); // active account: main pane (none here)
}

TEST_CASE("gif results fan out to every pop-out", "[shell][popout]")
{
    PoFx f;
    f.s.open_room_in_new_window("!a:x");
    f.s.dispatch_gif_to_secondary_windows_(1, {});
    f.s.dispatch_gif_failed_to_secondary_windows_(1, "x");
    CHECK(f.s.cached_gif_source_bytes_("https://x/y.gif").empty());
}
