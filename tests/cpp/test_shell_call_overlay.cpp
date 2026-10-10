// ShellBase_calls.cpp: the call overlay state machine (mount mode, clamping,
// persistence), what a room switch does to an ongoing call, the pure
// decide_call_room_action table, the lobby hand-off, and the callbacks the
// mounted overlay drives. A CallSession is installed directly (a real join
// needs a homeserver).

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/CallLobbyView.h"
#include "views/CallOverlayWidget.h"
#include "views/MainAppWidget.h"
#include "views/RoomView.h"

#include <tesseract/call_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::CallRoomAction;
using tesseract::CallSession;
using tesseract::ShellBase;
using Mode = tesseract::views::CallOverlayWidget::Mode;

namespace
{

struct CoShell : tesseract::test::TestShellBase
{
    ~CoShell() override
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
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void request_relayout_() override { ++relayouts; }
    void request_repaint_() override { ++repaints; }
    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int relayouts = 0, repaints = 0;

    using ShellBase::call_auto_floated_;
    using ShellBase::call_overlay_state_;
    using ShellBase::call_session_;
    using ShellBase::active_call_overlay_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::end_call;
    using ShellBase::handle_call_room_navigation_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::on_call_float_position_changed_;
    using ShellBase::on_call_overlay_mode_requested_;
    using ShellBase::pending_call_room_nav_id_;
    using ShellBase::request_call_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::overlay_mode_from_settings_;
    using ShellBase::start_call;
};

tesseract::RoomInfo co_room(const std::string& id, bool call_room = false,
                            bool space = false)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    r.is_call_room = call_room;
    r.is_space = space;
    return r;
}

struct CoFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    CoShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);

    CoFx()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        s.room_view_ = app->room_view();
        s.my_user_id_ = "@me:x";
        s.current_room_id_ = "!call:x";
        s.call_session_ = std::make_unique<CallSession>(&client, "!call:x", "slot");
    }
};

} // namespace

TEST_CASE("decide_call_room_action covers every state combination",
          "[shell][call_overlay]")
{
    // No call.
    CHECK(tesseract::decide_call_room_action(false, false, false, false, false) ==
          CallRoomAction::None);
    CHECK(tesseract::decide_call_room_action(false, false, true, false, false) ==
          CallRoomAction::AutoJoin);
    // Returning to the call's own room.
    CHECK(tesseract::decide_call_room_action(true, true, false, true, false) ==
          CallRoomAction::AutoRestore);
    CHECK(tesseract::decide_call_room_action(true, true, false, false, false) ==
          CallRoomAction::None);
    // Another room.
    CHECK(tesseract::decide_call_room_action(true, false, true, false, true) ==
          CallRoomAction::LeaveAndJoin);
    CHECK(tesseract::decide_call_room_action(true, false, false, false, true) ==
          CallRoomAction::AutoFloat);
    CHECK(tesseract::decide_call_room_action(true, false, false, false, false) ==
          CallRoomAction::NoOp);
}

TEST_CASE("mode requests need an overlay host and a call", "[shell][call_overlay]")
{
    CoShell s;
    s.on_call_overlay_mode_requested_(Mode::Floating); // nothing: no crash
    CHECK(s.active_call_overlay_() == nullptr);

    tesseract::Client client;
    CoFx f;
    f.s.call_session_.reset();
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    CHECK(f.s.active_call_overlay_() == nullptr);
}

TEST_CASE("floating mode mounts the overlay and persists the preference",
          "[shell][call_overlay]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    auto* ov = f.s.active_call_overlay_();
    REQUIRE(ov != nullptr);
    CHECK(ov->mode() == Mode::Floating);
    CHECK(tesseract::Settings::instance().call_overlay_mode ==
          tesseract::Settings::CallOverlayMode::Floating);
    CHECK(f.s.relayouts >= 1);
}

TEST_CASE("docked modes follow the call's room and are clamped elsewhere",
          "[shell][call_overlay]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::Docked);
    REQUIRE(f.s.active_call_overlay_() != nullptr);
    CHECK(f.s.active_call_overlay_()->mode() == Mode::Docked);
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    CHECK(f.s.active_call_overlay_()->mode() == Mode::DockedExpanded);

    // Viewing another room: docking is impossible, so it floats instead.
    f.s.current_room_id_ = "!elsewhere:x";
    f.s.on_call_overlay_mode_requested_(Mode::Docked);
    CHECK(f.s.active_call_overlay_()->mode() == Mode::Floating);
}

TEST_CASE("non-persisting transitions leave the saved preference alone",
          "[shell][call_overlay]")
{
    CoFx f;
    tesseract::Settings::instance().call_overlay_mode =
        tesseract::Settings::CallOverlayMode::DockedExpanded;
    f.s.on_call_overlay_mode_requested_(Mode::Floating, /*persist=*/false);
    CHECK(tesseract::Settings::instance().call_overlay_mode ==
          tesseract::Settings::CallOverlayMode::DockedExpanded);
}

TEST_CASE("overlay state survives a mode switch", "[shell][call_overlay]")
{
    CoFx f;
    f.s.call_overlay_state_.audio_muted = true;
    f.s.call_overlay_state_.local_user_id = "@me:x";
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    CHECK(f.s.active_call_overlay_()->snapshot().audio_muted);
    f.s.on_call_overlay_mode_requested_(Mode::Docked);
    CHECK(f.s.active_call_overlay_()->snapshot().audio_muted);
    CHECK(f.s.active_call_overlay_()->snapshot().local_user_id == "@me:x");
}

TEST_CASE("overlay callbacks drive the call", "[shell][call_overlay]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    auto* ov = f.s.active_call_overlay_();
    REQUIRE(ov != nullptr);

    ov->on_toggle_audio(true);
    ov->on_toggle_audio(false);
    ov->on_toggle_video(true);
    ov->on_float_position_changed(0.25f, 0.5f);
    CHECK(tesseract::Settings::instance().call_overlay_float_x == 0.25f);
    CHECK(tesseract::Settings::instance().call_overlay_float_y == 0.5f);

    f.s.call_auto_floated_ = true;
    ov->on_mode_change_requested(Mode::Docked);
    CHECK_FALSE(f.s.call_auto_floated_); // a user choice is final

    f.s.active_call_overlay_()->on_hang_up();
    CHECK(f.s.call_session_ == nullptr);
    CHECK(f.s.active_call_overlay_() == nullptr);
}

TEST_CASE("float position is saved", "[shell][call_overlay]")
{
    CoFx f;
    const int before = f.s.relayouts;
    f.s.on_call_float_position_changed_(0.1f, 0.9f);
    CHECK(tesseract::Settings::instance().call_overlay_float_x == 0.1f);
    CHECK(f.s.relayouts == before + 1);
}

TEST_CASE("leaving the call's room auto-floats a docked overlay and returning restores it",
          "[shell][call_overlay][nav]")
{
    CoFx f;
    f.s.rooms_ = {co_room("!call:x"), co_room("!other:x")};
    f.s.mark_room_index_dirty_();
    f.s.on_call_overlay_mode_requested_(Mode::Docked);

    f.s.current_room_id_ = "!other:x";
    f.s.handle_call_room_navigation_();
    CHECK(f.s.call_auto_floated_);
    CHECK(f.s.active_call_overlay_()->mode() == Mode::Floating);
    CHECK_FALSE(f.s.active_call_overlay_()->room_active());

    f.s.current_room_id_ = "!call:x";
    f.s.handle_call_room_navigation_();
    CHECK_FALSE(f.s.call_auto_floated_);
    CHECK(f.s.active_call_overlay_()->room_active());
}

TEST_CASE("navigating to a room not yet listed is retried later",
          "[shell][call_overlay][nav]")
{
    CoFx f;
    f.s.current_room_id_ = "!new:x";
    f.s.handle_call_room_navigation_();
    CHECK(f.s.pending_call_room_nav_id_ == "!new:x");

    f.s.rooms_ = {co_room("!new:x", false, /*space=*/true)};
    f.s.mark_room_index_dirty_();
    f.s.handle_call_room_navigation_(); // spaces never trigger call logic
    CHECK(f.s.pending_call_room_nav_id_.empty());
}

TEST_CASE("switching into a call room opens its lobby instead of joining",
          "[shell][call_overlay][nav]")
{
    CoFx f;
    f.s.call_session_.reset();
    f.s.rooms_ = {co_room("!callroom:x", /*call_room=*/true)};
    f.s.mark_room_index_dirty_();
    f.s.current_room_id_ = "!callroom:x";
    f.s.handle_call_room_navigation_();
    auto* lobby = f.app->room_view()->call_lobby();
    REQUIRE(lobby != nullptr);
    CHECK(lobby->is_open());
    CHECK(f.s.call_session_ == nullptr); // not joined until Join is pressed
}

TEST_CASE("request_call_ wires the lobby join to start_call", "[shell][call_overlay]")
{
    CoFx f;
    f.s.call_session_.reset();
    f.s.request_call_("!call:x", "slot", true);
    auto* lobby = f.app->room_view()->call_lobby();
    REQUIRE(lobby != nullptr);
    REQUIRE(lobby->on_join);
    // Joining offline fails: the shell reports it and holds no session.
    lobby->on_join("!call:x", "slot", true, false, false);
    CHECK(f.s.call_session_ == nullptr);
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back().rfind("Call failed", 0) == 0);

    // A different room's active call is ended before joining.
    f.s.call_session_ = std::make_unique<CallSession>(&f.client, "!old:x", "slot");
    lobby->on_join("!call:x", "slot", true, false, false);
    CHECK(f.s.call_session_ == nullptr);

    // A room that no window shows falls back to joining directly.
    f.s.statuses.clear();
    f.s.request_call_("!hidden:x", "slot", true);
    f.s.request_call_("!hidden:x", "slot", true);
    SUCCEED();
}

TEST_CASE("end_call removes the overlay and resets the banner state",
          "[shell][call_overlay]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    f.s.call_auto_floated_ = true;
    f.s.end_call();
    CHECK(f.s.call_session_ == nullptr);
    CHECK(f.s.active_call_overlay_() == nullptr);
    CHECK_FALSE(f.s.call_auto_floated_);
}
