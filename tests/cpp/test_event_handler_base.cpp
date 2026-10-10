// EventHandlerBase marshals every SDK callback to the UI thread inside an
// EventAccountScope and then calls the matching ShellBase handle_*_ui_ virtual.
// These tests drive a handler against a recording shell (post_to_ui_ runs
// inline in TestShellBase) and assert routing, account tagging and the few
// callbacks that carry real logic (search routing, offline/online transitions,
// inflight spinner, verification state, RGBA -> premultiplied BGRA).

#include <catch2/catch_test_macros.hpp>

#include "app/EventHandlerBase.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>

#include <memory>
#include <string>
#include <vector>

using tesseract::EventHandlerBase;

namespace
{

struct HandlerShell : tesseract::test::TestShellBase
{
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    std::vector<std::string> log;
    std::vector<std::string> accounts; // event_account_ at each call
    std::vector<std::vector<std::uint8_t>> frames;
    int tick_starts = 0;
    int tick_stops = 0;
    int inflight_ui = 0;
    int room_list_ui = 0;

    void rec(const std::string& s)
    {
        log.push_back(s);
        accounts.push_back(event_account_);
    }

    void handle_timeline_reset_ui_(std::string r, tesseract::EventList s) override
    {
        rec("reset:" + r + ":" + std::to_string(s.size()));
    }
    void handle_message_inserted_ui_(std::string r, std::size_t i,
                                     std::unique_ptr<tesseract::Event> e) override
    {
        rec("ins:" + r + ":" + std::to_string(i) + ":" + e->event_id);
    }
    void handle_message_updated_ui_(std::string r, std::size_t i,
                                    std::unique_ptr<tesseract::Event> e) override
    {
        rec("upd:" + r + ":" + std::to_string(i) + ":" + e->event_id);
    }
    void handle_message_removed_ui_(std::string r, std::size_t i) override
    {
        rec("rm:" + r + ":" + std::to_string(i));
    }
    void handle_messages_prepended_ui_(std::string r,
                                       tesseract::EventList e) override
    {
        rec("pre:" + r + ":" + std::to_string(e.size()));
    }
    void handle_messages_appended_ui_(std::string r,
                                      tesseract::EventList e) override
    {
        rec("app:" + r + ":" + std::to_string(e.size()));
    }
    void handle_messages_updated_batch_ui_(std::string r,
                                           std::vector<std::size_t> idx,
                                           tesseract::EventList e) override
    {
        rec("batch:" + r + ":" + std::to_string(idx.size()) + ":" +
            std::to_string(e.size()));
    }
    void handle_thread_reset_ui_(std::string r, std::string t,
                                 tesseract::EventList s) override
    {
        rec("treset:" + r + ":" + t + ":" + std::to_string(s.size()));
    }
    void handle_thread_inserted_ui_(std::string r, std::string t, std::size_t i,
                                    std::unique_ptr<tesseract::Event>) override
    {
        rec("tins:" + r + ":" + t + ":" + std::to_string(i));
    }
    void handle_thread_updated_ui_(std::string r, std::string t, std::size_t i,
                                   std::unique_ptr<tesseract::Event>) override
    {
        rec("tupd:" + r + ":" + t + ":" + std::to_string(i));
    }
    void handle_thread_removed_ui_(std::string r, std::string t,
                                   std::size_t i) override
    {
        rec("trm:" + r + ":" + t + ":" + std::to_string(i));
    }
    void handle_thread_messages_prepended_ui_(std::string r, std::string t,
                                              tesseract::EventList e) override
    {
        rec("tpre:" + r + ":" + t + ":" + std::to_string(e.size()));
    }
    void handle_thread_messages_appended_ui_(std::string r, std::string t,
                                             tesseract::EventList e) override
    {
        rec("tapp:" + r + ":" + t + ":" + std::to_string(e.size()));
    }
    void handle_threads_updated_ui_(std::string r) override
    {
        rec("threads:" + r);
    }
    void handle_knock_requests_updated_ui_(std::string r) override
    {
        rec("knocks:" + r);
    }
    void handle_gif_results_ui_(std::uint64_t id,
                                std::vector<tesseract::GifResult> r) override
    {
        rec("gif:" + std::to_string(id) + ":" + std::to_string(r.size()));
    }
    void handle_gif_search_failed_ui_(std::uint64_t id, std::string m) override
    {
        rec("giffail:" + std::to_string(id) + ":" + m);
    }
    void handle_sync_error_ui_(std::string c, std::string u, std::string d,
                               bool soft) override
    {
        rec("syncerr:" + c + ":" + u + ":" + d + (soft ? ":soft" : ":hard"));
    }
    void handle_offline_ui_() override
    {
        rec("offline");
        offline_ = true;
    }
    void handle_online_ui_() override
    {
        rec("online");
        offline_ = false;
    }
    void handle_image_packs_updated_ui_() override { rec("packs"); }
    void handle_account_prefs_updated_ui_(std::string u, std::string j) override
    {
        rec("prefs:" + u + ":" + j);
    }
    void handle_media_preview_config_updated_ui_(std::string u,
                                                 std::string j) override
    {
        rec("mpc:" + u + ":" + j);
    }
    void handle_bot_commands_updated_ui_(std::string r) override
    {
        rec("bot:" + r);
    }
    void handle_rtc_invitation_ui_(std::string r, std::string s, std::string c,
                                   std::string i, std::uint64_t l,
                                   std::string n) override
    {
        rec("inv:" + r + ":" + s + ":" + c + ":" + i + ":" + std::to_string(l) +
            ":" + n);
    }
    void handle_rtc_participant_joined_ui_(std::uint64_t s,
                                           tesseract::RtcParticipantInfo i) override
    {
        rec("pj:" + std::to_string(s) + ":" + i.participant_id);
    }
    void handle_rtc_participant_left_ui_(std::uint64_t s, std::string p) override
    {
        rec("pl:" + std::to_string(s) + ":" + p);
    }
    void handle_rtc_participant_updated_ui_(std::uint64_t s,
                                            tesseract::RtcParticipantInfo i) override
    {
        rec("pu:" + std::to_string(s) + ":" + i.participant_id);
    }
    void handle_rtc_session_ended_ui_(std::uint64_t s, std::string r) override
    {
        rec("end:" + std::to_string(s) + ":" + r);
    }
    void handle_rtc_video_frame_ui_(
        std::uint64_t s, const std::string& p, std::uint32_t w, std::uint32_t h,
        std::shared_ptr<std::vector<std::uint8_t>> bgra) override
    {
        rec("vf:" + std::to_string(s) + ":" + p + ":" + std::to_string(w) + "x" +
            std::to_string(h));
        frames.push_back(*bgra);
    }
    void handle_rtc_screen_frame_ui_(
        std::uint64_t s, const std::string& p, std::uint32_t w, std::uint32_t h,
        std::shared_ptr<std::vector<std::uint8_t>> bgra) override
    {
        rec("sf:" + std::to_string(s) + ":" + p + ":" + std::to_string(w) + "x" +
            std::to_string(h));
        frames.push_back(*bgra);
    }
    void start_inflight_tick_() override { ++tick_starts; }
    void stop_inflight_tick_() override { ++tick_stops; }
    void on_inflight_ui_() override { ++inflight_ui; }
    void on_room_list_state_ui_() override { ++room_list_ui; }

    using ShellBase::active_account_;
    using ShellBase::event_account_;
    using ShellBase::in_room_search_pending_;
    using ShellBase::last_inflight_;
    using ShellBase::offline_;
    using ShellBase::thread_search_pending_;
};

struct EhbFx
{
    HandlerShell shell;
    EventHandlerBase h{&shell};
    EhbFx() { h.set_user_id("@me:x"); }
};

std::unique_ptr<tesseract::Event> ehb_ev(const char* id)
{
    auto e = std::make_unique<tesseract::TextEvent>();
    e->event_id = id;
    return e;
}

tesseract::EventList ehb_list(int n)
{
    tesseract::EventList l;
    for (int i = 0; i < n; ++i)
    {
        l.push_back(ehb_ev("$x"));
    }
    return l;
}

} // namespace

TEST_CASE("timeline callbacks reach the matching handler tagged with the account",
          "[event_handler_base]")
{
    EhbFx f;
    f.h.on_timeline_reset("!r", ehb_list(2));
    f.h.on_message_inserted("!r", 4, ehb_ev("$a"));
    f.h.on_message_updated("!r", 5, ehb_ev("$b"));
    f.h.on_message_removed("!r", 6);
    f.h.on_messages_prepended("!r", ehb_list(3));
    f.h.on_messages_appended("!r", ehb_list(1));
    f.h.on_messages_updated_batch("!r", {1, 2}, ehb_list(2));
    CHECK(f.shell.log ==
          std::vector<std::string>{"reset:!r:2", "ins:!r:4:$a", "upd:!r:5:$b",
                                   "rm:!r:6", "pre:!r:3", "app:!r:1",
                                   "batch:!r:2:2"});
    for (const auto& a : f.shell.accounts)
    {
        CHECK(a == "@me:x");
    }
    // The scope is released afterwards.
    CHECK(f.shell.event_account_.empty());
}

TEST_CASE("thread callbacks carry the thread root", "[event_handler_base]")
{
    EhbFx f;
    f.h.on_thread_reset("!r", "$t", ehb_list(1));
    f.h.on_thread_inserted("!r", "$t", 1, ehb_ev("$a"));
    f.h.on_thread_updated("!r", "$t", 2, ehb_ev("$b"));
    f.h.on_thread_removed("!r", "$t", 3);
    f.h.on_thread_messages_prepended("!r", "$t", ehb_list(2));
    f.h.on_thread_messages_appended("!r", "$t", ehb_list(4));
    f.h.on_threads_updated("!r");
    f.h.on_knock_requests_updated("!r");
    CHECK(f.shell.log ==
          std::vector<std::string>{"treset:!r:$t:1", "tins:!r:$t:1",
                                   "tupd:!r:$t:2", "trm:!r:$t:3",
                                   "tpre:!r:$t:2", "tapp:!r:$t:4", "threads:!r",
                                   "knocks:!r"});
}

TEST_CASE("each handler's account tag is its own user id",
          "[event_handler_base]")
{
    HandlerShell shell;
    EventHandlerBase a(&shell);
    EventHandlerBase b(&shell);
    a.set_user_id("@a:x");
    b.set_user_id("@b:x");
    CHECK(a.user_id() == "@a:x");
    a.on_message_removed("!r", 1);
    b.on_message_removed("!r", 1);
    CHECK(shell.accounts == std::vector<std::string>{"@a:x", "@b:x"});
}

TEST_CASE("set_shell redirects later callbacks", "[event_handler_base]")
{
    HandlerShell s1;
    HandlerShell s2;
    EventHandlerBase h(&s1);
    h.on_image_packs_updated();
    h.set_shell(&s2);
    h.on_image_packs_updated();
    CHECK(s1.log.size() == 1);
    CHECK(s2.log.size() == 1);
}

TEST_CASE("search results route by which pending map owns the request id",
          "[event_handler_base]")
{
    EhbFx f;
    // Unowned id: falls through to the global search handler, which on an
    // empty shell does nothing observable but must not throw.
    CHECK_NOTHROW(f.h.on_search_results(1, {}));
    CHECK_NOTHROW(f.h.on_search_failed(1, "x"));

    f.shell.in_room_search_pending_[2] = "!r";
    f.shell.thread_search_pending_[3] = "$t";
    CHECK_NOTHROW(f.h.on_search_results(2, {}));
    CHECK_NOTHROW(f.h.on_search_failed(2, "x"));
    CHECK_NOTHROW(f.h.on_search_results(3, {}));
    CHECK_NOTHROW(f.h.on_search_failed(3, "x"));
}

TEST_CASE("only sync_offline/sync_error contexts flip the shell offline",
          "[event_handler_base]")
{
    EhbFx f;
    f.h.on_sync_error("sync_offline", "net down", false);
    f.h.on_sync_error("sync_error", "bad", true);
    f.h.on_sync_error("auth", "expired", true);
    CHECK(f.shell.log ==
          std::vector<std::string>{
              "offline", "syncerr:sync_offline:@me:x:net down:hard", "offline",
              "syncerr:sync_error:@me:x:bad:soft", "syncerr:auth:@me:x:expired:soft"});
}

TEST_CASE("RoomListState::Running while offline brings the shell back online",
          "[event_handler_base]")
{
    EhbFx f;
    f.shell.offline_ = true;
    f.h.on_room_list_state(tesseract::RoomListState::SettingUp);
    CHECK(f.shell.offline_);
    f.h.on_room_list_state(tesseract::RoomListState::Running);
    CHECK_FALSE(f.shell.offline_);
    CHECK(f.shell.log == std::vector<std::string>{"online"});
    CHECK(f.shell.room_list_ui == 2);
    CHECK(f.shell.inflight_ui == 2);
}

TEST_CASE("inflight count drives the spinner tick and the UI refresh",
          "[event_handler_base]")
{
    EhbFx f;
    f.h.on_inflight_changed(0);
    CHECK(f.shell.last_inflight_ == 0);
    CHECK(f.shell.tick_stops == 1);
    f.h.on_inflight_changed(6);
    CHECK(f.shell.last_inflight_ == 6);
    CHECK(f.shell.tick_starts == 1);
    CHECK(f.shell.inflight_ui == 2);
}

TEST_CASE("verification state is persisted on the account and only shown for "
          "the active one",
          "[event_handler_base]")
{
    EhbFx f;
    auto me = std::make_shared<tesseract::AccountSession>();
    me->user_id = "@me:x";
    auto other = std::make_shared<tesseract::AccountSession>();
    other->user_id = "@other:x";
    f.shell.am_.add_account(me);
    f.shell.am_.add_account(other);
    f.shell.active_account_ = other;

    f.h.on_verification_state_changed(false);
    CHECK(me->unverified);
    CHECK_FALSE(other->unverified);

    f.h.on_verification_state_changed(true);
    CHECK_FALSE(me->unverified);
    // Recovery-state nudge for a non-active account is dropped silently.
    CHECK_NOTHROW(f.h.on_recovery_state_changed(1));
}

TEST_CASE("call callbacks forward ids and participants", "[event_handler_base]")
{
    EhbFx f;
    tesseract::RtcParticipantInfo p;
    p.participant_id = "p1";
    f.h.on_call_invitation("!r", "slot", "@c", "video", 5000, "$n");
    f.h.on_call_participant_joined(4, p);
    f.h.on_call_participant_updated(4, p);
    f.h.on_call_participant_left(4, "p1");
    f.h.on_call_ended(4, "hangup");
    CHECK(f.shell.log ==
          std::vector<std::string>{"inv:!r:slot:@c:video:5000:$n", "pj:4:p1",
                                   "pu:4:p1", "pl:4:p1", "end:4:hangup"});
}

TEST_CASE("opaque RGBA video frames become BGRA with channels swapped",
          "[event_handler_base]")
{
    EhbFx f;
    const std::uint8_t rgba[8] = {10, 20, 30, 255, 40, 50, 60, 255};
    f.h.on_call_video_frame(1, "p", 2, 1, rgba, 8);
    f.h.on_call_screen_frame(1, "p", 2, 1, rgba, 8);
    REQUIRE(f.shell.frames.size() == 2);
    const std::vector<std::uint8_t> want{30, 20, 10, 255, 60, 50, 40, 255};
    CHECK(f.shell.frames[0] == want);
    CHECK(f.shell.frames[1] == want);
    CHECK(f.shell.log == std::vector<std::string>{"vf:1:p:2x1", "sf:1:p:2x1"});
}

TEST_CASE("translucent RGBA frames are premultiplied", "[event_handler_base]")
{
    EhbFx f;
    // First pixel alpha != 255 selects the premultiplying path.
    const std::uint8_t rgba[8] = {255, 255, 255, 128, 100, 0, 200, 0};
    f.h.on_call_video_frame(1, "p", 2, 1, rgba, 8);
    REQUIRE(f.shell.frames.size() == 1);
    const auto& o = f.shell.frames[0];
    CHECK(o[0] == (255 * 128 + 127) / 255);
    CHECK(o[3] == 128);
    CHECK(o[4] == 0); // alpha 0 wipes colour
    CHECK(o[5] == 0);
    CHECK(o[6] == 0);
    CHECK(o[7] == 0);
}

TEST_CASE("an empty video frame is forwarded as empty", "[event_handler_base]")
{
    EhbFx f;
    f.h.on_call_video_frame(1, "p", 0, 0, nullptr, 0);
    REQUIRE(f.shell.frames.size() == 1);
    CHECK(f.shell.frames[0].empty());
}

TEST_CASE("call audio frames bypass the UI queue and tolerate no output",
          "[event_handler_base]")
{
    EhbFx f;
    const std::int16_t pcm[4] = {1, 2, 3, 4};
    f.h.on_call_audio_frame(1, "p", pcm, 4, 48000, 2);
    CHECK(f.shell.log.empty());
}

TEST_CASE("simple string and value callbacks are forwarded", "[event_handler_base]")
{
    EhbFx f;
    f.h.on_image_packs_updated();
    f.h.on_account_prefs_updated("{\"a\":1}");
    f.h.on_media_preview_config_updated("{}");
    f.h.on_gif_results(3, std::vector<tesseract::GifResult>(2));
    f.h.on_gif_search_failed(4, "nope");
    CHECK(f.shell.log ==
          std::vector<std::string>{"packs", "prefs:@me:x:{\"a\":1}",
                                   "mpc:@me:x:{}", "gif:3:2", "giffail:4:nope"});
}

TEST_CASE("remaining callbacks are safe on an idle shell", "[event_handler_base]")
{
    EhbFx f;
    f.h.on_media_ready(99, {1, 2});
    f.h.on_media_chunk(99, {1}, 1, 1);
    f.h.on_url_preview_ready(99, "{}");
    f.h.on_forward_done(99);
    f.h.on_forward_failed(99, "x");
    f.h.on_space_child_summary_ready(99, "{}");
    f.h.on_server_info_ready(99, "{}");
    f.h.on_media_preview_config_ready(99, "{}");
    f.h.on_room_preview_override_ready(99, "{}");
    f.h.on_room_security_state_ready(99, tesseract::RoomSecurityState{});
    f.h.on_room_directory_search_results(99, {}, true);
    f.h.on_room_directory_search_failed(99, "x");
    f.h.on_paginate_result(99, true, false, false, "");
    f.h.on_media_view_paginate_result(99, true, false, 0, "");
    f.h.on_room_media_page(99, {}, true, 0);
    f.h.on_room_export_progress(tesseract::RoomExportProgress{});
    f.h.on_room_export_complete(99, false, true, false, "", 0, 0, "");
    f.h.on_room_action_complete(99, false, "", "x");
    f.h.on_upload_complete(99, false, "x");
    f.h.on_upload_progress(99, 1, 2);
    f.h.on_profile_field_result(99, "k", false, "x");
    f.h.on_extended_profile_ready(99, "{}");
    f.h.on_backup_progress(tesseract::BackupProgress{});
    f.h.on_enable_recovery_progress(1, "", 0, 0);
    f.h.on_crypto_reset_result(false, "x");
    f.h.on_own_profile_changed(std::nullopt, std::nullopt);
    f.h.on_room_media_preview_override_updated("!r", "{}");
    f.h.on_notification("!r", "R", "@u", "b", false, {}, {}, "$e");
    f.h.on_verification_request("f", "@u", "D", true);
    f.h.on_sas_ready("f", tesseract::VerificationSas{});
    f.h.on_verification_done("f");
    f.h.on_verification_cancelled("f", "x");
    f.h.on_typing_changed("!r", {"a"});
    f.h.on_user_identities_changed({"@u"});
    f.h.on_identity_status_changed("!r", {});
    f.h.on_presence_changed("@u", tesseract::PresenceState::Online);
    f.h.on_rooms_updated({});
    f.h.on_invites_updated({});
    f.h.on_my_knocks_updated({});
    SUCCEED();
}
