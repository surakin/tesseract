#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct WithAccountManager { tesseract::AccountManager am_; };

struct TestShell : WithAccountManager, ShellBase
{
    TestShell() : ShellBase(am_) {}

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
    void navigate_to_room_(const std::string& id) override
    {
        last_navigate_to = id;
    }
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
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

    using ShellBase::current_room_id_;
    using ShellBase::room_nav_history_;
    using ShellBase::room_nav_history_cursor_;
    using ShellBase::room_nav_in_progress_;
    using ShellBase::after_active_room_changed_;

    std::string last_navigate_to;
};

// Helper: simulate visiting room_id by setting current_room_id_ then
// calling after_active_room_changed_ (same sequence tab functions use).
void visit(TestShell& s, const std::string& room_id)
{
    s.current_room_id_ = room_id;
    s.after_active_room_changed_();
}

} // namespace

TEST_CASE("back/forward are no-ops on empty history", "[shell][nav_history]")
{
    TestShell s;
    s.navigate_history_back();
    CHECK(s.last_navigate_to.empty());
    s.navigate_history_forward();
    CHECK(s.last_navigate_to.empty());
}

TEST_CASE("after_active_room_changed_ appends to nav history",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    REQUIRE(s.room_nav_history_.size() == 1);
    CHECK(s.room_nav_history_[0] == "!a:x");
    CHECK(s.room_nav_history_cursor_ == 0);

    visit(s, "!b:x");
    REQUIRE(s.room_nav_history_.size() == 2);
    CHECK(s.room_nav_history_[1] == "!b:x");
    CHECK(s.room_nav_history_cursor_ == 1);
}

TEST_CASE("navigate_history_back goes to previous room",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!b:x");

    s.navigate_history_back();

    CHECK(s.last_navigate_to == "!a:x");
    CHECK(s.room_nav_history_cursor_ == 0);
}

TEST_CASE("navigate_history_back is a no-op at the oldest entry",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");

    s.navigate_history_back(); // already at oldest
    CHECK(s.last_navigate_to.empty());
    CHECK(s.room_nav_history_cursor_ == 0);
}

TEST_CASE("navigate_history_forward goes to next room after back",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!b:x");

    s.navigate_history_back(); // cursor -> 0
    s.navigate_history_forward(); // cursor -> 1

    CHECK(s.last_navigate_to == "!b:x");
    CHECK(s.room_nav_history_cursor_ == 1);
}

TEST_CASE("navigate_history_forward is a no-op at the newest entry",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!b:x");

    s.navigate_history_forward(); // already at newest
    CHECK(s.last_navigate_to.empty());
    CHECK(s.room_nav_history_cursor_ == 1);
}

TEST_CASE("new visit truncates forward history", "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!b:x");
    visit(s, "!c:x");

    s.navigate_history_back(); // cursor -> 1 (!b)
    s.navigate_history_back(); // cursor -> 0 (!a)

    // visiting !d:x should drop !b and !c
    visit(s, "!d:x");

    REQUIRE(s.room_nav_history_.size() == 2);
    CHECK(s.room_nav_history_[0] == "!a:x");
    CHECK(s.room_nav_history_[1] == "!d:x");
    CHECK(s.room_nav_history_cursor_ == 1);

    // forward must now be a no-op
    s.last_navigate_to.clear();
    s.navigate_history_forward();
    CHECK(s.last_navigate_to.empty());
}

TEST_CASE("history caps at kNavHistoryMax entries, cursor stays at end",
          "[shell][nav_history]")
{
    TestShell s;
    for (int i = 0; i < 105; ++i)
        visit(s, "!r" + std::to_string(i) + ":x");

    CHECK(s.room_nav_history_.size() == 100);
    CHECK(s.room_nav_history_.front() == "!r5:x");  // first 5 evicted
    CHECK(s.room_nav_history_.back() == "!r104:x");
    CHECK(s.room_nav_history_cursor_ == 99);
}

TEST_CASE("consecutive visits to same room append to history",
          "[shell][nav_history]")
{
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!a:x");
    REQUIRE(s.room_nav_history_.size() == 2);
    CHECK(s.room_nav_history_[0] == "!a:x");
    CHECK(s.room_nav_history_[1] == "!a:x");
}

TEST_CASE("room_nav_in_progress_ guard suppresses history append",
          "[shell][nav_history]")
{
    // Simulates what navigate_history_back/forward do: set the guard
    // flag, then trigger after_active_room_changed_ (which in production
    // happens inside the platform navigate_to_room_ chain). The guard
    // must prevent the back-target room from being appended as a new
    // forward entry.
    TestShell s;
    visit(s, "!a:x");
    visit(s, "!b:x");
    REQUIRE(s.room_nav_history_.size() == 2);

    s.room_nav_in_progress_ = true;
    s.current_room_id_       = "!a:x";
    s.after_active_room_changed_();
    s.room_nav_in_progress_ = false;

    REQUIRE(s.room_nav_history_.size() == 2); // no spurious append
    CHECK(s.room_nav_history_[0] == "!a:x");
    CHECK(s.room_nav_history_[1] == "!b:x");
}
