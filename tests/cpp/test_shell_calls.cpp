// ShellBase_calls.cpp: call session lifecycle and the RTC event handlers the
// shells route into (participants, session end reasons, camera errors, the
// "call in progress" banner). The RTC FFI itself needs a live homeserver, so a
// session-less Client stands in: starting a call fails, and handler tests
// install a CallSession directly.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
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

using tesseract::CallSession;
using tesseract::ShellBase;

namespace
{

struct CallShell : tesseract::test::TestShellBase
{
    ~CallShell() override
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

    void on_show_status_message_ui_(const std::string& m) override
    {
        status.push_back(m);
    }
    std::vector<std::string> status;

    using ShellBase::call_auto_floated_;
    using ShellBase::call_banner_rosters_;
    using ShellBase::call_overlay_state_;
    using ShellBase::call_session_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::end_call;
    using ShellBase::handle_call_video_error_;
    using ShellBase::handle_rtc_participant_joined_ui_;
    using ShellBase::handle_rtc_participant_left_ui_;
    using ShellBase::handle_rtc_participant_updated_ui_;
    using ShellBase::handle_rtc_session_ended_ui_;
    using ShellBase::handle_rtc_video_frame_ui_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::overlay_mode_from_settings_;
    using ShellBase::refresh_call_banners_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::start_call;
};

tesseract::RtcParticipantInfo shell_calls_participant(const std::string& id)
{
    tesseract::RtcParticipantInfo p;
    p.participant_id = id;
    p.user_id = "@" + id + ":x";
    return p;
}

void shell_calls_install_session(CallShell& s, tesseract::Client& c, const std::string& room)
{
    s.client_ = &c;
    s.call_session_ = std::make_unique<CallSession>(&c, room, "slot");
}

} // namespace

TEST_CASE("start_call without a client does nothing", "[shell][calls]")
{
    CallShell s;
    s.start_call("!r:x", "slot", true);
    CHECK(s.call_session_ == nullptr);
    CHECK(s.status.empty());
}

TEST_CASE("start_call reports a failed join and leaves no session behind",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    s.client_ = &c;
    s.start_call("!r:x", "slot", true);
    CHECK(s.call_session_ == nullptr);
    REQUIRE(s.status.size() == 1);
    CHECK(s.status[0].find("Call failed") != std::string::npos);
}

TEST_CASE("start_call is ignored while a call is already active", "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!active:x");
    s.start_call("!other:x", "slot", false);
    CHECK(s.call_session_->room_id() == "!active:x");
    CHECK(s.status.empty());
}

TEST_CASE("participant events build the roster and pin the session id",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    // No call: ignored.
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    CHECK(s.call_session_ == nullptr);

    shell_calls_install_session(s, c, "!r:x");
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("b"));
    CHECK(s.call_session_->session_id() == 5);
    CHECK(s.call_session_->participants().size() == 2);

    // Events for another session are ignored.
    s.handle_rtc_participant_left_ui_(99, "a");
    s.handle_rtc_participant_updated_ui_(99, shell_calls_participant("zzz"));
    CHECK(s.call_session_->participants().size() == 2);

    auto muted = shell_calls_participant("a");
    muted.is_audio_muted = true;
    s.handle_rtc_participant_updated_ui_(5, muted);
    CHECK(s.call_session_->participants()[0].is_audio_muted);

    s.handle_rtc_participant_left_ui_(5, "a");
    REQUIRE(s.call_session_->participants().size() == 1);
    CHECK(s.call_session_->participants()[0].participant_id == "b");
}

TEST_CASE("a session end for another call is ignored", "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x");
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    s.handle_rtc_session_ended_ui_(77, "hangup");
    CHECK(s.call_session_ != nullptr);
}

TEST_CASE("a session end before any participant joined is accepted",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x");
    s.handle_rtc_session_ended_ui_(42, "hangup");
    CHECK(s.call_session_ == nullptr);
    CHECK(s.status.empty()); // normal end reasons are silent
}

TEST_CASE("normal end reasons are silent; failures are explained",
          "[shell][calls]")
{
    struct Case
    {
        const char* reason;
        bool silent;
        const char* fragment;
    };
    const Case cases[] = {
        {"hangup", true, ""},
        {"user_action", true, ""},
        {"switch_device", true, ""},
        {"", true, ""},
        {"ice_failed", false, "network"},
        {"dtls_failed", false, "network"},
        {"network_error", false, "network"},
        {"media_error", false, "media"},
        {"transport_failure", false, "media"},
        {"codec_mismatch", false, "not supported"},
        {"unsupported_features", false, "not supported"},
        {"encryption_error", false, "encryption"},
        {"something_new", false, "unexpectedly"},
    };
    for (const auto& c : cases)
    {
        tesseract::Client client;
        CallShell s;
        shell_calls_install_session(s, client, "!r:x");
        s.handle_rtc_session_ended_ui_(0, c.reason);
        INFO(c.reason);
        CHECK(s.call_session_ == nullptr);
        if (c.silent)
        {
            CHECK(s.status.empty());
        }
        else
        {
            REQUIRE(s.status.size() == 1);
            CHECK(s.status[0].find(c.fragment) != std::string::npos);
        }
    }
}

TEST_CASE("a session end with no session is a no-op", "[shell][calls]")
{
    CallShell s;
    CHECK_NOTHROW(s.handle_rtc_session_ended_ui_(1, "hangup"));
    CHECK_NOTHROW(s.handle_rtc_video_frame_ui_(
        1, "p", 1, 1, std::make_shared<std::vector<std::uint8_t>>(4)));
}

TEST_CASE("end_call tears the session down and resets overlay state",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x");
    s.call_auto_floated_ = true;
    s.call_overlay_state_.audio_muted = true;
    s.end_call();
    CHECK(s.call_session_ == nullptr);
    CHECK_FALSE(s.call_auto_floated_);
    CHECK_FALSE(s.call_overlay_state_.audio_muted);
    CHECK_NOTHROW(s.end_call()); // idempotent
}

TEST_CASE("a camera error mutes video and tells the user", "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    // No call: ignored.
    s.handle_call_video_error_(tk::VideoCapture::Error::Busy);
    CHECK(s.status.empty());

    shell_calls_install_session(s, c, "!r:x");
    s.handle_call_video_error_(tk::VideoCapture::Error::Busy);
    CHECK(s.call_overlay_state_.video_muted);
    REQUIRE(s.status.size() == 1);
    CHECK_FALSE(s.status[0].empty());
}

TEST_CASE("overlay mode follows the saved setting", "[shell][calls]")
{
    tesseract::test::SettingsGuard guard;
    CallShell s;
    using SM = tesseract::Settings::CallOverlayMode;
    using OM = tesseract::views::CallOverlayWidget::Mode;
    auto& st = tesseract::Settings::instance();
    st.call_overlay_mode = SM::DockedExpanded;
    CHECK(s.overlay_mode_from_settings_() == OM::DockedExpanded);
    st.call_overlay_mode = SM::Floating;
    CHECK(s.overlay_mode_from_settings_() == OM::Floating);
    st.call_overlay_mode = SM::Popout;
    CHECK(s.overlay_mode_from_settings_() == OM::Popout);
    st.call_overlay_mode = SM::Docked;
    CHECK(s.overlay_mode_from_settings_() == OM::Docked);
}

namespace
{
struct BannerFixture
{
    tesseract::Client client; // outlives the shell
    CallShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);

    BannerFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        s.room_view_ = app->room_view();
        s.current_room_id_ = "!call:x";
        tesseract::RoomInfo r;
        r.id = "!call:x";
        r.name = "Call room";
        r.call_members = {"@alice:x"};
        s.rooms_.push_back(r);
        s.server_info_.supports_calls = true;
    }
};
} // namespace

TEST_CASE("an in-progress call shows the join banner in the room",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.refresh_call_banners_();
    CHECK(f.app->room_view()->call_banner_visible());
    REQUIRE(f.s.call_banner_rosters_.count("!call:x") == 1);
    // Display names fall back to the localpart when members are unknown.
    CHECK(f.s.call_banner_rosters_["!call:x"].names[0].second == "alice");
}

TEST_CASE("the banner is hidden when calls are unsupported or the room is empty",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.refresh_call_banners_();
    REQUIRE(f.app->room_view()->call_banner_visible());

    f.s.server_info_.supports_calls = false;
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_visible());
    CHECK(f.s.call_banner_rosters_.count("!call:x") == 0);

    f.s.server_info_.supports_calls = true;
    f.s.rooms_[0].call_members.clear();
    f.s.mark_room_index_dirty_();
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_visible());
}

TEST_CASE("the banner is hidden for a bridged room unless overridden",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.rooms_[0].is_bridged = true;
    f.s.mark_room_index_dirty_();
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_visible());
    f.s.rooms_[0].bridge_overridden = true;
    f.s.mark_room_index_dirty_();
    f.s.refresh_call_banners_();
    CHECK(f.app->room_view()->call_banner_visible());
}

TEST_CASE("the banner is hidden in the room the user is already calling from",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.call_session_ =
        std::make_unique<CallSession>(&f.client, "!call:x", "slot");
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_visible());
}
