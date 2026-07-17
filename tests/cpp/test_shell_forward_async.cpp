#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellForwardAsyncWithAccountManager
{
    tesseract::AccountManager am_;
};

// Minimal ShellBase test double for the forward-async callback handlers.
// main_app_ stays null so picker calls are skipped; we exercise the
// pending_forwards_ state mutations directly.
struct ForwardShell : ShellForwardAsyncWithAccountManager, ShellBase
{
    ForwardShell() : ShellBase(am_) {}

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
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}

    using ShellBase::handle_forward_done_ui_;
    using ShellBase::handle_forward_failed_ui_;
    using ShellBase::my_user_id_;
    using ShellBase::pending_forwards_;
    using ShellBase::push_rooms_;
};

} // namespace

// ── handle_forward_done_ui_ ───────────────────────────────────────────────

TEST_CASE("handle_forward_done_ui_ erases the completed entry",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[42] = "!room:x";
    s.handle_forward_done_ui_(42);
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_done_ui_ does not disturb other in-flight requests",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!a:x";
    s.pending_forwards_[2] = "!b:x";
    s.handle_forward_done_ui_(1);
    REQUIRE(s.pending_forwards_.size() == 1);
    REQUIRE(s.pending_forwards_.count(2) == 1);
}

TEST_CASE("handle_forward_done_ui_ with unknown request_id is a no-op",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!room:x";
    s.handle_forward_done_ui_(99);
    REQUIRE(s.pending_forwards_.size() == 1);
}

// ── handle_forward_failed_ui_ ────────────────────────────────────────────

TEST_CASE("handle_forward_failed_ui_ ignores unknown request_id",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!room:x";
    s.handle_forward_failed_ui_(99, "network error");
    REQUIRE(s.pending_forwards_.size() == 1);
}

TEST_CASE("handle_forward_failed_ui_ erases the failed entry",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[7] = "!room:x";
    s.handle_forward_failed_ui_(7, "timeout");
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_failed_ui_ leaves other in-flight requests intact",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.pending_forwards_[1] = "!a:x";
    s.pending_forwards_[2] = "!b:x";
    s.handle_forward_failed_ui_(1, "error");
    REQUIRE(s.pending_forwards_.size() == 1);
    REQUIRE(s.pending_forwards_.count(2) == 1);
}

TEST_CASE("handle_forward_failed_ui_ falls back to room_id when room unknown",
          "[shell][forward_async]")
{
    // Room is not in the room list — room_by_id_ returns null.
    // The handler must not crash; it falls back to the raw room_id string.
    ForwardShell s;
    s.my_user_id_ = "@me:x";
    s.pending_forwards_[3] = "!unknown:x";
    s.handle_forward_failed_ui_(3, "error"); // must not crash
    REQUIRE(s.pending_forwards_.empty());
}

TEST_CASE("handle_forward_failed_ui_ uses room name when room is known",
          "[shell][forward_async]")
{
    ForwardShell s;
    s.my_user_id_ = "@me:x";
    tesseract::RoomInfo r;
    r.id   = "!a:x";
    r.name = "General";
    s.push_rooms_("@me:x", {r});

    s.pending_forwards_[5] = "!a:x";
    // fp is null (no MainAppWidget in tests) so add_forward_error is not called,
    // but the handler must not crash and must erase the entry regardless.
    s.handle_forward_failed_ui_(5, "send failed");
    REQUIRE(s.pending_forwards_.empty());
}
