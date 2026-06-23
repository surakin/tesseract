#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct WithAccountManager { tesseract::AccountManager am_; };

// ShellBase test double that records whether the native avatar picker was
// opened, so we can assert that the /myroomavatar branch of the unified
// dispatch_room_send_ ladder routed there.
struct SendShell : WithAccountManager, ShellBase
{
    SendShell() : ShellBase(am_) {}

    bool avatar_picker_opened = false;

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
        std::function<void(std::vector<uint8_t>, std::string)>) override
    {
        // Record but never invoke the callback (i.e. simulate a still-open
        // picker), so no async upload work is queued during the test.
        avatar_picker_opened = true;
    }
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override { return nullptr; }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
#endif
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
    using ShellBase::dispatch_room_send_;
    using ShellBase::pending_room_actions_;
    using ShellBase::RoomActionKind;
};

} // namespace

TEST_CASE("dispatch_room_send_ with no client is treated as handled",
          "[shell][dispatch_room_send]")
{
    SendShell s;
    // client_ defaults to nullptr.
    auto out = s.dispatch_room_send_("!r:x", "hello", "");
    CHECK(out.handled_as_command);
}

TEST_CASE("dispatch_room_send_ routes /myroomavatar to the avatar picker",
          "[shell][dispatch_room_send]")
{
    SendShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "/myroomavatar", "");
    CHECK(out.handled_as_command);
    CHECK(s.avatar_picker_opened);
    // A command must NOT enqueue a normal send.
    CHECK(s.pending_room_actions_.empty());
}

TEST_CASE("dispatch_room_send_ routes /leave to a Leave room action",
          "[shell][dispatch_room_send]")
{
    SendShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "/leave", "");
    CHECK(out.handled_as_command);
    // leave_room_command_ records a pending Leave action synchronously before
    // dispatching the async SDK call.
    REQUIRE(s.pending_room_actions_.size() == 1);
    const auto& action = s.pending_room_actions_.begin()->second;
    CHECK(action.room_id == "!r:x");
    CHECK(action.kind == SendShell::RoomActionKind::Leave);
}

TEST_CASE("dispatch_room_send_ falls through to a normal send for plain text",
          "[shell][dispatch_room_send]")
{
    SendShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "just a message", "");
    // Not a recognized native command: it is a normal compose send, so the
    // caller (not the command ladder) owns the result.
    CHECK_FALSE(out.handled_as_command);
    CHECK(s.avatar_picker_opened == false);
    CHECK(s.pending_room_actions_.empty());
}
