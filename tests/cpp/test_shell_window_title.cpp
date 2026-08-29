#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellWindowTitleWithAccountManager { tesseract::AccountManager am_; };

struct ShellWindowTitleTestShell : ShellWindowTitleWithAccountManager, ShellBase
{
    ShellWindowTitleTestShell() : ShellBase(am_) {}

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

    void apply_window_title_ui_(const std::string& title) override
    {
        last_title = title;
    }

    void set_room(const std::string& id, const std::string& name)
    {
        tesseract::RoomInfo r;
        r.id = id;
        r.name = name;
        rooms_.push_back(std::move(r));
        mark_room_index_dirty_();
        current_room_id_ = id;
    }

    using ShellBase::current_room_id_;
    using ShellBase::rooms_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::app_settings_open_;
    using ShellBase::compose_window_title_;
    using ShellBase::refresh_window_title_;
    using ShellBase::set_app_settings_open_;

    std::string last_title;
};

} // namespace

TEST_CASE("window title is bare product name with no active room",
          "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    CHECK(s.compose_window_title_() == "Tesseract");
}

TEST_CASE("window title reflects the active room name",
          "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.refresh_window_title_();
    CHECK(s.last_title == "Tesseract - General");
}

TEST_CASE("opening app settings drops the room name, closing restores it",
          "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.refresh_window_title_();
    REQUIRE(s.last_title == "Tesseract - General");

    s.set_app_settings_open_(true);
    CHECK(s.last_title == "Tesseract");

    s.set_app_settings_open_(false);
    CHECK(s.last_title == "Tesseract - General");
}

TEST_CASE("title stays bare while settings is open even if rooms refresh",
          "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.set_app_settings_open_(true);
    REQUIRE(s.last_title == "Tesseract");

    // A sync tick would call refresh_window_title_() again.
    s.refresh_window_title_();
    CHECK(s.last_title == "Tesseract");
}
