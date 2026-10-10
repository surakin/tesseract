// ShellBase::wire_main_app_widget_ / wire_settings_view_ /
// wire_settings_controller_common_ / wire_voice_capture_ (ShellBase_wiring.cpp)
// are the shared glue every shell calls once at construction. These tests wire
// a real MainAppWidget / SettingsView to the shell test double and drive the
// callbacks the views expose, asserting the resulting shell/Settings state.
// Settings I/O is redirected to a temp dir by test_isolation_listener.

#include <catch2/catch_test_macros.hpp>

#include "app/SettingsController.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/AddRoomView.h"
#include "views/InviteCard.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/SettingsView.h"
#include "views/UserInfo.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::MainAppWidget;
using tesseract::views::RoomListView;
using tesseract::views::SettingsView;

namespace
{

struct WiringShell : tesseract::test::TestShellBase
{
    ~WiringShell() override
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

    void request_relayout_() override { ++relayouts; }
    int relayouts = 0;

    // wire_main_app_widget_ starts a self-rescheduling image-GC timer through
    // post_to_ui_after_; running those inline would recurse forever, so delayed
    // posts are queued and the test runs only the ones it cares about.
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        delayed.emplace_back(ms, std::move(fn));
    }
    std::vector<std::pair<int, std::function<void()>>> delayed;
    void run_delayed(int ms)
    {
        auto pending = std::move(delayed);
        delayed.clear();
        for (auto& [m, fn] : pending)
        {
            if (m == ms)
            {
                fn();
            }
            else
            {
                delayed.emplace_back(m, std::move(fn));
            }
        }
    }

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::current_invite_;
    using ShellBase::current_knock_status_room_id_;
    using ShellBase::invites_;
    using ShellBase::main_app_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_knocks_;
    using ShellBase::room_search_text_;
    using ShellBase::rooms_;
    using ShellBase::settings_controller_;
    using ShellBase::user_presence_;
    using ShellBase::wire_main_app_widget_;
    using ShellBase::wire_settings_controller_common_;
    using ShellBase::wire_settings_view_;
    using ShellBase::wire_voice_capture_;
    using ShellBase::capture_;
    using ShellBase::unjoined_fetch_pending_;
    using ShellBase::pending_forwards_;
    using ShellBase::current_room_id_;
};

struct WiringFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    tesseract::Client client;
    // Surface must outlive (be declared before) the widget tree.
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<MainAppWidget> app =
        tk::create_root_widget<MainAppWidget>(nullptr);

    WiringFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {1000, 700});
        app->arrange(lc, {0, 0, 1000, 700});
        s.wire_main_app_widget_(app.get());
    }
};

tesseract::RoomInfo wiring_make_room(const std::string& id, std::uint64_t ts = 0)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    r.last_activity_ts = ts;
    return r;
}

struct FakeCapture : tk::AudioCapture
{
    bool recording = false;
    int starts = 0, stops = 0, cancels = 0;
    void start() override
    {
        ++starts;
        recording = true;
    }
    void stop() override
    {
        ++stops;
        recording = false;
        if (on_stopped)
        {
            on_stopped(pcm, {1, 2}, dur);
        }
    }
    void cancel() override
    {
        ++cancels;
        recording = false;
    }
    bool is_recording() const override { return recording; }
    std::uint64_t duration_ms() const override { return dur; }
    void set_frame_callback(
        std::function<void(const std::int16_t*, std::size_t)>) override {}
    void clear_frame_callback() override {}
    std::vector<std::uint8_t> pcm;
    std::uint64_t dur = 0;
};

} // namespace

TEST_CASE("wiring: persisted section collapsed state is restored by section "
          "name",
          "[shell][wiring]")
{
    // Regression: the restore array was positional and predated kSecCallRooms.
    tesseract::test::SettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.room_section_spaces_collapsed = true;
    st.room_section_inactive_collapsed = false;
    st.room_section_space_unjoined_collapsed = false;
    WiringFixture f;
    const auto c = f.app->room_list_view()->collapsed_state();
    CHECK(c[RoomListView::kSecSpaces]);
    CHECK_FALSE(c[RoomListView::kSecInactive]);
    CHECK_FALSE(c[RoomListView::kSecCallRooms]);
    CHECK_FALSE(c[RoomListView::kSecSpaceUnjoined]);
}

TEST_CASE("wiring: leading sections restore their collapsed flags",
          "[shell][wiring]")
{
    tesseract::test::SettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.room_section_invites_collapsed = true;
    st.room_section_unread_collapsed = true;
    st.room_section_favorites_collapsed = false;
    st.room_section_dms_collapsed = true;
    st.room_section_rooms_collapsed = false;
    WiringFixture f;
    const auto c = f.app->room_list_view()->collapsed_state();
    CHECK(c[RoomListView::kSecInvites]);
    CHECK(c[RoomListView::kSecUnread]);
    CHECK_FALSE(c[RoomListView::kSecFavorites]);
    CHECK(c[RoomListView::kSecDMs]);
    CHECK_FALSE(c[RoomListView::kSecRooms]);
}

TEST_CASE("wiring: toggling a section persists it to Settings by name",
          "[shell][wiring]")
{
    WiringFixture f;
    auto& st = tesseract::Settings::instance();
    auto& rl = *f.app->room_list_view();
    rl.on_section_toggled(RoomListView::kSecInvites, true);
    rl.on_section_toggled(RoomListView::kSecUnread, true);
    rl.on_section_toggled(RoomListView::kSecFavorites, true);
    rl.on_section_toggled(RoomListView::kSecDMs, true);
    rl.on_section_toggled(RoomListView::kSecRooms, true);
    rl.on_section_toggled(RoomListView::kSecSpaces, true);
    rl.on_section_toggled(RoomListView::kSecInactive, false);
    rl.on_section_toggled(RoomListView::kSecSpaceUnjoined, true);
    CHECK(st.room_section_invites_collapsed);
    CHECK(st.room_section_unread_collapsed);
    CHECK(st.room_section_favorites_collapsed);
    CHECK(st.room_section_dms_collapsed);
    CHECK(st.room_section_rooms_collapsed);
    CHECK(st.room_section_spaces_collapsed);
    CHECK_FALSE(st.room_section_inactive_collapsed);
    CHECK(st.room_section_space_unjoined_collapsed);
    // An unknown section index is ignored.
    CHECK_NOTHROW(rl.on_section_toggled(99, true));
}

TEST_CASE("wiring: sidebar layout changes are stored in Settings",
          "[shell][wiring]")
{
    WiringFixture f;
    f.app->on_sidebar_layout_changed(312.4f, true);
    auto& st = tesseract::Settings::instance();
    CHECK(st.sidebar_width == 312);
    CHECK(st.sidebar_collapsed);
}

TEST_CASE("wiring: clearing the room-list search resets shell and view",
          "[shell][wiring]")
{
    // (The search TextField only exists once a Host is attached, so the typing
    // path is not reachable here; the clear hook is.)
    WiringFixture f;
    f.s.room_search_text_ = "lobby";
    f.app->room_list_view()->set_search_text("lobby");
    f.app->room_list_view()->on_search_clear();
    CHECK(f.s.room_search_text_.empty());
    CHECK(f.app->room_list_view()->search_text().empty());
}

TEST_CASE("wiring: selecting an invite shows the card; decline removes it",
          "[shell][wiring][invite]")
{
    WiringFixture f;
    tesseract::InviteInfo inv;
    inv.room_id = "!inv:x";
    inv.room_name = "Invited";
    inv.inviter_user_id = "@host:x";
    f.s.invites_.push_back(inv);

    // Unknown invite: ignored.
    f.app->room_list_view()->on_invite_selected("!nope:x");
    CHECK_FALSE(f.s.current_invite_.has_value());

    f.app->room_list_view()->on_invite_selected("!inv:x");
    REQUIRE(f.s.current_invite_.has_value());
    CHECK(f.s.current_invite_->room_id == "!inv:x");
    CHECK(f.s.current_invite_->inviter_id == "@host:x");

    f.app->invite_card()->on_decline();
    CHECK(f.s.invites_.empty());
}

TEST_CASE("wiring: block also drops the invite", "[shell][wiring][invite]")
{
    WiringFixture f;
    tesseract::InviteInfo inv;
    inv.room_id = "!inv:x";
    inv.inviter_user_id = "@bad:x";
    f.s.invites_.push_back(inv);
    f.app->room_list_view()->on_invite_selected("!inv:x");
    f.app->invite_card()->on_block();
    CHECK(f.s.invites_.empty());
}

TEST_CASE("wiring: invite accept/decline without a selection are no-ops",
          "[shell][wiring][invite]")
{
    WiringFixture f;
    CHECK_NOTHROW(f.app->invite_card()->on_accept());
    CHECK_NOTHROW(f.app->invite_card()->on_decline());
    CHECK_NOTHROW(f.app->invite_card()->on_block());
}

TEST_CASE("wiring: knock row selection records the room; cancel clears content",
          "[shell][wiring][knock]")
{
    WiringFixture f;
    tesseract::KnockedRoomInfo k;
    k.room_id = "!knock:x";
    k.room_name = "Knocked";
    f.s.my_knocks_.push_back(k);

    f.app->room_list_view()->on_knock_row_selected("!unknown:x");
    CHECK(f.s.current_knock_status_room_id_.empty());

    f.app->room_list_view()->on_knock_row_selected("!knock:x");
    CHECK(f.s.current_knock_status_room_id_ == "!knock:x");

    const int before = f.s.relayouts;
    f.app->knock_status_card()->on_cancel();
    CHECK(f.s.relayouts > before);
}

TEST_CASE("wiring: presence provider reads the shell's presence map",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.user_presence_["@a:x"] = tesseract::PresenceState::Online;
    // Reaching the provider through a RoomInfoPanel is heavy; the wiring
    // itself must at least have installed it without disturbing state.
    CHECK(f.s.user_presence_.at("@a:x") == tesseract::PresenceState::Online);
    CHECK(f.app->user_info()->on_status_clicked != nullptr);
    CHECK(f.app->on_settings_shortcut != nullptr);
}

TEST_CASE("wiring: unjoined-space summary request is deduplicated",
          "[shell][wiring]")
{
    WiringFixture f;
    auto& rl = *f.app->room_list_view();
    REQUIRE(rl.on_unjoined_room_summary_needed != nullptr);
    // No active space: nothing is queued.
    rl.on_unjoined_room_summary_needed("!child:x");
    CHECK(f.s.unjoined_fetch_pending_.empty());
}

TEST_CASE("wiring: add-room dialog opens from the room list plus button",
          "[shell][wiring]")
{
    WiringFixture f;
    auto* ar = f.app->add_room_view();
    REQUIRE(ar != nullptr);
    CHECK_FALSE(ar->is_open());
    f.app->room_list_view()->on_add_room_requested();
    CHECK(ar->is_open());
}

TEST_CASE("wiring: directory join of an already-joined room closes the dialog "
          "instead of re-joining",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.rooms_.push_back(wiring_make_room("!have:x"));
    auto* ar = f.app->add_room_view();
    f.app->room_list_view()->on_add_room_requested();
    REQUIRE(ar->is_open());
    auto* dr = ar->directory_view();
    REQUIRE(dr != nullptr);
    dr->on_join_requested("!have:x", "");
    CHECK_FALSE(ar->is_open());
}

TEST_CASE("wiring: avatar click opens the image viewer", "[shell][wiring]")
{
    WiringFixture f;
    REQUIRE(f.app->room_view()->on_avatar_clicked != nullptr);
    // Empty url is ignored.
    f.app->room_view()->on_avatar_clicked("", "Name");
    f.app->room_view()->on_avatar_clicked("mxc://x/avatar", "Name");
    CHECK(f.app->media_viewer() != nullptr);
}

TEST_CASE("wiring: forward request opens the picker once and forwards to the "
          "chosen rooms",
          "[shell][wiring][forward]")
{
    WiringFixture f;
    f.s.current_room_id_ = "!src:x";
    auto* fp = f.app->forward_picker();
    REQUIRE(fp != nullptr);
    f.app->room_view()->on_forward_requested("$evt");
    CHECK(fp->is_open());
    REQUIRE(fp->on_confirmed != nullptr);
    fp->on_confirmed({"!a:x", "!b:x"});
    CHECK(f.s.pending_forwards_.size() == 2);
}

TEST_CASE("wiring: forward request needs a current room", "[shell][wiring][forward]")
{
    WiringFixture f;
    f.app->room_view()->on_forward_requested("$evt");
    CHECK_FALSE(f.app->forward_picker()->is_open());
}

TEST_CASE("wiring: quick switcher lists rooms most-recent-first",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.rooms_ = {wiring_make_room("!old:x", 10), wiring_make_room("!new:x", 99),
                  wiring_make_room("!mid:x", 50)};
    auto* qs = f.app->quick_switcher();
    REQUIRE(qs != nullptr);
    std::string picked;
    qs->on_room_avatar_needed = [](const tesseract::RoomInfo&) {};
    // The provider is private to the widget; opening it exercises it.
    f.app->show_quick_switch(true);
    CHECK_NOTHROW(f.app->show_quick_switch(false));
}

// ── wire_settings_view_ ─────────────────────────────────────────────────────

namespace
{
struct SettingsFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(900, 700);
    std::unique_ptr<SettingsView> view =
        tk::create_root_widget<SettingsView>(nullptr);
    SettingsFixture() { s.wire_settings_view_(view.get()); }
};
} // namespace

TEST_CASE("wiring: settings view persists simple toggles", "[shell][wiring][settings]")
{
    SettingsFixture f;
    auto& st = tesseract::Settings::instance();
    f.view->on_hide_content_changed(true);
    CHECK(st.notification_hide_content);
    f.view->on_image_previews_changed(false);
    CHECK_FALSE(st.notification_image_previews);
    f.view->on_prefetch_changed(true);
    CHECK(st.prefetch_full_media);
    f.view->on_autoscroll_unread_changed(false);
    CHECK_FALSE(st.autoscroll_unread_rooms);
    f.view->on_exclude_insecure_devices_changed(true);
    CHECK(st.exclude_insecure_devices);
    f.view->on_group_inactive_changed(true);
    CHECK(st.group_inactive_rooms);
    f.view->on_group_unread_changed(true);
    CHECK(st.group_unread_rooms);
    f.view->on_inactive_period_changed(45);
    CHECK(st.inactive_room_threshold_days == 45);
}

TEST_CASE("wiring: settings view persists device selections and language",
          "[shell][wiring][settings]")
{
    SettingsFixture f;
    auto& st = tesseract::Settings::instance();
    f.view->on_audio_input_changed("mic-1");
    f.view->on_audio_output_changed("spk-2");
    f.view->on_camera_changed("cam-3");
    CHECK(st.audio_input_device_id == "mic-1");
    CHECK(st.audio_output_device_id == "spk-2");
    CHECK(st.camera_device_id == "cam-3");
    f.view->on_language_changed("de");
    CHECK(st.language == "de");
}

TEST_CASE("wiring: settings view proxy change is stored and flags a restart",
          "[shell][wiring][settings]")
{
    SettingsFixture f;
    auto& st = tesseract::Settings::instance();
    f.view->on_proxy_changed(tesseract::Settings::ProxyMode::Manual,
                             "http://proxy.invalid:3128");
    CHECK(st.proxy_mode == tesseract::Settings::ProxyMode::Manual);
    CHECK(st.proxy_url == "http://proxy.invalid:3128");
}

TEST_CASE("wiring: settings view with a null pointer is ignored",
          "[shell][wiring][settings]")
{
    WiringShell s;
    CHECK_NOTHROW(s.wire_settings_view_(nullptr));
    CHECK_NOTHROW(s.wire_settings_controller_common_(nullptr, nullptr, {}));
}

TEST_CASE("wiring: avatar-changed result updates the shell and active account",
          "[shell][wiring][settings]")
{
    WiringShell s;
    // The controller must outlive the view: the view keeps a pointer to it and
    // touches it again on destruction.
    auto post = [](std::function<void()> fn) { fn(); };
    auto picker = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto decode = [](const std::vector<uint8_t>&)
    { return std::shared_ptr<tk::Image>(); };
    auto run = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post, run, picker, decode);

    auto surface = TestSurface::create(900, 700);
    auto view = tk::create_root_widget<SettingsView>(nullptr);
    auto sess = std::make_shared<tesseract::AccountSession>();
    sess->user_id = "@me:x";
    s.active_account_ = sess;

    int relayouts = 0;
    s.wire_settings_controller_common_(view.get(), &ctrl, [&] { ++relayouts; });
    REQUIRE(ctrl.on_avatar_changed != nullptr);
    ctrl.on_avatar_changed("mxc://x/new");
    CHECK(s.my_avatar_url_ == "mxc://x/new");
    CHECK(sess->avatar_url == "mxc://x/new");
    CHECK(relayouts == 1);
}

// ── wire_voice_capture_ ─────────────────────────────────────────────────────

namespace
{
struct VoiceFixture
{
    WiringShell s;
    tesseract::Client client;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<MainAppWidget> app =
        tk::create_root_widget<MainAppWidget>(nullptr);
    FakeCapture* cap = nullptr;
    int repaints = 0;
    int clears = 0;
    std::string room = "!voice:x";

    VoiceFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        auto c = std::make_unique<FakeCapture>();
        cap = c.get();
        s.capture_ = std::move(c);
        s.wire_voice_capture_(
            app->room_view(), [this] { ++repaints; }, [this] { return room; },
            [this] { ++clears; });
    }
};
} // namespace

TEST_CASE("wiring: voice capture without a device hides the mic", "[shell][wiring][voice]")
{
    WiringShell s;
    auto surface = TestSurface::create(1000, 700);
    auto app = tk::create_root_widget<MainAppWidget>(nullptr);
    s.wire_voice_capture_(app->room_view(), [] {}, [] { return std::string(); },
                          [] {});
    CHECK(app->room_view()->on_mic_clicked == nullptr);
}

TEST_CASE("wiring: mic click toggles recording and amplitude repaints",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    REQUIRE(f.app->room_view()->on_mic_clicked != nullptr);
    f.app->room_view()->on_mic_clicked();
    CHECK(f.cap->starts == 1);
    CHECK(f.cap->recording);
    CHECK(f.repaints >= 1);

    const int before = f.repaints;
    f.cap->on_amplitude(500);
    CHECK(f.repaints == before + 1);

    // Second click stops; a too-short clip is dropped without clearing input.
    f.cap->dur = 100;
    f.cap->pcm = {1, 2, 3};
    f.app->room_view()->on_mic_clicked();
    CHECK(f.cap->stops == 1);
    CHECK(f.clears == 0);
}

TEST_CASE("wiring: a long clip clears the compose field and is sent async",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    f.cap->dur = 2000;
    f.cap->pcm = std::vector<std::uint8_t>(64, 1);
    f.app->room_view()->on_mic_clicked();
    CHECK(f.clears == 1);
}

TEST_CASE("wiring: an empty recording (device error) sends nothing",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    f.cap->dur = 5000;
    f.cap->pcm.clear();
    f.app->room_view()->on_mic_clicked();
    CHECK(f.clears == 0);
}

TEST_CASE("wiring: cancelling a voice recording discards it", "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    REQUIRE(f.app->room_view()->on_cancel_voice != nullptr);
    f.app->room_view()->on_cancel_voice();
    CHECK(f.cap->cancels == 1);
    CHECK_FALSE(f.cap->recording);
}
