#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "views/EncryptionSetupOverlay.h"

using tesseract::ShellBase;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct WithAccountManager { tesseract::AccountManager am_; };

struct TestShell : WithAccountManager, ShellBase
{
    TestShell() : ShellBase(am_) {}

    // ── ShellBase pure virtuals ───────────────────────────────────────────
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
    std::int64_t monotonic_ms_() override { return 0; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}

    // ── New pure virtuals (Task 11) ───────────────────────────────────────
    void raise_and_activate_() override {}
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    void request_relogin_(const std::string&) override {}

    // ── New pure virtual under test ───────────────────────────────────────
    EncryptionSetupOverlay::Mode last_mode_{};
    bool overlay_shown_ = false;
    void show_encryption_setup_overlay_(EncryptionSetupOverlay::Mode m) override
    {
        overlay_shown_ = true;
        last_mode_     = m;
    }

    // ── Inject recovery state for testing ────────────────────────────────
    uint8_t recovery_state_stub_ = 0;
    uint8_t read_recovery_state_() const override { return recovery_state_stub_; }

    bool identity_exists_stub_ = false;
    bool device_verified_stub_ = false;
    bool read_own_identity_exists_() const override { return identity_exists_stub_; }
    bool read_device_verified_() const override { return device_verified_stub_; }

    // ── Expose internals for test inspection ─────────────────────────────
    using ShellBase::check_encryption_setup_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::encryption_setup_dismissed_;
};

} // namespace

TEST_CASE("Disabled state → Fresh overlay shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1; // Disabled
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
    CHECK(shell.encryption_setup_shown_);
}

TEST_CASE("Disabled + foreign identity (exists, unverified) → Recover",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1; // Disabled
    shell.identity_exists_stub_ = true;  // cross-signing set up elsewhere
    shell.device_verified_stub_ = false; // this device can't satisfy it
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Disabled + own verified identity → Fresh (add recovery key)",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1; // Disabled
    shell.identity_exists_stub_ = true; // our own, just bootstrapped
    shell.device_verified_stub_ = true; // already cross-signed
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

TEST_CASE("Incomplete state → Recover overlay shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 3; // Incomplete
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Enabled state → overlay NOT shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 2; // Enabled
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("Unknown state → overlay NOT shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 0; // Unknown
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("encryption_setup_shown_ guards against double-raise",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    shell.overlay_shown_ = false;
    shell.check_encryption_setup_(); // second call — guarded
    CHECK_FALSE(shell.overlay_shown_);
    shell.check_encryption_setup_(); // third call — still guarded
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("encryption_setup_dismissed_ prevents overlay from showing",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1;
    shell.encryption_setup_dismissed_ = true;
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}
