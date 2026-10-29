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

    using ShellBase::call_account_;
    using ShellBase::call_auto_floated_;
    using ShellBase::call_banner_rosters_;
    using ShellBase::call_invite_decline_targets_;
    using ShellBase::call_overlay_state_;
    using ShellBase::call_session_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::end_call;
    using ShellBase::event_account_;
    using ShellBase::handle_call_video_error_;
    using ShellBase::decline_pending_call_invite_;
    using ShellBase::handle_rtc_invitation_ui_;
    using ShellBase::handle_rtc_participant_joined_ui_;
    using ShellBase::handle_rtc_participant_left_ui_;
    using ShellBase::handle_rtc_participant_updated_ui_;
    using ShellBase::handle_rtc_session_ended_ui_;
    using ShellBase::handle_rtc_video_frame_ui_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::overlay_mode_from_settings_;
    using ShellBase::PendingCallInvite;
    using ShellBase::pending_call_invites_;
    using ShellBase::refresh_call_banners_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::server_info_;
    using ShellBase::start_call;
    using ShellBase::CallInviteKey;
    using ShellBase::EventAccountScope;
    using ShellBase::call_banner_ids_;
    using ShellBase::call_invites_shown_by_banner_;
    using ShellBase::drop_call_invites_of_account_;
    using ShellBase::drop_call_invites_of_missing_rooms_;
    using ShellBase::kMaxPendingCallInvites;
    using ShellBase::push_rooms_;
    using ShellBase::trim_pending_call_invites_;

    // The active account's invites for one room (creating the entry).
    std::vector<PendingCallInvite>& invites(const std::string& room)
    {
        return pending_call_invites_[CallInviteKey{my_user_id_, room}];
    }
    std::size_t invite_entries_of(const std::string& account, const std::string& room) const
    {
        return pending_call_invites_.count(CallInviteKey{account, room});
    }
};

tesseract::RtcParticipantInfo shell_calls_participant(const std::string& id)
{
    tesseract::RtcParticipantInfo p;
    p.participant_id = id;
    p.user_id = "@" + id + ":x";
    return p;
}

void shell_calls_install_session(CallShell& s, tesseract::Client& c, const std::string& room,
                                 std::uint64_t session_id = 5)
{
    s.client_ = &c;
    s.call_session_ = std::make_unique<CallSession>(&c, room, "slot", session_id);
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

TEST_CASE("participant events build the roster of the call they belong to",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    // No call: ignored.
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    CHECK(s.call_session_ == nullptr);

    shell_calls_install_session(s, c, "!r:x");
    CHECK(s.call_session_->session_id() == 5); // known from the start
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("b"));
    CHECK(s.call_session_->participants().size() == 2);

    // Events for another session, or for none, are ignored.
    s.handle_rtc_participant_left_ui_(99, "a");
    s.handle_rtc_participant_left_ui_(0, "a");
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

TEST_CASE("a join for another session is ignored", "[shell][calls]")
{
    tesseract::Client c;
    CallShell s;
    shell_calls_install_session(s, c, "!r:x");
    s.handle_rtc_participant_joined_ui_(9, shell_calls_participant("late"));
    s.handle_rtc_participant_joined_ui_(0, shell_calls_participant("none"));
    CHECK(s.call_session_->session_id() == 5);
    CHECK(s.call_session_->participants().empty());
}

TEST_CASE("a session end for another call, or for none, is ignored", "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x");
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("a"));
    s.handle_rtc_session_ended_ui_(77, "hangup");
    s.handle_rtc_session_ended_ui_(0, "something_new");
    CHECK(s.call_session_ != nullptr);
    CHECK(s.status.empty());
}

TEST_CASE("a session end before any participant joined is accepted",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x", 42);
    s.handle_rtc_session_ended_ui_(42, "hangup");
    CHECK(s.call_session_ == nullptr);
    CHECK(s.status.empty()); // normal end reasons are silent
}

TEST_CASE("a late end of a hung-up call does not end the call started after it",
          "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x", 5);
    s.end_call(); // hang up
    // Join again at once: a new call, a new id, no participant yet.
    shell_calls_install_session(s, c, "!r:x", 6);
    // The old call's disconnect arrives now.
    s.handle_rtc_session_ended_ui_(5, "ClientInitiated");
    REQUIRE(s.call_session_ != nullptr);
    CHECK(s.call_session_->session_id() == 6);
    CHECK(s.status.empty()); // no "ended unexpectedly"
    // The new call's own end still works.
    s.handle_rtc_session_ended_ui_(6, "ClientInitiated");
    CHECK(s.call_session_ == nullptr);
}

TEST_CASE("an end tagged with another account is not this call's", "[shell][calls]")
{
    tesseract::Client c; // outlives the shell (its CallSession hangs up on it)
    CallShell s;
    shell_calls_install_session(s, c, "!r:x", 5);
    auto account = std::make_shared<tesseract::AccountSession>();
    account->user_id = "@a:x";
    s.call_account_ = account;

    s.event_account_ = "@b:x";
    s.handle_rtc_session_ended_ui_(5, "hangup");
    s.handle_rtc_participant_joined_ui_(5, shell_calls_participant("p"));
    CHECK(s.call_session_ != nullptr);
    CHECK(s.call_session_->participants().empty());

    s.event_account_ = "@a:x";
    s.handle_rtc_session_ended_ui_(5, "hangup");
    CHECK(s.call_session_ == nullptr);
    s.event_account_.clear();
}

TEST_CASE("a call that could not be set up ends with the reason shown",
          "[shell][calls]")
{
    tesseract::Client c;
    CallShell s;
    shell_calls_install_session(s, c, "!r:x", 8);
    s.handle_rtc_session_ended_ui_(8, std::string(tesseract::kRtcStartFailedReason) + "room not found");
    CHECK(s.call_session_ == nullptr);
    REQUIRE(s.status.size() == 1);
    CHECK(s.status[0] == "Call failed: room not found");
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
        s.handle_rtc_session_ended_ui_(5, c.reason);
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

TEST_CASE("an active invite adds Decline to the call banner", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    CHECK(f.app->room_view()->call_banner_visible());
    CHECK(f.app->room_view()->call_banner_decline_visible());
}

TEST_CASE("declining clears the pending invite and the Decline button",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    REQUIRE(f.s.invite_entries_of("", "!call:x") == 1);
    f.s.decline_pending_call_invite_("!call:x");
    CHECK(f.s.pending_call_invites_.empty());
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    // The call itself is still in progress; only the invite is gone.
    CHECK(f.app->room_view()->call_banner_visible());
}

TEST_CASE("a withdrawn invite removes the Decline button", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    REQUIRE(f.app->room_view()->call_banner_decline_visible());
    // A zero lifetime with an event id means the invite is no longer active.
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$another");
    CHECK(f.app->room_view()->call_banner_decline_visible());
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$inv");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    CHECK(f.s.pending_call_invites_.empty());
    CHECK(f.app->room_view()->call_banner_visible());
}

TEST_CASE("a DM call's legacy ring and MatrixRTC invite are tracked separately",
          "[shell][calls][banner]")
{
    BannerFixture f;
    // One DM call, two signals: the legacy ring ($ring) and the sticky invite ($inv).
    f.s.handle_rtc_invitation_ui_("!call:x", "call#default", "@alice:x", "video", 60000, "$ring");
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    REQUIRE(f.s.invites("!call:x").size() == 2);

    // Withdrawing the MatrixRTC invite must not clear the banner of the ring...
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$inv");
    CHECK(f.app->room_view()->call_banner_decline_visible());
    REQUIRE(f.s.invites("!call:x").size() == 1);
    CHECK(f.s.invites("!call:x")[0].event_id == "$ring");

    // ...and the ring's own cancellation finishes the job.
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$ring");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    CHECK(f.s.pending_call_invites_.empty());
}

TEST_CASE("cancelling the legacy ring leaves the MatrixRTC invite pending",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    f.s.handle_rtc_invitation_ui_("!call:x", "call#default", "@alice:x", "video", 60000, "$ring");
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$ring");
    CHECK(f.app->room_view()->call_banner_decline_visible());
    REQUIRE(f.s.invites("!call:x").size() == 1);
    CHECK(f.s.invites("!call:x")[0].event_id == "$inv");
}

TEST_CASE("announcing an invite again refreshes it instead of adding a second",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 1000, "$inv");
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    REQUIRE(f.s.invites("!call:x").size() == 1);
    CHECK(f.s.invites("!call:x")[0].expires >
          std::chrono::steady_clock::now() + std::chrono::seconds(30));
}

TEST_CASE("an expired invite leaves the other pending one alone", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "call#default", "@alice:x", "video", 60000, "$ring");
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    f.s.invites("!call:x")[0].expires =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    f.s.refresh_call_banners_();
    REQUIRE(f.s.invites("!call:x").size() == 1);
    CHECK(f.s.invites("!call:x")[0].event_id == "$inv");
    CHECK(f.app->room_view()->call_banner_decline_visible());
}

TEST_CASE("the banner lists every caller with a pending invite", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.rooms_[0].call_members.clear();
    f.s.mark_room_index_dirty_();
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "audio", 60000, "$a");
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@bob:x", "audio", 60000, "$b");
    f.s.handle_rtc_invitation_ui_("!call:x", "call#default", "@alice:x", "audio", 60000, "$a2");
    REQUIRE(f.s.call_banner_rosters_.count("!call:x") == 1);
    CHECK(f.s.call_banner_rosters_["!call:x"].ids ==
          std::vector<std::string>{"@alice:x", "@bob:x"});
}

TEST_CASE("declining answers the MatrixRTC invite first and the legacy ring too",
          "[shell][calls][banner]")
{
    using Pending = CallShell::PendingCallInvite;
    const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const std::vector<Pending> both{{"$ring", "call#default", "@alice:x", later},
                                    {"$inv", "m.call#room", "@alice:x", later}};
    CHECK(CallShell::call_invite_decline_targets_(both) ==
          std::vector<std::string>{"$inv", "$ring"});
    // A ring with no MatrixRTC invite beside it is still an m.rtc.notification
    // event a decline can reference.
    CHECK(CallShell::call_invite_decline_targets_({both[0]}) ==
          std::vector<std::string>{"$ring"});
    // Nothing without an id; the same id never twice.
    const std::vector<Pending> odd{{"", "m.call#room", "@a:x", later},
                                   {"$x", "m.call#room", "@a:x", later},
                                   {"$x", "m.call#room", "@b:x", later}};
    CHECK(CallShell::call_invite_decline_targets_(odd) == std::vector<std::string>{"$x"});
    CHECK(CallShell::call_invite_decline_targets_({}).empty());
}

TEST_CASE("a call member event without an event id leaves invites alone", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    f.s.handle_rtc_invitation_ui_("!call:x", "", "@alice:x", "", 0, "");
    CHECK(f.app->room_view()->call_banner_decline_visible());
}

TEST_CASE("showing or hiding Decline relayouts the room view", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.refresh_call_banners_(); // the banner is up without an invite
    int layouts = 0;
    f.app->room_view()->on_layout_changed = [&] { ++layouts; };
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    CHECK(layouts >= 1);
    const int after_show = layouts;
    f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$inv");
    CHECK(layouts > after_show);
}

TEST_CASE("an invite without an event id or lifetime offers no Decline",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 0, "$inv");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    CHECK(f.s.pending_call_invites_.empty());
}

TEST_CASE("an expired invite no longer offers Decline", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    f.s.invites("!call:x").front().expires =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    CHECK(f.s.pending_call_invites_.empty());
}

TEST_CASE("an invite shows the banner before the call roster has members",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.rooms_[0].call_members.clear();
    f.s.mark_room_index_dirty_();
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "audio", 60000, "$inv");
    CHECK(f.app->room_view()->call_banner_visible());
    CHECK(f.app->room_view()->call_banner_decline_visible());
    REQUIRE(f.s.call_banner_rosters_.count("!call:x") == 1);
    CHECK(f.s.call_banner_rosters_["!call:x"].names[0].second == "alice");
}

TEST_CASE("joining the call drops the pending invite", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$inv");
    shell_calls_install_session(f.s, f.client, "!call:x");
    f.s.refresh_call_banners_();
    CHECK(f.s.pending_call_invites_.empty());
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
        std::make_unique<CallSession>(&f.client, "!call:x", "slot", 3);
    f.s.refresh_call_banners_();
    CHECK_FALSE(f.app->room_view()->call_banner_visible());
}

TEST_CASE("Decline answers only the invites whose caller the banner lists",
          "[shell][calls][banner]")
{
    BannerFixture f; // the room's call has @alice:x in it
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$alice");
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@bob:x", "video", 60000, "$bob");
    REQUIRE(f.app->room_view()->call_banner_decline_visible());
    f.s.decline_pending_call_invite_("!call:x");
    // Bob's invite was never part of the banner the user clicked in.
    REQUIRE(f.s.invites("!call:x").size() == 1);
    CHECK(f.s.invites("!call:x")[0].event_id == "$bob");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
}

TEST_CASE("an invite from a caller the banner does not list offers no Decline",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@bob:x", "video", 60000, "$bob");
    CHECK_FALSE(f.app->room_view()->call_banner_decline_visible());
    f.s.decline_pending_call_invite_("!call:x");
    CHECK(f.s.invites("!call:x").size() == 1);
}

TEST_CASE("with no call members the banner lists every inviting caller",
          "[shell][calls][banner]")
{
    using Pending = CallShell::PendingCallInvite;
    const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const std::vector<Pending> both{{"$a", "m.call#room", "@alice:x", later},
                                    {"$b", "m.call#room", "@bob:x", later},
                                    {"$a2", "call#default", "@alice:x", later}};
    const auto ids = CallShell::call_banner_ids_(nullptr, both);
    CHECK(ids == std::vector<std::string>{"@alice:x", "@bob:x"});
    CHECK(CallShell::call_invites_shown_by_banner_(ids, both).size() == 3);
    tesseract::RoomInfo r;
    r.call_members = {"@carol:x"};
    CHECK(CallShell::call_banner_ids_(&r, both) == std::vector<std::string>{"@carol:x"});
    CHECK(CallShell::call_invites_shown_by_banner_({"@carol:x"}, both).empty());
}

TEST_CASE("two accounts in one room keep separate invites", "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.my_user_id_ = "@me:x";
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$mine");
    {
        CallShell::EventAccountScope other(f.s, "@other:x");
        f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$theirs");
    }
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 1);
    CHECK(f.s.invite_entries_of("@other:x", "!call:x") == 1);
    // Declining as the active account leaves the other account's invite alone.
    f.s.decline_pending_call_invite_("!call:x");
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 0);
    CHECK(f.s.invite_entries_of("@other:x", "!call:x") == 1);
    // A cancellation of the other account's invite does not touch ours either.
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$mine");
    {
        CallShell::EventAccountScope other(f.s, "@other:x");
        f.s.handle_rtc_invitation_ui_("!call:x", "", "", "", 0, "$mine");
    }
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 1);
}

TEST_CASE("signing an account out forgets its invites and the banner rosters",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.my_user_id_ = "@me:x";
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$mine");
    {
        CallShell::EventAccountScope other(f.s, "@other:x");
        f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$theirs");
    }
    REQUIRE(f.s.call_banner_rosters_.count("!call:x") == 1);
    f.s.drop_call_invites_of_account_("@other:x");
    CHECK(f.s.invite_entries_of("@other:x", "!call:x") == 0);
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 1);
    CHECK(f.s.call_banner_rosters_.empty());
}

TEST_CASE("invites of a room the account no longer has are dropped",
          "[shell][calls][banner]")
{
    BannerFixture f;
    f.s.my_user_id_ = "@me:x";
    f.s.handle_rtc_invitation_ui_("!call:x", "m.call#room", "@alice:x", "video", 60000, "$a");
    f.s.handle_rtc_invitation_ui_("!kept:x", "m.call#room", "@alice:x", "video", 60000, "$b");
    REQUIRE(f.s.call_banner_rosters_.count("!call:x") == 1);
    tesseract::RoomInfo kept;
    kept.id = "!kept:x";
    // Another account's room list changes nothing for ours.
    f.s.drop_call_invites_of_missing_rooms_("@other:x", {});
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 1);
    f.s.drop_call_invites_of_missing_rooms_("@me:x", {kept});
    CHECK(f.s.invite_entries_of("@me:x", "!call:x") == 0);
    CHECK(f.s.invite_entries_of("@me:x", "!kept:x") == 1);
    CHECK(f.s.call_banner_rosters_.count("!call:x") == 0);
}

TEST_CASE("pending invites are capped across rooms, dropping the soonest to expire",
          "[shell][calls][banner]")
{
    CallShell s;
    const std::size_t cap = CallShell::kMaxPendingCallInvites;
    const std::size_t sent = cap + 8;
    for (std::size_t n = 0; n < sent; ++n)
    {
        // Each later invite lives a little longer than the one before.
        s.handle_rtc_invitation_ui_("!room" + std::to_string(n) + ":x", "m.call#room",
                                    "@c" + std::to_string(n) + ":x", "video",
                                    60000 + n * 1000, "$i" + std::to_string(n));
    }
    std::size_t total = 0;
    for (const auto& [key, list] : s.pending_call_invites_)
    {
        total += list.size();
    }
    CHECK(total == cap);
    CHECK(s.invite_entries_of("", "!room0:x") == 0);
    CHECK(s.invite_entries_of("", "!room" + std::to_string(sent - 1) + ":x") == 1);
}
