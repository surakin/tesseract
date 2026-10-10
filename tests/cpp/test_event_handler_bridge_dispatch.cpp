// Dispatch tests for tesseract_ffi::EventHandlerBridge (client/src/
// event_handler_bridge.cpp): the object Rust calls into, which converts FFI
// types and forwards to the tesseract::IEventHandler held in a HandlerSlot.
//
// test_event_handler_bridge.cpp covers ffi_convert.h only; this file drives the
// bridge itself with a recording handler: argument forwarding/conversion,
// detach() dropping late callbacks, and exception containment.

#include <catch2/catch_test_macros.hpp>

#include "tesseract/event_handler_bridge.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using tesseract_ffi::EventHandlerBridge;
using tesseract_ffi::HandlerSlot;

namespace
{

struct BridgeRecorder : tesseract::IEventHandler
{
    std::vector<std::string> log;
    std::vector<std::string> ids;
    std::vector<std::size_t> idx;
    std::vector<tesseract::RoomInfo> rooms;
    bool throw_on_removed = false;

    void on_timeline_reset(const std::string& r,
                           tesseract::EventList snap) override
    {
        log.push_back("reset:" + r);
        for (auto& e : snap)
        {
            ids.push_back(e->event_id);
        }
    }
    void on_message_inserted(const std::string& r, std::size_t i,
                             std::unique_ptr<tesseract::Event> e) override
    {
        log.push_back("ins:" + r);
        idx.push_back(i);
        ids.push_back(e->event_id);
    }
    void on_message_updated(const std::string& r, std::size_t i,
                            std::unique_ptr<tesseract::Event> e) override
    {
        log.push_back("upd:" + r);
        idx.push_back(i);
        ids.push_back(e->event_id);
    }
    void on_message_removed(const std::string& r, std::size_t i) override
    {
        if (throw_on_removed)
        {
            throw std::runtime_error("boom");
        }
        log.push_back("rm:" + r);
        idx.push_back(i);
    }
    void on_messages_prepended(const std::string& r,
                               tesseract::EventList evs) override
    {
        log.push_back("pre:" + r + ":" + std::to_string(evs.size()));
    }
    void on_messages_appended(const std::string& r,
                              tesseract::EventList evs) override
    {
        log.push_back("app:" + r + ":" + std::to_string(evs.size()));
    }
    void on_messages_updated_batch(const std::string& r,
                                   std::vector<std::size_t> is,
                                   tesseract::EventList evs) override
    {
        log.push_back("batch:" + r + ":" + std::to_string(evs.size()));
        idx = is;
    }
    void on_thread_reset(const std::string& r, const std::string& t,
                         tesseract::EventList s) override
    {
        log.push_back("treset:" + r + ":" + t + ":" + std::to_string(s.size()));
    }
    void on_thread_messages_prepended(const std::string& r, const std::string& t,
                                      tesseract::EventList s) override
    {
        log.push_back("tpre:" + r + ":" + t + ":" + std::to_string(s.size()));
    }
    void on_thread_messages_appended(const std::string& r, const std::string& t,
                                     tesseract::EventList s) override
    {
        log.push_back("tapp:" + r + ":" + t + ":" + std::to_string(s.size()));
    }
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& r) override
    {
        rooms = r;
        log.push_back("rooms");
    }
    void on_sync_error(const std::string& c, const std::string& d,
                       bool soft) override
    {
        log.push_back("err:" + c + ":" + d + ":" + (soft ? "soft" : "hard"));
    }
    void on_session_saved(const std::string& j) override
    {
        log.push_back("session:" + j);
    }
    void on_room_list_state(tesseract::RoomListState s) override
    {
        log.push_back("rls:" + std::to_string(static_cast<int>(s)));
    }
    void on_media_ready(std::uint64_t id,
                        const std::vector<std::uint8_t>& b) override
    {
        log.push_back("media:" + std::to_string(id) + ":" +
                      std::to_string(b.size()));
    }
    void on_own_profile_changed(const std::optional<std::string>& n,
                                const std::optional<std::string>& a) override
    {
        log.push_back(std::string("profile:") + (n ? *n : "-") + ":" +
                      (a ? *a : "-"));
    }
    void on_presence_changed(const std::string& u,
                             tesseract::PresenceState s) override
    {
        log.push_back("presence:" + u + ":" + std::to_string(static_cast<int>(s)));
    }
    void on_identity_status_changed(
        const std::string& r,
        const std::vector<tesseract::IdentityWarning>& w) override
    {
        std::string s = "identity:" + r;
        for (auto& x : w)
        {
            s += ":" + x.user_id + "/" + x.display_name + "/" +
                 (x.kind == tesseract::IdentityWarning::Kind::VerificationBroken
                      ? "broken"
                      : "changed");
        }
        log.push_back(s);
    }
    void on_typing_changed(const std::string& r,
                           const std::vector<std::string>& n) override
    {
        log.push_back("typing:" + r + ":" + std::to_string(n.size()));
    }
    void on_gif_results(std::uint64_t id,
                        const std::vector<tesseract::GifResult>& r) override
    {
        log.push_back("gif:" + std::to_string(id));
        if (!r.empty())
        {
            log.push_back(r[0].id + "|" + r[0].image_url + "|" +
                          std::to_string(r[0].image_w));
        }
    }
    void on_search_results(std::uint64_t id,
                           const std::vector<tesseract::SearchHit>& r) override
    {
        log.push_back("search:" + std::to_string(id) + ":" +
                      std::to_string(r.size()));
        if (!r.empty())
        {
            log.push_back(r[0].room_name + "|" + std::to_string(r[0].timestamp_ms));
        }
    }
    void on_room_directory_search_results(
        std::uint64_t id, const std::vector<tesseract::RoomDirectoryEntry>& e,
        bool end) override
    {
        log.push_back("dir:" + std::to_string(id) + ":" +
                      std::to_string(e.size()) + (end ? ":end" : ":more"));
        if (!e.empty())
        {
            log.push_back(e[0].alias + "|" + std::to_string(e[0].joined_members));
        }
    }
    void on_verification_request(const std::string& f, const std::string& u,
                                 const std::string& d, bool in) override
    {
        log.push_back("vreq:" + f + ":" + u + ":" + d + (in ? ":in" : ":out"));
    }
    void on_sas_ready(const std::string& f, tesseract::VerificationSas s) override
    {
        log.push_back("sas:" + f + ":" + std::to_string(s.emojis.size()) + ":" +
                      std::to_string(s.decimals[1]));
    }
    void on_call_participant_joined(std::uint64_t s,
                                    const tesseract::RtcParticipantInfo& i) override
    {
        log.push_back("pj:" + std::to_string(s) + ":" + i.participant_id + ":" +
                      i.user_id + (i.is_screen_sharing ? ":share" : ""));
    }
    void on_call_video_frame(std::uint64_t s, const std::string& p,
                             std::uint32_t w, std::uint32_t h,
                             const std::uint8_t*, std::size_t n) override
    {
        log.push_back("vf:" + std::to_string(s) + ":" + p + ":" +
                      std::to_string(w) + "x" + std::to_string(h) + ":" +
                      std::to_string(n));
    }
    void on_call_audio_frame(std::uint64_t s, const std::string& p,
                             const std::int16_t*, std::size_t n,
                             std::uint32_t rate, std::uint32_t ch) override
    {
        log.push_back("af:" + std::to_string(s) + ":" + p + ":" +
                      std::to_string(n) + ":" + std::to_string(rate) + ":" +
                      std::to_string(ch));
    }
    void on_call_invitation(const std::string& r, const std::string& s,
                            const std::string& c, const std::string& i,
                            std::uint64_t life, const std::string& n) override
    {
        log.push_back("inv:" + r + ":" + s + ":" + c + ":" + i + ":" +
                      std::to_string(life) + ":" + n);
    }
    void on_notification(const std::string& r, const std::string& rn,
                         const std::string& s, const std::string& b, bool m,
                         const std::vector<std::uint8_t>& av,
                         const std::vector<std::uint8_t>& img,
                         const std::string& e) override
    {
        log.push_back("notif:" + r + ":" + rn + ":" + s + ":" + b +
                      (m ? ":m:" : ":-:") + std::to_string(av.size()) + ":" +
                      std::to_string(img.size()) + ":" + e);
    }
    void on_room_media_page(std::uint64_t id,
                            const std::vector<tesseract::MediaIndexRow>& rows,
                            bool end, std::uint64_t total) override
    {
        log.push_back("mpage:" + std::to_string(id) + ":" +
                      std::to_string(rows.size()) + (end ? ":end:" : ":more:") +
                      std::to_string(total));
        if (!rows.empty())
        {
            log.push_back(rows[0].event_id + "|" + rows[0].src_mxc + "|" +
                          std::to_string(rows[0].media_w));
        }
    }
};

struct BridgeFixture
{
    BridgeRecorder rec;
    std::shared_ptr<HandlerSlot> slot = std::make_shared<HandlerSlot>(&rec);
    EventHandlerBridge bridge{slot};
};

tesseract_ffi::TimelineEvent ffi_ev(const char* id)
{
    tesseract_ffi::TimelineEvent e{};
    e.msg_type = "m.text";
    e.event_id = id;
    return e;
}

} // namespace

TEST_CASE("bridge forwards single timeline mutations with index and event",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_message_inserted("!r:s", 3, ffi_ev("$a"));
    f.bridge.on_message_updated("!r:s", 4, ffi_ev("$b"));
    f.bridge.on_message_removed("!r:s", 5);
    CHECK(f.rec.log == std::vector<std::string>{"ins:!r:s", "upd:!r:s", "rm:!r:s"});
    CHECK(f.rec.idx == std::vector<std::size_t>{3, 4, 5});
    CHECK(f.rec.ids == std::vector<std::string>{"$a", "$b"});
}

TEST_CASE("bridge converts a snapshot vector preserving order",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<tesseract_ffi::TimelineEvent> v;
    v.push_back(ffi_ev("$1"));
    v.push_back(ffi_ev("$2"));
    v.push_back(ffi_ev("$3"));
    f.bridge.on_timeline_reset("!r:s", v);
    f.bridge.on_messages_prepended("!r:s", v);
    f.bridge.on_messages_appended("!r:s", v);
    f.bridge.on_thread_reset("!r:s", "$root", v);
    f.bridge.on_thread_messages_prepended("!r:s", "$root", v);
    f.bridge.on_thread_messages_appended("!r:s", "$root", v);
    CHECK(f.rec.ids == std::vector<std::string>{"$1", "$2", "$3"});
    CHECK(f.rec.log ==
          std::vector<std::string>{"reset:!r:s", "pre:!r:s:3", "app:!r:s:3",
                                   "treset:!r:s:$root:3", "tpre:!r:s:$root:3",
                                   "tapp:!r:s:$root:3"});
}

TEST_CASE("bridge batch update requires matching non-empty parallel arrays",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<std::uint64_t> idx;
    rust::Vec<tesseract_ffi::TimelineEvent> evs;

    // Empty: dropped.
    f.bridge.on_messages_updated_batch("!r:s", idx, evs);
    CHECK(f.rec.log.empty());

    // Mismatched lengths: dropped.
    idx.push_back(1);
    idx.push_back(2);
    evs.push_back(ffi_ev("$1"));
    f.bridge.on_messages_updated_batch("!r:s", idx, evs);
    CHECK(f.rec.log.empty());

    // Matching: forwarded with indices.
    evs.push_back(ffi_ev("$2"));
    f.bridge.on_messages_updated_batch("!r:s", idx, evs);
    CHECK(f.rec.log == std::vector<std::string>{"batch:!r:s:2"});
    CHECK(f.rec.idx == std::vector<std::size_t>{1, 2});
}

TEST_CASE("detach() makes every later callback a silent no-op",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_message_removed("!r:s", 1);
    REQUIRE(f.rec.log.size() == 1);
    f.slot->detach();
    CHECK(f.slot->load() == nullptr);
    f.bridge.on_message_removed("!r:s", 2);
    f.bridge.on_error("c", "m", false);
    f.bridge.on_image_packs_updated();
    f.bridge.on_room_list_state(2);
    CHECK(f.rec.log.size() == 1);
}

TEST_CASE("a throwing handler never propagates out of the bridge",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.rec.throw_on_removed = true;
    CHECK_NOTHROW(f.bridge.on_message_removed("!r:s", 1));
    // The bridge stays usable afterwards.
    f.bridge.on_error("ctx", "msg", true);
    CHECK(f.rec.log == std::vector<std::string>{"err:ctx:msg:soft"});
}

TEST_CASE("bridge forwards error and session callbacks",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_error("sync", "bad", false);
    f.bridge.on_session_refreshed("{\"a\":1}");
    CHECK(f.rec.log == std::vector<std::string>{"err:sync:bad:hard",
                                                "session:{\"a\":1}"});
}

TEST_CASE("room list state codes beyond Terminated clamp to Init",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_room_list_state(1);
    f.bridge.on_room_list_state(static_cast<std::uint8_t>(
        tesseract::RoomListState::Terminated));
    f.bridge.on_room_list_state(200);
    CHECK(f.rec.log ==
          std::vector<std::string>{
              "rls:1",
              "rls:" + std::to_string(static_cast<int>(
                           tesseract::RoomListState::Terminated)),
              "rls:0"});
}

TEST_CASE("bridge converts room lists", "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<tesseract_ffi::RoomInfo> v;
    tesseract_ffi::RoomInfo r{};
    r.id = "!x:s";
    r.name = "Room X";
    r.is_direct = true;
    v.push_back(std::move(r));
    f.bridge.on_rooms_updated(v);
    REQUIRE(f.rec.rooms.size() == 1);
    CHECK(f.rec.rooms[0].id == "!x:s");
    CHECK(f.rec.rooms[0].name == "Room X");
    CHECK(f.rec.rooms[0].is_direct);
}

TEST_CASE("bridge copies byte slices", "[bridge][dispatch]")
{
    BridgeFixture f;
    const std::uint8_t data[] = {1, 2, 3, 4};
    f.bridge.on_media_ready(7, rust::Slice<const std::uint8_t>(data, 4));
    f.bridge.on_notification("!r", "Name", "@u", "hi", true,
                             rust::Slice<const std::uint8_t>(data, 2),
                             rust::Slice<const std::uint8_t>(data, 3), "$e");
    CHECK(f.rec.log == std::vector<std::string>{"media:7:4",
                                                "notif:!r:Name:@u:hi:m:2:3:$e"});
}

TEST_CASE("own profile change maps absent fields to nullopt",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_own_profile_changed(true, "Bob", false, "ignored");
    f.bridge.on_own_profile_changed(false, "ignored", true, "mxc://a/b");
    f.bridge.on_own_profile_changed(true, "", true, "");
    CHECK(f.rec.log == std::vector<std::string>{"profile:Bob:-",
                                                "profile:-:mxc://a/b",
                                                "profile::"});
}

TEST_CASE("presence codes map to Online/Unavailable/Offline",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_presence_changed("@a", 1);
    f.bridge.on_presence_changed("@b", 2);
    f.bridge.on_presence_changed("@c", 0);
    f.bridge.on_presence_changed("@d", 99);
    auto on = std::to_string(static_cast<int>(tesseract::PresenceState::Online));
    auto un = std::to_string(static_cast<int>(tesseract::PresenceState::Unavailable));
    auto off = std::to_string(static_cast<int>(tesseract::PresenceState::Offline));
    CHECK(f.rec.log == std::vector<std::string>{"presence:@a:" + on,
                                                "presence:@b:" + un,
                                                "presence:@c:" + off,
                                                "presence:@d:" + off});
}

TEST_CASE("identity status zips parallel arrays tolerating short ones",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<rust::String> ids;
    rust::Vec<rust::String> names;
    rust::Vec<std::uint8_t> kinds;
    ids.push_back("@a");
    ids.push_back("@b");
    ids.push_back("@c");
    names.push_back("Alice");
    names.push_back("Bob");
    kinds.push_back(1);
    kinds.push_back(2);
    f.bridge.on_identity_status_changed("!r", ids, names, kinds);
    CHECK(f.rec.log ==
          std::vector<std::string>{
              "identity:!r:@a/Alice/changed:@b/Bob/broken:@c//changed"});
}

TEST_CASE("typing list is forwarded", "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<rust::String> ids;
    ids.push_back("@a");
    ids.push_back("@b");
    f.bridge.on_typing_changed("!r", ids);
    CHECK(f.rec.log == std::vector<std::string>{"typing:!r:2"});
}

TEST_CASE("gif, search and directory results convert every field",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    {
        rust::Vec<tesseract_ffi::GifResult> v;
        tesseract_ffi::GifResult g{};
        g.id = "g1";
        g.image_url = "https://x/g.gif";
        g.image_w = 320;
        v.push_back(std::move(g));
        f.bridge.on_gif_results(11, v);
    }
    {
        rust::Vec<tesseract_ffi::SearchHit> v;
        tesseract_ffi::SearchHit h{};
        h.room_name = "Lobby";
        h.timestamp_ms = 123456789;
        v.push_back(std::move(h));
        f.bridge.on_search_results(12, v);
    }
    {
        rust::Vec<tesseract_ffi::RoomDirectoryEntryFfi> v;
        tesseract_ffi::RoomDirectoryEntryFfi e{};
        e.alias = "#a:s";
        e.joined_members = 42;
        v.push_back(std::move(e));
        f.bridge.on_room_directory_search_results(13, v, true);
    }
    CHECK(f.rec.log ==
          std::vector<std::string>{"gif:11", "g1|https://x/g.gif|320",
                                   "search:12:1", "Lobby|123456789",
                                   "dir:13:1:end", "#a:s|42"});
}

TEST_CASE("media index rows convert", "[bridge][dispatch]")
{
    BridgeFixture f;
    rust::Vec<tesseract_ffi::MediaIndexRowFfi> v;
    tesseract_ffi::MediaIndexRowFfi r{};
    r.event_id = "$m";
    r.src_mxc = "mxc://s/m";
    r.media_w = 640;
    v.push_back(std::move(r));
    f.bridge.on_room_media_page(5, v, false, 99);
    CHECK(f.rec.log == std::vector<std::string>{"mpage:5:1:more:99",
                                                "$m|mxc://s/m|640"});
}

TEST_CASE("verification and call callbacks forward their arguments",
          "[bridge][dispatch]")
{
    BridgeFixture f;
    f.bridge.on_verification_request("f1", "@u", "DEV", true);
    tesseract_ffi::VerificationSas sas{};
    tesseract_ffi::VerificationEmoji em{};
    em.symbol = "x";
    sas.emojis.push_back(std::move(em));
    sas.decimals = {10, 20, 30};
    f.bridge.on_sas_ready("f1", sas);

    tesseract_ffi::RtcParticipantInfo p{};
    p.participant_id = "p";
    p.user_id = "@u";
    p.is_screen_sharing = true;
    f.bridge.on_rtc_participant_joined(9, p);

    const std::uint8_t px[8] = {};
    f.bridge.on_rtc_video_frame(9, "p", 2, 1, rust::Slice<const std::uint8_t>(px, 8));
    const std::int16_t pcm[4] = {};
    f.bridge.on_rtc_audio_frame(9, "p", rust::Slice<const std::int16_t>(pcm, 4),
                                48000, 2);
    f.bridge.on_rtc_invitation("!r", "slot", "@c", "video", 30000, "$n");

    CHECK(f.rec.log ==
          std::vector<std::string>{"vreq:f1:@u:DEV:in", "sas:f1:1:20",
                                   "pj:9:p:@u:share", "vf:9:p:2x1:8",
                                   "af:9:p:4:48000:2",
                                   "inv:!r:slot:@c:video:30000:$n"});
}
