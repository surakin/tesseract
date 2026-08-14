#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::KnockedRoomInfo;
using tesseract::KnockRequestInfo;

namespace
{

struct ShellKnockRequestsWithAccountManager { tesseract::AccountManager am_; };

// Minimal concrete ShellBase, mirroring test_shell_dispatch_room_send.cpp's
// harness: every pure virtual gets a no-op body, and `using` declarations
// expose the private knock-related members under test.
struct KnockShell : ShellKnockRequestsWithAccountManager, ShellBase
{
    KnockShell() : ShellBase(am_) {}

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
    void raise_and_activate_() override {}
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
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::client_;
    using ShellBase::my_user_id_;
    using ShellBase::my_knocks_;
    using ShellBase::current_room_knock_requests_;
    using ShellBase::knock_requests_panel_room_id_;
    using ShellBase::pending_room_actions_;
    using ShellBase::RoomActionKind;
    using ShellBase::find_my_knock_;
    using ShellBase::push_my_knocks_;
    using ShellBase::decline_knock_request_async_;
    using ShellBase::decline_and_ban_knock_request_async_;
    using ShellBase::handle_room_action_complete_ui_;
    using ShellBase::next_room_action_id_;
};

KnockedRoomInfo make_knock(std::string room_id)
{
    KnockedRoomInfo k;
    k.room_id = std::move(room_id);
    k.room_name = "Test Room";
    return k;
}

KnockRequestInfo make_request(std::string room_id, std::string user_id)
{
    KnockRequestInfo r;
    r.room_id = std::move(room_id);
    r.user_id = std::move(user_id);
    r.display_name = "Alice";
    return r;
}

} // namespace

TEST_CASE("push_my_knocks_ populates my_knocks_ for the active account and find_my_knock_ resolves it",
          "[shell][knock]")
{
    KnockShell s;
    s.my_user_id_ = "@me:example.org";

    std::vector<KnockedRoomInfo> knocks;
    knocks.push_back(make_knock("!a:example.org"));
    knocks.push_back(make_knock("!b:example.org"));

    s.push_my_knocks_("@me:example.org", knocks);

    REQUIRE(s.my_knocks_.size() == 2);
    const auto* found = s.find_my_knock_("!b:example.org");
    REQUIRE(found != nullptr);
    CHECK(found->room_id == "!b:example.org");
    CHECK(s.find_my_knock_("!missing:example.org") == nullptr);
}

TEST_CASE("push_my_knocks_ for a non-active account does not touch my_knocks_",
          "[shell][knock]")
{
    KnockShell s;
    s.my_user_id_ = "@me:example.org";

    std::vector<KnockedRoomInfo> other_knocks;
    other_knocks.push_back(make_knock("!other:example.org"));

    s.push_my_knocks_("@someone-else:example.org", other_knocks);

    CHECK(s.my_knocks_.empty());
}

TEST_CASE("decline_knock_request_async_ optimistically removes the request immediately",
          "[shell][knock]")
{
    KnockShell s;
    tesseract::Client client; // unauthenticated; the FFI call itself no-ops
    s.client_ = &client;

    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@bob:example.org"));

    s.decline_knock_request_async_("!r:example.org", "@alice:example.org");

    REQUIRE(s.current_room_knock_requests_.size() == 1);
    CHECK(s.current_room_knock_requests_.front().user_id == "@bob:example.org");
}

TEST_CASE("decline_and_ban_knock_request_async_ optimistically removes the request immediately",
          "[shell][knock]")
{
    KnockShell s;
    tesseract::Client client;
    s.client_ = &client;

    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));

    s.decline_and_ban_knock_request_async_("!r:example.org", "@alice:example.org", "spam");

    CHECK(s.current_room_knock_requests_.empty());
}

TEST_CASE("decline_knock_request_async_ is a no-op without a live client",
          "[shell][knock]")
{
    KnockShell s;
    // client_ defaults to nullptr.
    s.knock_requests_panel_room_id_ = "!r:example.org";
    s.current_room_knock_requests_.push_back(make_request("!r:example.org", "@alice:example.org"));

    s.decline_knock_request_async_("!r:example.org", "@alice:example.org");

    // Guarded on !client_ before any local-list mutation — the request stays.
    CHECK(s.current_room_knock_requests_.size() == 1);
}

TEST_CASE("handle_room_action_complete_ui_ resolves a pending Knock/AcceptKnock action without crashing",
          "[shell][knock]")
{
    KnockShell s;

    auto knock_id = s.next_room_action_id_++;
    s.pending_room_actions_[knock_id] = {"!r:example.org", ShellBase::RoomActionKind::Knock};
    s.handle_room_action_complete_ui_(knock_id, /*ok=*/true, "", "");
    CHECK(s.pending_room_actions_.count(knock_id) == 0);

    auto accept_id = s.next_room_action_id_++;
    s.pending_room_actions_[accept_id] = {"!r:example.org", ShellBase::RoomActionKind::AcceptKnock};
    s.handle_room_action_complete_ui_(accept_id, /*ok=*/true, "", "");
    CHECK(s.pending_room_actions_.count(accept_id) == 0);

    // Failure path also resolves cleanly (exercises the "send/accept join
    // request" verb strings in the failure-message switch).
    auto failed_id = s.next_room_action_id_++;
    s.pending_room_actions_[failed_id] = {"!r:example.org", ShellBase::RoomActionKind::Knock};
    s.handle_room_action_complete_ui_(failed_id, /*ok=*/false, "", "M_FORBIDDEN");
    CHECK(s.pending_room_actions_.count(failed_id) == 0);
}
