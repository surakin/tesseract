#include <catch2/catch_test_macros.hpp>

#include "app/EncryptionFlowController.h"
#include "app/ShellBase.h"
#include "views/EncryptionSetupOverlay.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct ShellEncryptionSetupWithAccountManager { tesseract::AccountManager am_; };

struct ShellEncryptionSetupTestShell : ShellEncryptionSetupWithAccountManager, ShellBase
{
    ShellEncryptionSetupTestShell() : ShellBase(am_) {}

    // ── ShellBase pure virtuals ───────────────────────────────────────────
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void post_to_ui_after_(int, std::function<void()> fn) override { fn(); }
    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const tk::CacheKey&, MediaKind,
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
    int raised_ = 0;
    void raise_and_activate_() override { ++raised_; }
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override { return nullptr; }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
    bool is_ctrl_held_() const override { return false; }
    std::vector<std::string> switched_to_;
    void switch_active_account_(const std::string& uid) override
    {
        switched_to_.push_back(uid);
        active_account_ = am_.find(uid);
    }
    void refresh_account_ui_after_switch_() override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string&) override { return nullptr; }
    void install_account_notifier_(tesseract::AccountSession&) override {}
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
    bool have_keys_stub_       = false;
    bool read_own_identity_exists_() const override { return identity_exists_stub_; }
    bool read_device_verified_() const override { return device_verified_stub_; }
    bool read_have_cross_signing_keys_() const override { return have_keys_stub_; }

    std::int64_t now_s_ = 1'000'000;
    std::int64_t wall_clock_s_() const override { return now_s_; }

    // ── Expose internals for test inspection ─────────────────────────────
    using ShellBase::my_user_id_;
    using ShellBase::active_account_;
    using ShellBase::handle_verification_request_ui_;
    using ShellBase::check_encryption_setup_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::encryption_setup_dismissed_;
};

} // namespace

TEST_CASE("Disabled state → Fresh overlay shown", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 1; // Disabled
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
    CHECK(shell.encryption_setup_shown_);
}

TEST_CASE("Disabled + foreign identity (exists, no local keys) → Recover",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_  = 1;     // Disabled
    shell.identity_exists_stub_ = true;  // cross-signing set up elsewhere
    shell.have_keys_stub_       = false; // we don't hold the private keys
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Disabled + own identity with local keys → Fresh (add recovery key)",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_  = 1;    // Disabled
    shell.identity_exists_stub_ = true; // our own, just bootstrapped
    shell.have_keys_stub_       = true; // private keys present locally
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

// Regression: a fresh first device whose login-time bootstrap created the
// identity but whose verification_state() has not yet flipped to Verified.
// device_verified() is false, yet the private keys are present → must be Fresh,
// not Recover (the bug where the friend got a "verify this device" dead end).
TEST_CASE("Disabled + own keys but not-yet-verified → Fresh",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_  = 1;     // Disabled
    shell.identity_exists_stub_ = true;  // bootstrapped at login
    shell.have_keys_stub_       = true;  // private keys stored locally
    shell.device_verified_stub_ = false; // verification_state lags
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

TEST_CASE("Incomplete state → Recover overlay shown", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 3; // Incomplete
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Enabled + foreign identity on an unconfirmed device → Recover",
          "[shell][encryption]")
{
    // What the old "verify this device" banner used to prompt for now opens
    // the one dialog.
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_  = 2;
    shell.identity_exists_stub_ = true;
    shell.have_keys_stub_       = false;
    shell.device_verified_stub_ = false;
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("A snoozed reminder also holds back the automatic dialog",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.my_user_id_          = "@snooze-test:example.org";
    shell.recovery_state_stub_ = 1;
    auto& snoozes = tesseract::Settings::instance().encryption_reminder_snoozed_until;
    snoozes[shell.my_user_id_] = shell.now_s_ + 60;
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);

    shell.now_s_ += 61; // snooze over
    shell.check_encryption_setup_();
    CHECK(shell.overlay_shown_);
    snoozes.erase(shell.my_user_id_);
}

TEST_CASE("Enabled state → overlay NOT shown", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 2; // Enabled
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("Unknown state → overlay NOT shown", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 0; // Unknown
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("encryption_setup_shown_ guards against double-raise",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
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
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 1;
    shell.encryption_setup_dismissed_ = true;
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

// ── EncryptionFlowController rules ──────────────────────────────────────────

using tesseract::EncryptionFlowController;
using Reminder = EncryptionFlowController::Reminder;

TEST_CASE("Reminder: fresh account without recovery → SetupNeeded",
          "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(1, true, false) == Reminder::SetupNeeded);
    CHECK(EncryptionFlowController::reminder_for(1, false, false) == Reminder::SetupNeeded);
}

TEST_CASE("Reminder: unconfirmed device on a foreign identity → Locked",
          "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(2, false, true) == Reminder::Locked);
    CHECK(EncryptionFlowController::reminder_for(1, false, true) == Reminder::Locked);
    CHECK(EncryptionFlowController::reminder_for(0, false, true) == Reminder::Locked);
}

TEST_CASE("Reminder: Incomplete recovery → Locked only while unconfirmed",
          "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(3, false, false) == Reminder::Locked);
    // Just verified via another device; its secrets haven't arrived yet.
    CHECK(EncryptionFlowController::reminder_for(3, true, false) == Reminder::None);
    CHECK(EncryptionFlowController::reminder_for(3, true, true) == Reminder::None);
}

TEST_CASE("Reminder: nothing to do → None", "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(2, true, false) == Reminder::None);
    CHECK(EncryptionFlowController::reminder_for(0, true, false) == Reminder::None);
    // Confirmed device, identity made elsewhere, no recovery: can't fix it
    // from here and messages are readable.
    CHECK(EncryptionFlowController::reminder_for(1, true, true) == Reminder::None);
}

TEST_CASE("Snooze window", "[encryption][flow]")
{
    const std::int64_t until = 1000 + EncryptionFlowController::kSnoozeSeconds;
    CHECK(EncryptionFlowController::snoozed(until, 1000));
    CHECK(EncryptionFlowController::snoozed(until, until - 1));
    CHECK_FALSE(EncryptionFlowController::snoozed(until, until));
    CHECK_FALSE(EncryptionFlowController::snoozed(0, 1000)); // never snoozed
}

TEST_CASE("Incoming requests are refused only while the dialog is busy",
          "[encryption][flow]")
{
    using Action = EncryptionFlowController::IncomingAction;
    CHECK(EncryptionFlowController::on_incoming(false, false) == Action::Show);
    CHECK(EncryptionFlowController::on_incoming(true, false) == Action::Show);
    CHECK(EncryptionFlowController::on_incoming(true, true) == Action::RefuseBusy);
    // A hidden dialog's stale step can't block anything.
    CHECK(EncryptionFlowController::on_incoming(false, true) == Action::Show);
}

TEST_CASE("Flow bookkeeping", "[encryption][flow]")
{
    EncryptionFlowController f;
    CHECK_FALSE(f.has_flow());
    f.set_awaiting_outgoing(true);
    CHECK(f.awaiting_outgoing());
    f.begin({.id = "flow1", .user_id = "@me:x", .incoming = false});
    CHECK(f.is_flow("flow1"));
    CHECK_FALSE(f.is_flow("flow2"));
    CHECK_FALSE(f.awaiting_outgoing()); // adopted
    f.clear();
    CHECK_FALSE(f.has_flow());
    CHECK_FALSE(f.is_flow("flow1"));
}

// ── Verification requests for a background account ──────────────────────────

namespace
{
std::shared_ptr<tesseract::AccountSession> make_account(const std::string& uid)
{
    auto s = std::make_shared<tesseract::AccountSession>();
    s->user_id = uid;
    s->client  = std::make_unique<tesseract::Client>();
    return s;
}
} // namespace

TEST_CASE("An incoming request for a background account switches to it and "
          "raises the window",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    auto bob   = make_account("@bob:example.org");
    shell.am_.add_account(alice);
    shell.am_.add_account(bob);
    shell.active_account_ = alice;

    shell.handle_verification_request_ui_("@bob:example.org", "flow1", "@bob:example.org",
                                          "PHONE", /*incoming=*/true);
    REQUIRE(shell.switched_to_.size() == 1);
    CHECK(shell.switched_to_[0] == "@bob:example.org");
    CHECK(shell.raised_ == 1);
}

TEST_CASE("A request for the active account doesn't switch or raise",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    shell.am_.add_account(alice);
    shell.active_account_ = alice;

    shell.handle_verification_request_ui_("@alice:example.org", "flow1",
                                          "@alice:example.org", "PHONE", true);
    CHECK(shell.switched_to_.empty());
    CHECK(shell.raised_ == 0);
}

TEST_CASE("Background-account events that aren't new requests are ignored",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    auto bob   = make_account("@bob:example.org");
    shell.am_.add_account(alice);
    shell.am_.add_account(bob);
    shell.active_account_ = alice;

    // An accepted outgoing flow, and a request for an account we don't have.
    shell.handle_verification_request_ui_("@bob:example.org", "flow1", "@bob:example.org",
                                          "PHONE", /*incoming=*/false);
    shell.handle_verification_request_ui_("@carol:example.org", "flow2",
                                          "@carol:example.org", "PHONE", true);
    CHECK(shell.switched_to_.empty());
    CHECK(shell.raised_ == 0);
}
