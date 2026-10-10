// Remaining EventHandlerBridge callbacks (thread timeline, roster lists, crypto
// progress, request/response completions, uploads, export, verification and
// RTC lifecycle): every one must reach the handler with its arguments intact.
// See test_event_handler_bridge_dispatch.cpp for the core cases.

#include <catch2/catch_test_macros.hpp>

#include "tesseract/event_handler_bridge.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <memory>
#include <string>
#include <vector>

using tesseract_ffi::EventHandlerBridge;
using tesseract_ffi::HandlerSlot;

namespace
{

std::string b(bool v)
{
    return v ? "1" : "0";
}
std::string n(std::uint64_t v)
{
    return std::to_string(v);
}

struct FwdRecorder : tesseract::IEventHandler
{
    std::vector<std::string> log;

    void on_timeline_reset(const std::string&, tesseract::EventList) override {}
    void on_message_inserted(const std::string&, std::size_t,
                             std::unique_ptr<tesseract::Event>) override {}
    void on_message_updated(const std::string&, std::size_t,
                            std::unique_ptr<tesseract::Event>) override {}
    void on_message_removed(const std::string&, std::size_t) override {}
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>&) override {}
    void on_sync_error(const std::string&, const std::string&, bool) override {}

    void on_thread_inserted(const std::string& r, const std::string& t,
                            std::size_t i,
                            std::unique_ptr<tesseract::Event> e) override
    {
        log.push_back("tins:" + r + ":" + t + ":" + n(i) + ":" + e->event_id);
    }
    void on_thread_updated(const std::string& r, const std::string& t,
                           std::size_t i,
                           std::unique_ptr<tesseract::Event> e) override
    {
        log.push_back("tupd:" + r + ":" + t + ":" + n(i) + ":" + e->event_id);
    }
    void on_thread_removed(const std::string& r, const std::string& t,
                           std::size_t i) override
    {
        log.push_back("trm:" + r + ":" + t + ":" + n(i));
    }
    void on_invites_updated(const std::vector<tesseract::InviteInfo>& v) override
    {
        log.push_back("invites:" + n(v.size()) + ":" + v.at(0).room_id + ":" +
                      v.at(0).inviter_user_id + ":" + v.at(0).reason);
    }
    void on_my_knocks_updated(
        const std::vector<tesseract::KnockedRoomInfo>& v) override
    {
        log.push_back("knocks:" + n(v.size()) + ":" + v.at(0).room_id + ":" +
                      v.at(0).reason);
    }
    void on_backup_progress(const tesseract::BackupProgress& p) override
    {
        log.push_back("backup:" + n(static_cast<int>(p.state)) + ":" +
                      n(p.imported_keys) + "/" + n(p.total_keys));
    }
    void on_enable_recovery_progress(uint8_t step, const std::string& key,
                                     uint32_t done, uint32_t total) override
    {
        log.push_back("recovery:" + n(step) + ":" + key + ":" + n(done) + "/" +
                      n(total));
    }
    void on_crypto_reset_result(bool ok, const std::string& m) override
    {
        log.push_back("reset:" + b(ok) + ":" + m);
    }
    void on_inflight_changed(std::uint32_t c) override
    {
        log.push_back("inflight:" + n(c));
    }
    void on_inflight_changed_debug(std::uint32_t c, std::string u) override
    {
        log.push_back("inflightdbg:" + n(c) + ":" + u);
    }
    void on_image_packs_updated() override { log.push_back("packs"); }
    void on_threads_updated(const std::string& r) override
    {
        log.push_back("threads:" + r);
    }
    void on_knock_requests_updated(const std::string& r) override
    {
        log.push_back("knockreq:" + r);
    }
    void on_bot_commands_updated(const std::string& r) override
    {
        log.push_back("bot:" + r);
    }
    void on_media_chunk(std::uint64_t id, const std::vector<std::uint8_t>& c,
                        std::uint8_t st, std::uint64_t total) override
    {
        log.push_back("chunk:" + n(id) + ":" + n(c.size()) + ":" + n(st) + ":" +
                      n(total));
    }
    void on_url_preview_ready(std::uint64_t id, const std::string& j) override
    {
        log.push_back("preview:" + n(id) + ":" + j);
    }
    void on_gif_search_failed(std::uint64_t id, const std::string& m) override
    {
        log.push_back("giffail:" + n(id) + ":" + m);
    }
    void on_forward_done(std::uint64_t id) override
    {
        log.push_back("fwddone:" + n(id));
    }
    void on_forward_failed(std::uint64_t id, const std::string& m) override
    {
        log.push_back("fwdfail:" + n(id) + ":" + m);
    }
    void on_space_child_summary_ready(std::uint64_t id,
                                      const std::string& j) override
    {
        log.push_back("space:" + n(id) + ":" + j);
    }
    void on_server_info_ready(std::uint64_t id, const std::string& j) override
    {
        log.push_back("server:" + n(id) + ":" + j);
    }
    void on_media_preview_config_ready(std::uint64_t id,
                                       const std::string& j) override
    {
        log.push_back("mpcr:" + n(id) + ":" + j);
    }
    void on_room_preview_override_ready(std::uint64_t id,
                                        const std::string& j) override
    {
        log.push_back("rpo:" + n(id) + ":" + j);
    }
    void on_room_security_state_ready(
        std::uint64_t id, const tesseract::RoomSecurityState& s) override
    {
        log.push_back("sec:" + n(id) + ":" + b(s.is_encrypted) + ":" +
                      s.join_rule + ":" + b(s.guest_access) + ":" +
                      s.history_visibility);
    }
    void on_search_failed(std::uint64_t id, const std::string& m) override
    {
        log.push_back("searchfail:" + n(id) + ":" + m);
    }
    void on_room_directory_search_failed(std::uint64_t id,
                                         const std::string& m) override
    {
        log.push_back("dirfail:" + n(id) + ":" + m);
    }
    void on_paginate_result(std::uint64_t id, bool ok, bool s, bool e,
                            const std::string& m) override
    {
        log.push_back("pag:" + n(id) + b(ok) + b(s) + b(e) + ":" + m);
    }
    void on_media_view_paginate_result(std::uint64_t id, bool ok, bool s,
                                       std::uint64_t c,
                                       const std::string& m) override
    {
        log.push_back("mvpag:" + n(id) + b(ok) + b(s) + ":" + n(c) + ":" + m);
    }
    void on_room_export_progress(const tesseract::RoomExportProgress& p) override
    {
        log.push_back("exp:" + n(p.request_id) + ":" + p.room_id + ":" +
                      n(p.events_written) + ":" + n(p.bytes_written) + ":" +
                      b(p.reached_start) + b(p.finalizing) + ":" +
                      n(p.assembly_done) + "/" + n(p.assembly_total));
    }
    void on_room_export_complete(std::uint64_t id, bool ok, bool cancelled,
                                 bool start, const std::string& path,
                                 std::uint64_t ev, std::uint64_t by,
                                 const std::string& m) override
    {
        log.push_back("expdone:" + n(id) + b(ok) + b(cancelled) + b(start) + ":" +
                      path + ":" + n(ev) + ":" + n(by) + ":" + m);
    }
    void on_room_action_complete(std::uint64_t id, bool ok,
                                 const std::string& room,
                                 const std::string& m) override
    {
        log.push_back("action:" + n(id) + b(ok) + ":" + room + ":" + m);
    }
    void on_upload_complete(std::uint64_t id, bool ok,
                            const std::string& m) override
    {
        log.push_back("upload:" + n(id) + b(ok) + ":" + m);
    }
    void on_upload_progress(std::uint64_t id, std::uint64_t c,
                            std::uint64_t t) override
    {
        log.push_back("uprog:" + n(id) + ":" + n(c) + "/" + n(t));
    }
    void on_profile_field_result(std::uint64_t id, const std::string& k, bool ok,
                                 const std::string& m) override
    {
        log.push_back("field:" + n(id) + ":" + k + b(ok) + ":" + m);
    }
    void on_extended_profile_ready(std::uint64_t id,
                                   const std::string& j) override
    {
        log.push_back("ext:" + n(id) + ":" + j);
    }
    void on_account_prefs_updated(const std::string& j) override
    {
        log.push_back("prefs:" + j);
    }
    void on_media_preview_config_updated(const std::string& j) override
    {
        log.push_back("mpcu:" + j);
    }
    void on_room_media_preview_override_updated(const std::string& r,
                                                const std::string& j) override
    {
        log.push_back("rmpo:" + r + ":" + j);
    }
    void on_verification_done(const std::string& f) override
    {
        log.push_back("vdone:" + f);
    }
    void on_verification_cancelled(const std::string& f,
                                   const std::string& r) override
    {
        log.push_back("vcancel:" + f + ":" + r);
    }
    void on_verification_state_changed(bool v) override
    {
        log.push_back("vstate:" + b(v));
    }
    void on_recovery_state_changed(std::uint8_t s) override
    {
        log.push_back("rstate:" + n(s));
    }
    void on_user_identities_changed(const std::vector<std::string>& u) override
    {
        log.push_back("idents:" + n(u.size()) + ":" + u.at(0));
    }
    void on_call_participant_left(std::uint64_t s, const std::string& p) override
    {
        log.push_back("pleft:" + n(s) + ":" + p);
    }
    void on_call_participant_updated(std::uint64_t s,
                                     const tesseract::RtcParticipantInfo& i) override
    {
        log.push_back("pupd:" + n(s) + ":" + i.participant_id + b(i.is_video_muted));
    }
    void on_call_ended(std::uint64_t s, const std::string& r) override
    {
        log.push_back("ended:" + n(s) + ":" + r);
    }
    void on_call_screen_frame(std::uint64_t s, const std::string& p,
                              std::uint32_t w, std::uint32_t h,
                              const std::uint8_t*, std::size_t sz) override
    {
        log.push_back("sf:" + n(s) + ":" + p + ":" + n(w) + "x" + n(h) + ":" +
                      n(sz));
    }
};

struct FwdFixture
{
    FwdRecorder rec;
    std::shared_ptr<HandlerSlot> slot = std::make_shared<HandlerSlot>(&rec);
    EventHandlerBridge bridge{slot};
};

tesseract_ffi::TimelineEvent fwd_ev(const char* id)
{
    tesseract_ffi::TimelineEvent e{};
    e.msg_type = "m.text";
    e.event_id = id;
    return e;
}

} // namespace

TEST_CASE("bridge forwards thread timeline mutations", "[bridge][forwarding]")
{
    FwdFixture f;
    f.bridge.on_thread_inserted("!r", "$t", 1, fwd_ev("$a"));
    f.bridge.on_thread_updated("!r", "$t", 2, fwd_ev("$b"));
    f.bridge.on_thread_removed("!r", "$t", 3);
    CHECK(f.rec.log == std::vector<std::string>{"tins:!r:$t:1:$a",
                                                "tupd:!r:$t:2:$b", "trm:!r:$t:3"});
}

TEST_CASE("bridge converts invite and knock lists", "[bridge][forwarding]")
{
    FwdFixture f;
    rust::Vec<tesseract_ffi::InviteInfo> inv;
    tesseract_ffi::InviteInfo i{};
    i.room_id = "!i:x";
    i.inviter_user_id = "@inviter:x";
    i.reason = "join us";
    inv.push_back(std::move(i));
    f.bridge.on_invites_updated(inv);

    rust::Vec<tesseract_ffi::KnockedRoomInfo> kn;
    tesseract_ffi::KnockedRoomInfo k{};
    k.room_id = "!k:x";
    k.reason = "please";
    kn.push_back(std::move(k));
    f.bridge.on_my_knocks_updated(kn);

    CHECK(f.rec.log == std::vector<std::string>{"invites:1:!i:x:@inviter:x:join us",
                                                "knocks:1:!k:x:please"});
}

TEST_CASE("bridge forwards crypto progress callbacks", "[bridge][forwarding]")
{
    FwdFixture f;
    tesseract_ffi::BackupProgress bp{};
    bp.state = 3; // Downloading
    bp.imported_keys = 5;
    bp.total_keys = 10;
    f.bridge.on_backup_progress(bp);
    f.bridge.on_enable_recovery_progress(2, "key", 3, 9);
    f.bridge.on_crypto_reset_result(true, "done");
    f.bridge.on_verification_done("f");
    f.bridge.on_verification_cancelled("f", "mismatch");
    f.bridge.on_verification_state_changed(true);
    f.bridge.on_recovery_state_changed(4);
    rust::Vec<rust::String> ids;
    ids.push_back("@a:x");
    f.bridge.on_user_identities_changed(ids);
    CHECK(f.rec.log ==
          std::vector<std::string>{
              "backup:" +
                  std::to_string(static_cast<int>(tesseract::BackupState::Downloading)) +
                  ":5/10",
              "recovery:2:key:3/9", "reset:1:done", "vdone:f",
              "vcancel:f:mismatch", "vstate:1", "rstate:4", "idents:1:@a:x"});
}

TEST_CASE("bridge forwards parameterless and room-keyed notifications",
          "[bridge][forwarding]")
{
    FwdFixture f;
    f.bridge.on_inflight_changed(4);
    f.bridge.on_image_packs_updated();
    f.bridge.on_threads_updated("!r");
    f.bridge.on_knock_requests_updated("!r");
    f.bridge.on_bot_commands_updated("!r");
    f.bridge.on_account_prefs_updated("{}");
    f.bridge.on_media_preview_config_updated("{\"a\":1}");
    f.bridge.on_room_media_preview_override_updated("!r", "{\"o\":1}");
    CHECK(f.rec.log ==
          std::vector<std::string>{"inflight:4", "packs", "threads:!r",
                                   "knockreq:!r", "bot:!r", "prefs:{}",
                                   "mpcu:{\"a\":1}", "rmpo:!r:{\"o\":1}"});
}

#ifndef NDEBUG
TEST_CASE("bridge forwards the debug inflight breakdown in debug builds",
          "[bridge][forwarding]")
{
    FwdFixture f;
    f.bridge.on_inflight_changed_debug(2, "a\nb");
    CHECK(f.rec.log == std::vector<std::string>{"inflightdbg:2:a\nb"});
}
#endif

TEST_CASE("bridge forwards request/response completions", "[bridge][forwarding]")
{
    FwdFixture f;
    const std::uint8_t data[] = {1, 2, 3};
    f.bridge.on_media_chunk(1, rust::Slice<const std::uint8_t>(data, 3), 2, 99);
    f.bridge.on_url_preview_ready(2, "{p}");
    f.bridge.on_gif_search_failed(3, "gf");
    f.bridge.on_forward_done(4);
    f.bridge.on_forward_failed(5, "ff");
    f.bridge.on_space_child_summary_ready(6, "{s}");
    f.bridge.on_server_info_ready(7, "{i}");
    f.bridge.on_media_preview_config_ready(8, "{c}");
    f.bridge.on_room_preview_override_ready(9, "{o}");
    f.bridge.on_search_failed(10, "sf");
    f.bridge.on_room_directory_search_failed(11, "df");
    f.bridge.on_paginate_result(12, true, false, true, "p");
    f.bridge.on_media_view_paginate_result(13, false, true, 42, "mv");
    f.bridge.on_room_action_complete(14, true, "!joined", "m");
    f.bridge.on_upload_complete(15, false, "um");
    f.bridge.on_upload_progress(16, 5, 50);
    f.bridge.on_profile_field_result(17, "status", true, "ok");
    f.bridge.on_extended_profile_ready(18, "{e}");
    CHECK(f.rec.log ==
          std::vector<std::string>{
              "chunk:1:3:2:99", "preview:2:{p}", "giffail:3:gf", "fwddone:4",
              "fwdfail:5:ff", "space:6:{s}", "server:7:{i}", "mpcr:8:{c}",
              "rpo:9:{o}", "searchfail:10:sf", "dirfail:11:df", "pag:12101:p",
              "mvpag:13" + std::string("01") + ":42:mv", "action:141:!joined:m",
              "upload:150:um", "uprog:16:5/50", "field:17:status1:ok",
              "ext:18:{e}"});
}

TEST_CASE("bridge converts the room security state", "[bridge][forwarding]")
{
    FwdFixture f;
    tesseract_ffi::RoomSecurityStateFfi s{};
    s.is_encrypted = true;
    s.join_rule = "knock";
    s.guest_access = true;
    s.history_visibility = "invited";
    f.bridge.on_room_security_state_ready(3, s);
    CHECK(f.rec.log == std::vector<std::string>{"sec:3:1:knock:1:invited"});
}

TEST_CASE("bridge converts export progress and completion", "[bridge][forwarding]")
{
    FwdFixture f;
    tesseract_ffi::RoomExportProgressFfi p{};
    p.request_id = 8;
    p.room_id = "!r";
    p.events_written = 100;
    p.bytes_written = 2048;
    p.reached_start = true;
    p.assembly_done = 3;
    p.assembly_total = 7;
    f.bridge.on_room_export_progress(p);
    f.bridge.on_room_export_complete(8, true, false, true, "/tmp/out", 100, 2048,
                                     "fine");
    CHECK(f.rec.log ==
          std::vector<std::string>{"exp:8:!r:100:2048:10:3/7",
                                   "expdone:8101:/tmp/out:100:2048:fine"});
}

TEST_CASE("bridge forwards the remaining RTC lifecycle callbacks",
          "[bridge][forwarding]")
{
    FwdFixture f;
    tesseract_ffi::RtcParticipantInfo info{};
    info.participant_id = "p";
    info.is_video_muted = true;
    f.bridge.on_rtc_participant_updated(3, info);
    f.bridge.on_rtc_participant_left(3, "p");
    f.bridge.on_rtc_session_ended(3, "hangup");
    const std::uint8_t px[4] = {};
    f.bridge.on_rtc_screen_frame(3, "p", 1, 1, rust::Slice<const std::uint8_t>(px, 4));
    CHECK(f.rec.log ==
          std::vector<std::string>{"pupd:3:p1", "pleft:3:p", "ended:3:hangup",
                                   "sf:3:p:1x1:4"});
}
