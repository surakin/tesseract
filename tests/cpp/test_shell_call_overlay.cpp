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
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/RoomView.h"
#if TESSERACT_WITH_ENGINE
#include "views/world/WorldLoader.h"
#endif

#include <tesseract/call_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <tesseract/world_trust.h>

#include <filesystem>
#include <fstream>
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
    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void request_relayout_() override
    {
        ++relayouts;
    }
    void request_repaint_() override
    {
        ++repaints;
    }
    tesseract::RtcStartResult rtc_start_call_(const std::string& room, const std::string& slot,
                                              bool audio_only) override
    {
        if (!fake_start)
        {
            return ShellBase::rtc_start_call_(room, slot, audio_only);
        }
        tesseract::RtcStartResult r;
        r.ok = true;
        r.session_id = 7;
        return r;
    }
    void show_message_dialog_(std::string title, std::string text, bool is_error) override
    {
        messages.push_back({std::move(title), std::move(text), is_error});
    }
    void pick_folder_(std::string, std::function<void(std::string)> cb) override
    {
        ++folder_picks;
        folder_cb = std::move(cb);
    }
    void pick_open_file_(std::string, std::function<void(std::string)> cb) override
    {
        ++file_picks;
        file_cb = std::move(cb);
    }
    struct Message
    {
        std::string title, text;
        bool is_error;
    };
    std::vector<Message> messages;
    int folder_picks = 0, file_picks = 0;
    std::function<void(std::string)> folder_cb, file_cb;
    bool fake_start = false; // start_call succeeds without a homeserver
    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int relayouts = 0, repaints = 0;

    using ShellBase::call_auto_floated_;
    using ShellBase::call_overlay_state_;
    using ShellBase::call_session_;
    using ShellBase::active_call_overlay_;
    using ShellBase::client_;
    using ShellBase::ensure_world_loader_;
    using ShellBase::handle_room_world_changed_ui_;
    using ShellBase::main_app_;
    using ShellBase::world_consent_token_;
    using ShellBase::world_seams_for_test_;
    using Seams = ShellBase::WorldLoadSeams;
    using ShellBase::world_loader_;
    using ShellBase::world_wanted_;
    using ShellBase::refresh_world_actions_;
    using ShellBase::run_world_transfer_;
    using ShellBase::world_import_;
    using ShellBase::world_export_;
    using ShellBase::current_room_id_;
    using ShellBase::end_call;
    using ShellBase::handle_call_room_navigation_;
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
    using ShellBase::start_world_send_tick_;
    using ShellBase::world_available_;
    using ShellBase::call_world_offered_;
    using ShellBase::world_tick_running_;
    using ShellBase::call_overlay_teardown_hook_;
    using ShellBase::handle_rtc_video_frame_ui_;
};

tesseract::RoomInfo co_room(const std::string& id, bool call_room = false, bool space = false)
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
        s.call_session_ = std::make_unique<CallSession>(&client, "!call:x", "slot", 1);
    }
};

} // namespace

TEST_CASE("decide_call_room_action covers every state combination", "[shell][call_overlay]")
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

TEST_CASE("floating mode mounts the overlay and persists the preference", "[shell][call_overlay]")
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

TEST_CASE("docked modes follow the call's room and are clamped elsewhere", "[shell][call_overlay]")
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

TEST_CASE("non-persisting transitions leave the saved preference alone", "[shell][call_overlay]")
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

TEST_CASE("navigating to a room not yet listed is retried later", "[shell][call_overlay][nav]")
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
    f.s.request_call_("!call:x", "slot", tesseract::views::CallType::Audio);
    auto* lobby = f.app->room_view()->call_lobby();
    REQUIRE(lobby != nullptr);
    REQUIRE(lobby->on_join);
    // Joining offline fails: the shell reports it and holds no session.
    lobby->on_join("!call:x", "slot", tesseract::views::CallType::Audio, false, false);
    CHECK(f.s.call_session_ == nullptr);
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back().rfind("Call failed", 0) == 0);

    // A different room's active call is ended before joining.
    f.s.call_session_ = std::make_unique<CallSession>(&f.client, "!old:x", "slot", 2);
    lobby->on_join("!call:x", "slot", tesseract::views::CallType::Audio, false, false);
    CHECK(f.s.call_session_ == nullptr);

    // A room that no window shows falls back to joining directly.
    f.s.statuses.clear();
    f.s.request_call_("!hidden:x", "slot", tesseract::views::CallType::Audio);
    f.s.request_call_("!hidden:x", "slot", tesseract::views::CallType::Audio);
    SUCCEED();
}

TEST_CASE("end_call removes the overlay and resets the banner state", "[shell][call_overlay]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    f.s.call_auto_floated_ = true;
    f.s.end_call();
    CHECK(f.s.call_session_ == nullptr);
    CHECK(f.s.active_call_overlay_() == nullptr);
    CHECK_FALSE(f.s.call_auto_floated_);
}

#if TESSERACT_WITH_ENGINE
TEST_CASE("the world choice follows the call across mode switches, and the old engine stops first",
          "[shell][call_overlay][world]")
{
    CoFx f;
    f.s.call_world_offered_ = true;
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    auto* ov = f.s.active_call_overlay_();
    REQUIRE(ov != nullptr);
    ov->click_world_button_for_test();
    CHECK(ov->world_shown());
    CHECK(ov->world_view_active_for_test());

    // The hook runs after the old world view is deactivated, before the new overlay exists.
    int hooks = 0;
    f.s.call_overlay_teardown_hook_ = [&](tesseract::views::CallOverlayWidget& old)
    {
        ++hooks;
        CHECK_FALSE(old.world_view_active_for_test());
        CHECK(old.world_shown()); // the choice is only parked
    };
    f.s.on_call_overlay_mode_requested_(Mode::Floating);
    CHECK(hooks == 1);
    ov = f.s.active_call_overlay_();
    CHECK(ov->world_shown());
    CHECK_FALSE(ov->world_view_active_for_test()); // too small: the grid shows

    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    ov = f.s.active_call_overlay_();
    CHECK(ov->world_shown());
    CHECK(ov->world_view_active_for_test());
    CHECK(ov->world_button_visible_for_test() == true);
}

TEST_CASE("without the capability the overlay offers no world", "[shell][call_overlay][world]")
{
    CoFx f;
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    CHECK_FALSE(f.s.active_call_overlay_()->world_button_visible_for_test());
}

TEST_CASE("start_call starts the world send tick and end_call stops it",
          "[shell][call_overlay][world]")
{
    CoFx f;
    f.s.call_session_.reset();
    f.s.fake_start = true;
    f.s.world_available_ = true;
    f.s.start_call("!call:x", "slot", tesseract::views::CallType::Audio, false, false);
    REQUIRE(f.s.call_session_ != nullptr);
    CHECK(f.s.call_world_offered_);
    CHECK(f.s.world_tick_running_);
    REQUIRE(f.s.delayed.size() >= 1);
    // The overlay of this call offers the world.
    f.s.active_call_overlay_()->set_world_available(true);

    // Every pending tick re-arms itself while the call lives.
    auto pending = std::move(f.s.delayed);
    f.s.delayed.clear();
    const std::size_t armed = pending.size();
    for (auto& fn : pending)
    {
        fn();
    }
    CHECK(f.s.delayed.size() >= 1); // at least the tick came back
    CHECK(f.s.delayed.size() <= armed + 1);

    f.s.end_call();
    CHECK_FALSE(f.s.world_tick_running_);
    CHECK_FALSE(f.s.call_world_offered_);
    auto stale = std::move(f.s.delayed);
    f.s.delayed.clear();
    const std::size_t before = stale.size();
    for (auto& fn : stale)
    {
        fn();
    }
    // Nothing re-armed the tick (other pending tasks may have posted their own).
    CHECK_FALSE(f.s.world_tick_running_);
    (void)before;
}

TEST_CASE("a call started before the probe finished does not offer the world",
          "[shell][call_overlay][world]")
{
    CoFx f;
    f.s.call_session_.reset();
    f.s.fake_start = true;
    f.s.world_available_ = false;
    f.s.start_call("!call:x", "slot", tesseract::views::CallType::Audio, false, false);
    REQUIRE(f.s.active_call_overlay_() != nullptr);
    f.s.world_available_ = true; // the probe finishes mid-call
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    CHECK_FALSE(f.s.active_call_overlay_()->world_button_visible_for_test());
    CHECK_FALSE(f.s.world_tick_running_);
    f.s.start_world_send_tick_();
    CHECK_FALSE(f.s.world_tick_running_);

    // The next call has it.
    f.s.end_call();
    f.s.start_call("!call:x", "slot", tesseract::views::CallType::Audio, false, false);
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    CHECK(f.s.active_call_overlay_()->world_button_visible_for_test());
    CHECK(f.s.world_tick_running_);
}

TEST_CASE("a remote video frame reaches the world view while the world is shown",
          "[shell][call_overlay][world]")
{
    CoFx f;
    f.s.call_world_offered_ = true;
    f.s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
    auto* ov = f.s.active_call_overlay_();
    auto frame = std::make_shared<std::vector<std::uint8_t>>(4 * 4 * 4, 90);
    f.s.handle_rtc_video_frame_ui_(1, "p2", 4, 4, frame);
    CHECK(ov->world_frames_for_test() == 0);
    ov->click_world_button_for_test();
    f.s.handle_rtc_video_frame_ui_(1, "p2", 4, 4, frame);
    CHECK(ov->world_frames_for_test() == 1);
    f.s.handle_rtc_video_frame_ui_(2, "p2", 4, 4, frame); // another call's frame
    CHECK(ov->world_frames_for_test() == 1);
}

namespace
{
const char* const kWorldHashA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char* const kWorldHashB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
const char* const kWorldManifest =
    R"({"format":1,"name":"T","field_asset":"f.glb","user_asset":"r.glb","user_script":{"file":"robot.lua","attach":"robot"}})";

// A call with the real world path (ShellBase::world_wanted_ -> WorldLoader -> the consent dialog and
// the overlay) over a fake room, download, extraction and a deferred worker.
struct WorldFx : CoFx
{
    std::string hash = kWorldHashA;
    bool read_world = true; // false: the room has no world
    bool verified = false;
    int reads = 0, extracts = 0;
    std::vector<std::pair<std::function<void()>, std::function<void()>>> queue;
    tesseract::views::CallOverlayWidget* ov = nullptr;

    WorldFx()
    {
        s.call_world_offered_ = true;
        s.on_call_overlay_mode_requested_(Mode::DockedExpanded);
        ov = s.active_call_overlay_();
        CoShell::Seams seams;
        seams.room_world = [this](const std::string&)
        {
            ++reads;
            tesseract::RoomWorldRead r;
            if (read_world)
            {
                r.kind = tesseract::RoomWorldRead::Kind::World;
                r.world = {"Arena", hash, "mxc://s/x", "@alice:s", 3, 1};
            }
            return r;
        };
        seams.download = [](const tesseract::RoomWorld&, std::vector<std::uint8_t>& out)
        {
            out = {1, 2, 3};
            return tesseract::Result{true, {}};
        };
        seams.sender_verified = [this](const std::string&)
        {
            return verified;
        };
        seams.extract = [this](const std::vector<std::uint8_t>&, const std::string& dest)
        {
            ++extracts;
            std::filesystem::create_directories(dest);
            std::ofstream(std::filesystem::path(dest) / "world.json") << kWorldManifest;
            return tesseract::WorldBundleExtract{true, {}, kWorldManifest};
        };
        seams.run_async = [this](std::function<void()> work, std::function<void()> done)
        {
            queue.emplace_back(std::move(work), std::move(done));
        };
        s.world_seams_for_test_ = std::move(seams);
    }
    // The worker runs everything queued, then the UI-thread completions (which may queue more).
    void drain()
    {
        for (int i = 0; i < 6 && !queue.empty(); ++i)
        {
            auto q = std::move(queue);
            queue.clear();
            for (auto& [work, done] : q)
            {
                work();
            }
            for (auto& [work, done] : q)
            {
                done();
            }
        }
    }
    bool trusted(const std::string& h)
    {
        return tesseract::WorldTrustStore(tesseract::WorldTrustStore::default_path()).trusted(h);
    }
    bool declined_message_shown() const
    {
        for (const auto& m : s.statuses)
        {
            if (m.find("not run") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};
} // namespace

TEST_CASE("shell_call: the real world path reads the room only while World is on screen",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    REQUIRE(f.ov != nullptr);
    f.s.world_available_ = true;
    f.ov->set_world_available(true);
    f.drain();
    CHECK(f.queue.empty()); // the grid is showing: nothing downloaded or run
    CHECK(f.reads == 0);

    f.verified = true;
    f.ov->click_world_button_for_test(); // World
    CHECK(f.queue.size() == 1);
    f.drain();
    CHECK(f.reads >= 1);
    CHECK(f.extracts == 1);
    CHECK(f.s.world_loader_ != nullptr);

    const int before = f.reads;
    f.ov->click_world_button_for_test(); // Grid
    f.ov->click_world_button_for_test(); // World again: the room is read again
    f.drain();
    CHECK(f.reads > before);
    CHECK(f.extracts == 1); // the extraction is reused
}

TEST_CASE("shell_call: a world change shows the banner, Reload reads the room again",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    f.verified = true;
    f.ov->set_world_available(true);
    f.ov->click_world_button_for_test();
    f.drain();
    REQUIRE(f.s.world_loader_ != nullptr);
    REQUIRE(f.s.world_loader_->loaded());

    f.s.handle_room_world_changed_ui_("!call:x"); // the same world again
    f.drain();
    CHECK_FALSE(f.ov->world_banner_visible());

    f.hash = kWorldHashB;
    f.s.handle_room_world_changed_ui_("!other:x"); // another room's world
    CHECK(f.queue.empty());
    f.s.handle_room_world_changed_ui_("!call:x");
    f.drain();
    CHECK(f.ov->world_banner_visible());
    CHECK(f.s.world_loader_->loaded_hash() == kWorldHashA); // still running the old one

    f.ov->click_world_banner_for_test(); // the real Reload
    CHECK(f.queue.size() == 1);
    f.drain();
    CHECK(f.s.world_loader_->loaded_hash() == kWorldHashB);
    CHECK_FALSE(f.ov->world_banner_visible());
}

TEST_CASE("shell_call: a world change with the overlay gone or no call does nothing",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    f.verified = true;
    f.ov->set_world_available(true);
    f.ov->click_world_button_for_test();
    f.drain();
    REQUIRE(f.s.world_loader_->loaded());

    f.hash = kWorldHashB;
    f.app->unmount_call_overlay(); // the overlay is gone, the call is not
    f.s.handle_room_world_changed_ui_("!call:x");
    f.drain(); // the callbacks find no overlay
    CHECK(f.s.active_call_overlay_() == nullptr);

    f.s.end_call();
    f.s.handle_room_world_changed_ui_("!call:x"); // no call: no work
    CHECK(f.queue.empty());
}

TEST_CASE(
    "shell_call: ending the call closes the consent question; nothing is decided or persisted",
    "[shell][call_overlay][world]")
{
    WorldFx f;
    f.ov->set_world_available(true);
    f.ov->click_world_button_for_test();
    f.drain();
    auto* dialog = f.s.main_app_->confirm_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->is_open()); // asked: the sender is not verified
    CHECK(f.ov->world_waiting_text_for_test() == "Waiting for your answer in the main window");

    f.s.end_call();
    CHECK_FALSE(dialog->is_open());
    CHECK(f.s.world_consent_token_ == 0);
    CHECK_FALSE(f.trusted(kWorldHashA));
    CHECK_FALSE(f.declined_message_shown()); // cancelled before the dismissal, so no "not run"
    f.drain();
    CHECK_FALSE(f.declined_message_shown());
}

TEST_CASE("shell_call: a second question replaces the first and hang-up still closes it",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    f.ov->set_world_available(true);
    f.ov->click_world_button_for_test();
    f.drain();
    auto* dialog = f.s.main_app_->confirm_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->is_open());

    f.s.world_wanted_(); // a second request: its question replaces the first dialog
    f.drain();
    CHECK(dialog->is_open());
    CHECK(f.s.world_consent_token_ != 0); // the replaced question's dismissal did not clear it

    f.s.end_call();
    CHECK_FALSE(dialog->is_open());
    CHECK(f.s.world_consent_token_ == 0);
    CHECK_FALSE(f.trusted(kWorldHashA));
}

TEST_CASE("shell_call: the waiting text goes away when the question is answered",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    f.ov->set_world_available(true);
    f.ov->click_world_button_for_test();
    CHECK(f.ov->world_waiting_text_for_test() == "Loading the world…");
    f.drain();
    CHECK(f.ov->world_waiting_text_for_test() == "Waiting for your answer in the main window");
    f.s.main_app_->confirm_dialog()->confirm(); // Run once
    CHECK(f.ov->world_waiting_text_for_test().empty());
    CHECK(f.s.world_loader_->loaded());
    CHECK_FALSE(f.trusted(kWorldHashA)); // once, not always
}

TEST_CASE("shell_call: the Import entries follow can_set_room_world and stay quiet while off",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    bool allowed = false;
    f.s.world_seams_for_test_->can_set_room_world = [&](const std::string& room)
    {
        CHECK(room == "!call:x");
        return allowed;
    };
    f.s.refresh_world_actions_();
    auto* bar = f.ov->world_button_bar();
    REQUIRE(bar != nullptr);
    CHECK_FALSE(bar->entry_enabled("import_folder"));
    CHECK_FALSE(bar->entry_enabled("import_zip"));
    CHECK(bar->entry_enabled("export")); // anyone in the room may export
    bar->entry_button("import_folder")->click();
    bar->entry_button("import_zip")->click();
    CHECK(f.s.folder_picks == 0);
    CHECK(f.s.file_picks == 0);

    allowed = true;
    f.s.refresh_world_actions_();
    CHECK(bar->entry_enabled("import_folder"));
    CHECK(bar->entry_enabled("import_zip"));
    bar->entry_button("import_folder")->click();
    CHECK(f.s.folder_picks == 1);
    bar->entry_button("import_zip")->click();
    CHECK(f.s.file_picks == 1);
    bar->entry_button("export")->click();
    CHECK(f.s.folder_picks == 2);

    allowed = false; // the power level was taken away
    f.s.refresh_world_actions_();
    CHECK_FALSE(bar->entry_enabled("import_folder"));
}

TEST_CASE(
    "shell_call: a transfer whose work throws still closes the busy dialog and shows an error",
    "[shell][call_overlay][world]")
{
    WorldFx f;
    REQUIRE(f.app->busy_dialog() != nullptr);
    f.s.run_world_transfer_(
        "Working",
        [](std::string&) -> bool
        {
            throw std::invalid_argument("bad bytes");
        },
        "Done");
    CHECK(f.app->busy_dialog()->is_open());
    f.drain();
    CHECK_FALSE(f.app->busy_dialog()->is_open());
    REQUIRE(f.s.messages.size() == 1);
    CHECK(f.s.messages[0].is_error);
    CHECK(f.s.messages[0].text.find("bad bytes") != std::string::npos);

    f.s.run_world_transfer_(
        "Working",
        [](std::string&) -> bool
        {
            throw 7;
        },
        "Done");
    f.drain();
    CHECK_FALSE(f.app->busy_dialog()->is_open());
    REQUIRE(f.s.messages.size() == 2);
    CHECK(f.s.messages[1].is_error);
    CHECK_FALSE(f.s.messages[1].text.empty());

    f.s.run_world_transfer_(
        "Working",
        [](std::string& m)
        {
            m = "all good";
            return true;
        },
        "Done");
    f.drain();
    CHECK_FALSE(f.app->busy_dialog()->is_open());
    REQUIRE(f.s.messages.size() == 3);
    CHECK_FALSE(f.s.messages[2].is_error);
    CHECK(f.s.messages[2].title == "Done");
    CHECK(f.s.messages[2].text == "all good");
}

TEST_CASE("shell_call: an import picked after the call ended does nothing and shows nothing",
          "[shell][call_overlay][world]")
{
    WorldFx f; // no account behind the call: it counts as ended
    f.s.world_import_(false);
    REQUIRE(f.s.folder_cb);
    f.s.folder_cb("/some/world");
    CHECK_FALSE(f.app->busy_dialog()->is_open());
    CHECK(f.queue.empty());
    CHECK(f.s.messages.empty());
}

#endif

TEST_CASE("shell_call: the world's button bar steps aside while the changed banner shows",
          "[shell][call_overlay][world]")
{
    WorldFx f;
    auto* bar = f.ov->world_button_bar();
    REQUIRE(bar != nullptr);
    CHECK(bar->own_visible());
    f.ov->show_world_banner("The world changed");
    CHECK_FALSE(bar->own_visible());
    f.ov->hide_world_banner();
    CHECK(bar->own_visible());
}
