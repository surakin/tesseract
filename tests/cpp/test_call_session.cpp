// CallSession (client/src/CallSession.cpp): per-call participant roster plus
// call-control forwarding. Driven with a session-less Client, whose RTC FFI
// calls are harmless no-ops / failures.

#include <catch2/catch_test_macros.hpp>

#include <tesseract/call_session.h>
#include <tesseract/client.h>

#include <string>

using tesseract::CallSession;
using tesseract::RtcParticipantInfo;

namespace
{
RtcParticipantInfo call_session_participant(const std::string& id, bool audio_muted = false)
{
    RtcParticipantInfo p;
    p.participant_id = id;
    p.user_id = "@" + id + ":x";
    p.is_audio_muted = audio_muted;
    return p;
}
} // namespace

TEST_CASE("CallSession exposes its room and slot", "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!room:x", "slot-1");
    CHECK(s.room_id() == "!room:x");
    CHECK(s.slot_id() == "slot-1");
    CHECK(s.session_id() == 0);
    CHECK(s.participants().empty());
}

TEST_CASE("CallSession session id is set once", "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!r:x", "s");
    s.set_session_id(7);
    s.set_session_id(9);
    CHECK(s.session_id() == 7);
}

TEST_CASE("CallSession tracks joins, updates and leaves", "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!r:x", "s");
    s.on_participant_joined(call_session_participant("a"));
    s.on_participant_joined(call_session_participant("b"));
    REQUIRE(s.participants().size() == 2);

    s.on_participant_updated(call_session_participant("a", true));
    auto ps = s.participants();
    REQUIRE(ps.size() == 2);
    CHECK(ps[0].is_audio_muted);
    CHECK_FALSE(ps[1].is_audio_muted);

    s.on_participant_left("a");
    ps = s.participants();
    REQUIRE(ps.size() == 1);
    CHECK(ps[0].participant_id == "b");

    // Leaving an unknown participant is a no-op.
    s.on_participant_left("nobody");
    CHECK(s.participants().size() == 1);
}

TEST_CASE("CallSession update for an untracked participant adds it",
          "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!r:x", "s");
    s.on_participant_updated(call_session_participant("late"));
    REQUIRE(s.participants().size() == 1);
    CHECK(s.participants()[0].participant_id == "late");
}

TEST_CASE("CallSession end clears the roster and blocks further controls",
          "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!r:x", "s");
    s.on_participant_joined(call_session_participant("a"));
    s.on_session_ended("remote hung up");
    CHECK(s.participants().empty());

    CHECK_NOTHROW(s.mute_audio(true));
    CHECK_NOTHROW(s.mute_video(true));
    CHECK_NOTHROW(s.stop_screen_share());
    auto r = s.start_screen_share();
    CHECK_FALSE(r.ok);
    CHECK(r.message == "call has ended");
    CHECK_NOTHROW(s.hang_up());
}

TEST_CASE("CallSession controls forward while the call is live",
          "[call_session]")
{
    tesseract::Client client;
    CallSession s(&client, "!r:x", "s");
    CHECK_NOTHROW(s.mute_audio(true));
    CHECK_NOTHROW(s.mute_video(false));
    CHECK_NOTHROW(s.stop_screen_share());
    // No active call on a session-less client: the share request is refused
    // by the SDK rather than reported as "call has ended".
    auto r = s.start_screen_share();
    CHECK(r.message != "call has ended");
    CHECK_NOTHROW(s.hang_up());
    CHECK_NOTHROW(s.hang_up()); // idempotent
}
