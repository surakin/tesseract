#include <catch2/catch_test_macros.hpp>

#include "app/SearchBackend.h"
#include "app/ShellBase.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

using tesseract::SearchBackend;
using tesseract::ShellBase;

namespace
{

tesseract::RoomInfo room(const std::string& id, const std::string& name,
                         const std::string& topic = {})
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = name;
    r.topic = topic;
    return r;
}

tesseract::RoomMember user(const std::string& id, const std::string& name)
{
    tesseract::RoomMember m;
    m.user_id = id;
    m.display_name = name;
    return m;
}

} // namespace

TEST_CASE("query matches room name/topic case-insensitively", "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms = {
        room("!a:x", "Engineering", "sprint planning"),
        room("!b:x", "Random", "off-topic chatter"),
    };
    sb.register_shell({.rooms = [&] { return rooms; },
                       .known_users = {},
                       .activate_room = {},
                       .activate_contact = {}});

    auto by_name = sb.query("engin");
    REQUIRE(by_name.size() == 1);
    CHECK(by_name[0].id == "!a:x");
    CHECK(by_name[0].kind == SearchBackend::ResultKind::Room);

    auto by_topic = sb.query("CHATTER");
    REQUIRE(by_topic.size() == 1);
    CHECK(by_topic[0].id == "!b:x");
}

TEST_CASE("query matches contacts by display name and mxid", "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomMember> users = {
        user("@alice:x", "Alice"),
        user("@bob:x", ""), // falls back to mxid for title
    };
    sb.register_shell({.rooms = {},
                       .known_users = [&] { return users; },
                       .activate_room = {},
                       .activate_contact = {}});

    auto r1 = sb.query("alice");
    REQUIRE(r1.size() == 1);
    CHECK(r1[0].kind == SearchBackend::ResultKind::Contact);
    CHECK(r1[0].id == "@alice:x");

    auto r2 = sb.query("@bob");
    REQUIRE(r2.size() == 1);
    CHECK(r2[0].id == "@bob:x");
    CHECK(r2[0].title == "@bob:x");
}

TEST_CASE("rooms rank ahead of contacts, both alphabetical within group",
          "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms = {
        room("!z:x", "zzz-team"),
        room("!a:x", "aaa-team"),
    };
    std::vector<tesseract::RoomMember> users = {
        user("@z:x", "zzz-person"),
        user("@a:x", "aaa-person"),
    };
    sb.register_shell({.rooms = [&] { return rooms; },
                       .known_users = [&] { return users; },
                       .activate_room = {},
                       .activate_contact = {}});

    auto results = sb.query("aa");
    // "aaa-team" (room) sorts before "aaa-person" (contact) purely because
    // rooms are ranked as a group ahead of contacts, not alphabetically
    // across kinds.
    REQUIRE(results.size() == 2);
    CHECK(results[0].kind == SearchBackend::ResultKind::Room);
    CHECK(results[0].id == "!a:x");
    CHECK(results[1].kind == SearchBackend::ResultKind::Contact);
    CHECK(results[1].id == "@a:x");
}

TEST_CASE("query respects max_results cap", "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms = {
        room("!1:x", "team-1"), room("!2:x", "team-2"), room("!3:x", "team-3"),
    };
    sb.register_shell({.rooms = [&] { return rooms; },
                       .known_users = {},
                       .activate_room = {},
                       .activate_contact = {}});

    CHECK(sb.query("team", 2).size() == 2);
    CHECK(sb.query("team", 10).size() == 3);
}

TEST_CASE("query carries avatar_url through for rooms and contacts",
          "[search_backend]")
{
    SearchBackend sb;

    tesseract::RoomInfo with_avatar = room("!a:x", "Engineering");
    with_avatar.avatar_url = "mxc://x/room-avatar";

    tesseract::RoomInfo dm_fallback = room("!b:x", "Direct Chat");
    dm_fallback.dm_avatar_url = "mxc://x/dm-avatar";

    tesseract::RoomInfo no_avatar = room("!c:x", "No Avatar");

    std::vector<tesseract::RoomInfo> rooms = {with_avatar, dm_fallback,
                                              no_avatar};

    tesseract::RoomMember alice = user("@alice:x", "Alice");
    alice.avatar_url = "mxc://x/alice-avatar";
    std::vector<tesseract::RoomMember> users = {alice};

    sb.register_shell({.rooms = [&] { return rooms; },
                       .known_users = [&] { return users; },
                       .activate_room = {},
                       .activate_contact = {}});

    auto r1 = sb.query("engin");
    REQUIRE(r1.size() == 1);
    CHECK(r1[0].avatar_url == "mxc://x/room-avatar");

    // effective_avatar_url() falls back to dm_avatar_url when avatar_url is
    // empty — the room result must carry that fallback through, not the
    // (empty) room avatar_url directly.
    auto r2 = sb.query("direct");
    REQUIRE(r2.size() == 1);
    CHECK(r2[0].avatar_url == "mxc://x/dm-avatar");

    auto r3 = sb.query("no avatar");
    REQUIRE(r3.size() == 1);
    CHECK(r3[0].avatar_url.empty());

    auto r4 = sb.query("alice");
    REQUIRE(r4.size() == 1);
    CHECK(r4[0].avatar_url == "mxc://x/alice-avatar");
}

TEST_CASE("activate routes to the registered shell owning the id",
          "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms_a = {room("!a:x", "Alpha")};
    std::vector<tesseract::RoomInfo> rooms_b = {room("!b:x", "Beta")};
    std::string activated_in_a, activated_in_b;

    sb.register_shell({.rooms = [&] { return rooms_a; },
                       .known_users = {},
                       .activate_room = [&](const std::string& id)
                       { activated_in_a = id; },
                       .activate_contact = {}});
    sb.register_shell({.rooms = [&] { return rooms_b; },
                       .known_users = {},
                       .activate_room = [&](const std::string& id)
                       { activated_in_b = id; },
                       .activate_contact = {}});

    sb.activate("!b:x", SearchBackend::ResultKind::Room);
    CHECK(activated_in_a.empty());
    CHECK(activated_in_b == "!b:x");
}

TEST_CASE("activate is a no-op for an id that no longer resolves",
          "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms = {room("!a:x", "Alpha")};
    bool activated = false;
    sb.register_shell({.rooms = [&] { return rooms; },
                       .known_users = {},
                       .activate_room = [&](const std::string&)
                       { activated = true; },
                       .activate_contact = {}});

    sb.activate("!gone:x", SearchBackend::ResultKind::Room);
    CHECK_FALSE(activated);
}

TEST_CASE("unregister_shell drops a shell's rooms/contacts from future queries",
          "[search_backend]")
{
    SearchBackend sb;
    std::vector<tesseract::RoomInfo> rooms = {room("!a:x", "Alpha")};
    auto h = sb.register_shell({.rooms = [&] { return rooms; },
                                .known_users = {},
                                .activate_room = {},
                                .activate_contact = {}});
    REQUIRE(sb.query("alpha").size() == 1);

    sb.unregister_shell(h);
    CHECK(sb.query("alpha").empty());
}

namespace
{

// Minimal ShellBase test double, mirroring the pattern in
// test_shell_roster_invalidation.cpp, to exercise ShellBase's own
// register_search_backend_() wiring (rooms_ provider + raise_and_activate_ /
// tab_select_room on activation) rather than SearchBackend in isolation.
// Takes AccountManager by reference (rather than owning one, as
// RosterShell does) so multiple shells in one test can share a single
// SearchBackend, the way real multi-window/multi-account shells do.
struct SearchWiringShell : ShellBase
{
    explicit SearchWiringShell(tesseract::AccountManager& am) : ShellBase(am) {}

    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void post_to_ui_after_(int, std::function<void()> fn) override { fn(); }
    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override { raised = true; }
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override { return nullptr; }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void refresh_account_ui_after_switch_() override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string&) override { return nullptr; }
    void install_account_notifier_(tesseract::AccountSession&) override {}
    void request_relogin_(const std::string&) override {}
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}

    bool raised = false;

    using ShellBase::register_search_backend_;
    using ShellBase::rooms_;
    using ShellBase::tab_select_room;
};

} // namespace

TEST_CASE("ShellBase registers its rooms_ into the shared SearchBackend",
          "[search_backend][shell]")
{
    tesseract::AccountManager am;
    SearchWiringShell s(am);
    s.rooms_ = {room("!a:x", "Engineering")};
    s.register_search_backend_();

    auto results = am.search_backend().query("engin");
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == "!a:x");
}

TEST_CASE("activating a room result raises the window and selects the room",
          "[search_backend][shell]")
{
    tesseract::AccountManager am;
    SearchWiringShell s(am);
    s.rooms_ = {room("!a:x", "Engineering")};
    s.register_search_backend_();

    am.search_backend().activate("!a:x", SearchBackend::ResultKind::Room);
    CHECK(s.raised);
}

TEST_CASE("destroying a ShellBase unregisters it from the shared SearchBackend",
          "[search_backend][shell]")
{
    tesseract::AccountManager am;
    {
        SearchWiringShell s(am);
        s.rooms_ = {room("!a:x", "Engineering")};
        s.register_search_backend_();
        REQUIRE(am.search_backend().query("engin").size() == 1);
    }
    CHECK(am.search_backend().query("engin").empty());
}
