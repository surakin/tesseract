#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double exposing push_rooms_ + the known-users roster cache
// flags, so the room-set-change invalidation can be exercised without a window
// or client. Pure-virtual surface stubbed to no-ops.
struct WithAccountManager
{
    tesseract::AccountManager am_;
};

struct RosterShell : WithAccountManager, ShellBase
{
    RosterShell() : ShellBase(am_) {}

    void post_to_ui_(std::function<void()> fn) override { queue.push_back(std::move(fn)); }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
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
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}

    std::vector<std::function<void()>> queue;

    using ShellBase::known_users_built_;
    using ShellBase::my_user_id_;
    using ShellBase::push_rooms_;
};

tesseract::RoomInfo room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    return r;
}

} // namespace

TEST_CASE("roster survives a same-set push (no invalidation)",
          "[shell][roster]")
{
    RosterShell s;
    s.my_user_id_ = "@me:x";
    s.push_rooms_("@me:x", {room("!a:x"), room("!b:x")});
    s.known_users_built_ = true; // simulate a built roster

    // Same set, different order → no invalidation.
    s.push_rooms_("@me:x", {room("!b:x"), room("!a:x")});
    CHECK(s.known_users_built_);
}

TEST_CASE("roster is invalidated when the room set changes with equal count",
          "[shell][roster]")
{
    RosterShell s;
    s.my_user_id_ = "@me:x";
    s.push_rooms_("@me:x", {room("!a:x"), room("!b:x")});
    s.known_users_built_ = true;

    // Join one, leave one — count unchanged, set changed → must invalidate.
    s.push_rooms_("@me:x", {room("!a:x"), room("!c:x")});
    CHECK_FALSE(s.known_users_built_);
}
