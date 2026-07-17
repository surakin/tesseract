#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "views/RoomView.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct SendStickerShellWithAccountManager { tesseract::AccountManager am_; };

// Minimal ShellBase test double exercising send_sticker_. A
// default-constructed tesseract::Client has no live FFI (SH_FFI short-
// circuits before reaching the network), so send_sticker_/send_thread_sticker_
// are safe no-ops here — these tests assert on compose-bar reply-state side
// effects, not on FFI results (mirrors SendShell in
// test_shell_dispatch_room_send.cpp).
struct SendStickerShell : SendStickerShellWithAccountManager, ShellBase
{
    SendStickerShell() : ShellBase(am_) {}

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
    using ShellBase::current_room_id_;
    using ShellBase::current_thread_root_;
    using ShellBase::room_view_;
    using ShellBase::send_sticker_;
    using ShellBase::thread_panel_;
};

} // namespace

TEST_CASE("send_sticker_ clears an active reply after sending",
          "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;
    s.current_room_id_ = "!r:x";

    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    s.room_view_ = &view;

    view.compose_bar()->set_reply_to("$evt1", "Alice", "hi");
    REQUIRE(view.compose_bar()->has_reply());

    s.send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}

TEST_CASE("send_sticker_ is a no-op on reply state when no reply is pending",
          "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;
    s.current_room_id_ = "!r:x";

    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    s.room_view_ = &view;

    REQUIRE_FALSE(view.compose_bar()->has_reply());

    s.send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}

TEST_CASE("send_sticker_ clears an active reply in the thread-open branch too",
          "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;
    s.current_room_id_ = "!r:x";
    s.thread_panel_ = ShellBase::ThreadPanel::Open;
    s.current_thread_root_ = "$root:x";

    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    s.room_view_ = &view;

    view.compose_bar()->set_reply_to("$evt2", "Bob", "hello");
    REQUIRE(view.compose_bar()->has_reply());

    s.send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}
