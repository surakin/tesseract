#include <catch2/catch_test_macros.hpp>

#include "app/EncryptionFlowController.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "views/EncryptionSetupOverlay.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct ShellEncryptionSetupTestShell : tesseract::test::TestShellBase
{
    // ── ShellBase pure virtuals ───────────────────────────────────────────
    std::int64_t monotonic_ms_() override
    {
        return 0;
    }

    // ── New pure virtuals (Task 11) ───────────────────────────────────────
    int raised_ = 0;
    void raise_and_activate_() override
    {
        ++raised_;
    }
    std::vector<std::string> switched_to_;
    void switch_active_account_(const std::string& uid) override
    {
        switched_to_.push_back(uid);
        active_account_ = am_.find(uid);
    }

    // ── New pure virtual under test ───────────────────────────────────────
    EncryptionSetupOverlay::Mode last_mode_{};
    bool overlay_shown_ = false;
    void show_encryption_setup_overlay_(EncryptionSetupOverlay::Mode m) override
    {
        overlay_shown_ = true;
        last_mode_ = m;
    }

    // ── Inject recovery state for testing ────────────────────────────────
    uint8_t recovery_state_stub_ = 0;
    uint8_t read_recovery_state_() const override
    {
        return recovery_state_stub_;
    }

    bool identity_exists_stub_ = false;
    bool device_verified_stub_ = false;
    bool have_keys_stub_ = false;
    bool read_own_identity_exists_() const override
    {
        return identity_exists_stub_;
    }
    bool read_device_verified_() const override
    {
        return device_verified_stub_;
    }
    bool read_have_cross_signing_keys_() const override
    {
        return have_keys_stub_;
    }

    std::int64_t now_s_ = 1'000'000;
    std::int64_t wall_clock_s_() const override
    {
        return now_s_;
    }

    // ── Silent setup: no SDK calls, an in-memory "keychain" ───────────────
    int silent_enables_ = 0;
    void run_silent_enable_recovery_(std::shared_ptr<tesseract::AccountSession>) override
    {
        ++silent_enables_;
    }
    std::vector<std::string> silent_recovers_;
    std::function<void(bool)> silent_recover_done_;
    void run_silent_recover_(std::shared_ptr<tesseract::AccountSession>, std::string key,
                             std::function<void(bool)> done) override
    {
        silent_recovers_.push_back(std::move(key));
        silent_recover_done_ = std::move(done);
    }
    std::map<std::string, std::string> keychain_;
    bool keychain_works_ = true;
    bool keychain_readable_ = true;
    using Lookup = tesseract::SecretStore::RecoveryKeyLookup;
    void load_stored_recovery_key_(const std::string& uid,
                                   std::function<void(Lookup)> done) override
    {
        if (!keychain_readable_)
        {
            return done({Lookup::Status::Unreadable, {}});
        }
        auto it = keychain_.find(uid);
        done(it == keychain_.end() ? Lookup{Lookup::Status::Missing, {}}
                                   : Lookup{Lookup::Status::Found, it->second});
    }

    void store_recovery_key_(const std::string& uid, const std::string& key,
                             std::function<void(bool)> done) override
    {
        if (keychain_works_)
        {
            keychain_[uid] = key;
        }
        done(keychain_works_);
    }
    void forget_stored_recovery_key_(const std::string& uid) override
    {
        keychain_.erase(uid);
    }

    // ── Expose internals for test inspection ─────────────────────────────
    using ShellBase::my_user_id_;
    using ShellBase::active_account_;
    using ShellBase::handle_verification_request_ui_;
    using ShellBase::check_encryption_setup_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::encryption_setup_dismissed_;
    using ShellBase::begin_gated_encryption_setup_if_needed_;
    using ShellBase::FinalizeLoginResult;
    using ShellBase::handle_enable_recovery_progress_ui_;
    using ShellBase::mark_recovery_key_saved_;
    using ShellBase::reopen_encryption_setup_;
    using ShellBase::open_save_key_dialog_;
    using ShellBase::silent_recovery_in_flight_;
    using ShellBase::unstored_recovery_keys_;
    using ShellBase::kSilentRecoveryMaxFailures;
    using ShellBase::kSilentRecoveryRetrySeconds;
    using ShellBase::intercept_sign_out_for_unsaved_key_;
    using ShellBase::proceed_sign_out_;
    using ShellBase::settle_recovery_key_on_sign_out_;
    using ShellBase::silent_recovery_pending_;
    using ShellBase::dialog_recovery_setup_users_;
    using ShellBase::silent_recovery_exhausted_;
    using ShellBase::skip_unsaved_key_check_for_next_sign_out_;
    using ShellBase::save_key_dialog_uid_;
    using ShellBase::snooze_save_key_reminder_;
    using ShellBase::silent_recovery_setup_enabled_;
    using ShellBase::silent_recovery_declined_;
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

TEST_CASE("Disabled + foreign identity (exists, no local keys) → Recover", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 1;     // Disabled
    shell.identity_exists_stub_ = true; // cross-signing set up elsewhere
    shell.have_keys_stub_ = false;      // we don't hold the private keys
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Disabled + own identity with local keys → Fresh (add recovery key)",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 1;     // Disabled
    shell.identity_exists_stub_ = true; // our own, just bootstrapped
    shell.have_keys_stub_ = true;       // private keys present locally
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

// Regression: a fresh first device whose login-time bootstrap created the
// identity but whose verification_state() has not yet flipped to Verified.
// device_verified() is false, yet the private keys are present → must be Fresh,
// not Recover (the bug where the friend got a "verify this device" dead end).
TEST_CASE("Disabled + own keys but not-yet-verified → Fresh", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 1;      // Disabled
    shell.identity_exists_stub_ = true;  // bootstrapped at login
    shell.have_keys_stub_ = true;        // private keys stored locally
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

TEST_CASE("Enabled + foreign identity on an unconfirmed device → Recover", "[shell][encryption]")
{
    // What the old "verify this device" banner used to prompt for now opens
    // the one dialog.
    ShellEncryptionSetupTestShell shell;
    shell.recovery_state_stub_ = 2;
    shell.identity_exists_stub_ = true;
    shell.have_keys_stub_ = false;
    shell.device_verified_stub_ = false;
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("A snoozed reminder also holds back the automatic dialog", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    shell.my_user_id_ = "@snooze-test:example.org";
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

TEST_CASE("encryption_setup_shown_ guards against double-raise", "[shell][encryption]")
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

TEST_CASE("encryption_setup_dismissed_ prevents overlay from showing", "[shell][encryption]")
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

TEST_CASE("Reminder: fresh account without recovery → SetupNeeded", "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(1, true, false) == Reminder::SetupNeeded);
    CHECK(EncryptionFlowController::reminder_for(1, false, false) == Reminder::SetupNeeded);
}

TEST_CASE("Reminder: unconfirmed device on a foreign identity → Locked", "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(2, false, true) == Reminder::Locked);
    CHECK(EncryptionFlowController::reminder_for(1, false, true) == Reminder::Locked);
    CHECK(EncryptionFlowController::reminder_for(0, false, true) == Reminder::Locked);
}

TEST_CASE("Reminder: Incomplete recovery → Locked only while unconfirmed", "[encryption][flow]")
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

TEST_CASE("Incoming requests are refused only while the dialog is busy", "[encryption][flow]")
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
    s->client = std::make_unique<tesseract::Client>();
    return s;
}
} // namespace

TEST_CASE("An incoming request for a background account switches to it and "
          "raises the window",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    auto bob = make_account("@bob:example.org");
    shell.am_.add_account(alice);
    shell.am_.add_account(bob);
    shell.active_account_ = alice;

    shell.handle_verification_request_ui_("@bob:example.org", "flow1", "@bob:example.org", "PHONE",
                                          /*incoming=*/true);
    REQUIRE(shell.switched_to_.size() == 1);
    CHECK(shell.switched_to_[0] == "@bob:example.org");
    CHECK(shell.raised_ == 1);
}

TEST_CASE("A request for the active account doesn't switch or raise", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    shell.am_.add_account(alice);
    shell.active_account_ = alice;

    shell.handle_verification_request_ui_("@alice:example.org", "flow1", "@alice:example.org",
                                          "PHONE", true);
    CHECK(shell.switched_to_.empty());
    CHECK(shell.raised_ == 0);
}

TEST_CASE("Background-account events that aren't new requests are ignored", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    auto bob = make_account("@bob:example.org");
    shell.am_.add_account(alice);
    shell.am_.add_account(bob);
    shell.active_account_ = alice;

    // An accepted outgoing flow, and a request for an account we don't have.
    shell.handle_verification_request_ui_("@bob:example.org", "flow1", "@bob:example.org", "PHONE",
                                          /*incoming=*/false);
    shell.handle_verification_request_ui_("@carol:example.org", "flow2", "@carol:example.org",
                                          "PHONE", true);
    CHECK(shell.switched_to_.empty());
    CHECK(shell.raised_ == 0);
}

TEST_CASE("Another user's request to a background account doesn't switch or raise",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    auto alice = make_account("@alice:example.org");
    auto bob = make_account("@bob:example.org");
    shell.am_.add_account(alice);
    shell.am_.add_account(bob);
    shell.active_account_ = alice;

    // @mallory asks background account @bob to verify, in a shared room.
    shell.handle_verification_request_ui_("@bob:example.org", "flow1", "@mallory:example.org",
                                          "LAPTOP", true);
    CHECK(shell.switched_to_.empty());
    CHECK(shell.raised_ == 0);
    CHECK(shell.active_account_ == alice);
}

// ── Silent setup for new accounts ────────────────────────────────────────────

namespace
{
// Settings is a process-wide singleton: scrub what these tests write.
struct SettingsScrub
{
    std::string uid;
    ~SettingsScrub()
    {
        auto& s = tesseract::Settings::instance();
        s.recovery_key_unsaved.erase(uid);
        s.save_key_reminder_dismissals.erase(uid);
        s.encryption_reminder_snoozed_until.erase(uid);
        s.save_key_reminder_snoozed_until.erase(uid);
        s.silent_recovery_declined.erase(uid);
    }
};

std::shared_ptr<tesseract::AccountSession> silent_account(ShellEncryptionSetupTestShell& shell,
                                                          const std::string& uid)
{
    auto sess = make_account(uid);
    shell.am_.add_account(sess);
    shell.active_account_ = sess;
    shell.my_user_id_ = uid;
    // Independent of the build's TESSERACT_ENABLE_SILENT_RECOVERY_SETUP.
    shell.silent_recovery_setup_enabled_ = true;
    return sess;
}
} // namespace

TEST_CASE("Recovery is set up silently, without the setup dialog", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new1:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1; // Disabled, no identity elsewhere
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
    CHECK(shell.silent_enables_ == 1);
    CHECK(shell.encryption_setup_shown_);

    // The key lands in the keychain and the account is flagged unsaved; the
    // reminder isn't held back.
    auto& s = tesseract::Settings::instance();
    s.save_key_reminder_snoozed_until[scrub.uid] = shell.now_s_ + 60;
    shell.handle_enable_recovery_progress_ui_(4, "EsTc abcd", 0, 0);
    CHECK(shell.keychain_[scrub.uid] == "EsTc abcd");
    CHECK(s.recovery_key_unsaved.count(scrub.uid) == 1);
    CHECK(s.save_key_reminder_snoozed_until.count(scrub.uid) == 0);
    // Set up from a sync tick (not a first sign-in), so the account isn't
    // brand-new: the user is told what happened.
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::AutoSetupNotice);
    CHECK(shell.silent_recovery_in_flight_.empty());
}

TEST_CASE("Silent setup failure retries after a backoff, then falls back to the dialog",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new2:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;

    constexpr int kMax = ShellEncryptionSetupTestShell::kSilentRecoveryMaxFailures;
    for (int i = 0; i < kMax; ++i)
    {
        shell.check_encryption_setup_();
        REQUIRE(shell.silent_enables_ == i + 1);
        shell.handle_enable_recovery_progress_ui_(5, "boom", 0, 0);
        CHECK_FALSE(shell.encryption_setup_shown_);
        if (i + 1 == kMax)
        {
            break;
        }
        shell.check_encryption_setup_(); // still in the backoff: nothing happens
        CHECK(shell.silent_enables_ == i + 1);
        CHECK_FALSE(shell.overlay_shown_);
        shell.now_s_ += ShellEncryptionSetupTestShell::kSilentRecoveryRetrySeconds;
    }
    // Out of attempts: the dialog takes over on the next tick, no more backoff.
    shell.check_encryption_setup_();
    CHECK(shell.silent_enables_ == kMax);
    CHECK(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

TEST_CASE("Silent setup without a keychain shows the key at once", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new3:example.org"};
    silent_account(shell, scrub.uid);
    shell.keychain_works_ = false;
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    shell.handle_enable_recovery_progress_ui_(4, "KEY", 0, 0);
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::SaveKey);
    CHECK(shell.unstored_recovery_keys_[scrub.uid] == "KEY");
}

TEST_CASE("Gated first login of a new account starts silent setup", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new4:example.org"};
    auto sess = silent_account(shell, scrub.uid);
    sess->sync_started = true; // keep the gate release from starting a real sync
    ShellEncryptionSetupTestShell::FinalizeLoginResult fin;
    fin.ok = true;
    fin.user_id = scrub.uid;
    fin.needs_encryption_setup = true;
    shell.begin_gated_encryption_setup_if_needed_(fin);
    CHECK(shell.silent_enables_ == 1);
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("Saving the key stops the reminder for good", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5:example.org"};
    silent_account(shell, scrub.uid);
    auto& s = tesseract::Settings::instance();
    s.recovery_key_unsaved.insert(scrub.uid);
    s.save_key_reminder_dismissals[scrub.uid] = 2;
    shell.keychain_[scrub.uid] = "KEY";

    shell.open_save_key_dialog_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::SaveKey);

    shell.mark_recovery_key_saved_(scrub.uid, /*keep_on_device=*/false);
    CHECK(s.recovery_key_unsaved.count(scrub.uid) == 0);
    CHECK(s.save_key_reminder_dismissals.count(scrub.uid) == 0);
    CHECK(shell.keychain_.count(scrub.uid) == 0); // the user has their own copy now
}

TEST_CASE("Saving the key can opt in to keeping Tesseract's copy", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5b:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.mark_recovery_key_saved_(scrub.uid, /*keep_on_device=*/true);
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 0);
    CHECK(shell.keychain_[scrub.uid] == "KEY");
}

TEST_CASE("A setup made in the dialog with a passphrase also drops the held key",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5c:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "OLD";
    shell.dialog_recovery_setup_users_.insert(scrub.uid);
    shell.handle_enable_recovery_progress_ui_(4, "", 0, 0); // passphrase: no key
    CHECK(shell.keychain_.count(scrub.uid) == 0);
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 0);
}

TEST_CASE("No setup reminder or second setup while a silent one is pending", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5d:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    REQUIRE(shell.silent_recovery_pending_(scrub.uid));
    shell.reopen_encryption_setup_(); // e.g. the strip's "Set up key storage"
    CHECK_FALSE(shell.overlay_shown_);
    CHECK(shell.silent_enables_ == 1);

    shell.handle_enable_recovery_progress_ui_(5, "boom", 0, 0);
    CHECK(shell.silent_recovery_pending_(scrub.uid)); // waiting out the backoff
    shell.reopen_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
    shell.now_s_ += ShellEncryptionSetupTestShell::kSilentRecoveryRetrySeconds;
    CHECK_FALSE(shell.silent_recovery_pending_(scrub.uid));
}

TEST_CASE("Signing out with an unsaved key offers to save it first", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5e:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";

    int signed_out = 0;
    std::function<void()> sign_out = [&]
    {
        if (shell.intercept_sign_out_for_unsaved_key_(sign_out))
        {
            return;
        }
        ++signed_out;
    };
    sign_out();
    CHECK(signed_out == 0);
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::SaveKey);

    shell.proceed_sign_out_(); // "Sign out without saving"
    CHECK(signed_out == 1);
}

TEST_CASE("Signing out keeps an unsaved key (the only copy) and drops a saved one",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new5f:example.org"};
    silent_account(shell, scrub.uid);
    auto& s = tesseract::Settings::instance();

    s.recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.settle_recovery_key_on_sign_out_(scrub.uid);
    CHECK(shell.keychain_.count(scrub.uid) == 1);
    CHECK(s.recovery_key_unsaved.count(scrub.uid) == 1);

    s.recovery_key_unsaved.erase(scrub.uid); // saved, kept on the device
    shell.settle_recovery_key_on_sign_out_(scrub.uid);
    CHECK(shell.keychain_.count(scrub.uid) == 0);
}

TEST_CASE("A key made in the dialog replaces the one Tesseract held", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new6:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "OLD";
    shell.dialog_recovery_setup_users_.insert(scrub.uid); // the dialog started it
    shell.handle_enable_recovery_progress_ui_(4, "NEW", 0, 0);
    CHECK(shell.keychain_.count(scrub.uid) == 0);
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 0);
}

TEST_CASE("A locked device unlocks silently with the stored key", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new7:example.org"};
    silent_account(shell, scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.recovery_state_stub_ = 3; // Incomplete
    shell.check_encryption_setup_();
    REQUIRE(shell.silent_recovers_.size() == 1);
    CHECK(shell.silent_recovers_[0] == "KEY");
    CHECK_FALSE(shell.overlay_shown_);
    shell.silent_recover_done_(true);
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("A failed silent unlock falls back to the Recover dialog", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new8:example.org"};
    silent_account(shell, scrub.uid);
    shell.keychain_[scrub.uid] = "STALE";
    shell.recovery_state_stub_ = 3;
    shell.check_encryption_setup_();
    REQUIRE(shell.silent_recover_done_);
    shell.silent_recover_done_(false);
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Reminder: unsaved auto-made key → SaveKey, but only once nothing else is wrong",
          "[encryption][flow]")
{
    CHECK(EncryptionFlowController::reminder_for(2, true, false, true) == Reminder::SaveKey);
    CHECK(EncryptionFlowController::reminder_for(2, true, false, false) == Reminder::None);
    CHECK(EncryptionFlowController::reminder_for(2, false, true, true) == Reminder::Locked);
    CHECK(EncryptionFlowController::reminder_for(3, false, false, true) == Reminder::Locked);
}

TEST_CASE("SaveKey snooze ladder: 1, 3, 7, then 14 days", "[encryption][flow]")
{
    constexpr std::int64_t d = EncryptionFlowController::kDaySeconds;
    CHECK(EncryptionFlowController::save_key_snooze_seconds(1) == 1 * d);
    CHECK(EncryptionFlowController::save_key_snooze_seconds(2) == 3 * d);
    CHECK(EncryptionFlowController::save_key_snooze_seconds(3) == 7 * d);
    CHECK(EncryptionFlowController::save_key_snooze_seconds(4) == 14 * d);
    CHECK(EncryptionFlowController::save_key_snooze_seconds(40) == 14 * d);
}

TEST_CASE("Silent setup stays pending until its key is stored", "[shell][encryption]")
{
    struct SlowKeychainShell : ShellEncryptionSetupTestShell
    {
        std::function<void(bool)> pending_store_;
        void store_recovery_key_(const std::string&, const std::string&,
                                 std::function<void(bool)> done) override
        {
            pending_store_ = std::move(done);
        }
    };
    SlowKeychainShell shell;
    SettingsScrub scrub{"@new9:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    shell.handle_enable_recovery_progress_ui_(4, "KEY", 0, 0);
    CHECK(shell.silent_recovery_pending_(scrub.uid)); // storing: no "set up" strip
    REQUIRE(shell.pending_store_);
    shell.pending_store_(true);
    CHECK_FALSE(shell.silent_recovery_pending_(scrub.uid));
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 1);
}

TEST_CASE("A successful unlock keeps the key Tesseract holds", "[shell][encryption]")
{
    // recover() reports success as progress step 4 with no key, the same
    // event a passphrase setup sends; only the dialog's setup replaces it.
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new10:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.handle_enable_recovery_progress_ui_(4, "", 0, 0);
    CHECK(shell.keychain_[scrub.uid] == "KEY");
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 1);
}

TEST_CASE("The 'set up recovery' reminder waits until silent setup gives up", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new11:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;
    CHECK_FALSE(shell.silent_recovery_exhausted_(scrub.uid));
    for (int i = 0; i < ShellEncryptionSetupTestShell::kSilentRecoveryMaxFailures; ++i)
    {
        shell.check_encryption_setup_();
        shell.handle_enable_recovery_progress_ui_(5, "boom", 0, 0);
        shell.now_s_ += ShellEncryptionSetupTestShell::kSilentRecoveryRetrySeconds;
    }
    CHECK(shell.silent_recovery_exhausted_(scrub.uid));
}

TEST_CASE("Silent setup of an existing account explains what happened", "[shell][encryption]")
{
    // Not via the first-login gate: an account that already had history here.
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@old1:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    shell.handle_enable_recovery_progress_ui_(4, "KEY", 0, 0);
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::AutoSetupNotice);
}

TEST_CASE("Silent setup at a brand-new account's first sign-in shows no notice",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new12:example.org"};
    auto sess = silent_account(shell, scrub.uid);
    sess->sync_started = true; // keep the gate release from starting a real sync
    ShellEncryptionSetupTestShell::FinalizeLoginResult fin;
    fin.ok = true;
    fin.user_id = scrub.uid;
    fin.needs_encryption_setup = true;
    shell.begin_gated_encryption_setup_if_needed_(fin);
    shell.handle_enable_recovery_progress_ui_(4, "KEY", 0, 0);
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("An unreadable keychain keeps the unsaved key's state", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new13:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.keychain_readable_ = false;
    shell.open_save_key_dialog_();
    CHECK_FALSE(shell.overlay_shown_);
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 1);

    // Signing out still keeps the key (the only copy).
    shell.settle_recovery_key_on_sign_out_(scrub.uid);
    CHECK(shell.keychain_.count(scrub.uid) == 1);
}

TEST_CASE("An expired session skips the save-your-key offer", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new14:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    shell.skip_unsaved_key_check_for_next_sign_out_();
    CHECK_FALSE(shell.intercept_sign_out_for_unsaved_key_([] {}));
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("Signing out from the SaveKey dialog only signs out the account it was for",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new15:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    int signed_out = 0;
    REQUIRE(shell.intercept_sign_out_for_unsaved_key_(
        [&]
        {
            ++signed_out;
        }));
    CHECK(shell.save_key_dialog_uid_ == scrub.uid);

    silent_account(shell, "@other:example.org"); // switched accounts meanwhile
    shell.proceed_sign_out_();
    CHECK(signed_out == 0);
}

TEST_CASE("Dismissing the SaveKey reminder doesn't snooze the other reminders",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new16:example.org"};
    silent_account(shell, scrub.uid);
    auto& s = tesseract::Settings::instance();
    s.recovery_key_unsaved.insert(scrub.uid);
    shell.snooze_save_key_reminder_(scrub.uid);
    CHECK(s.save_key_reminder_snoozed_until.count(scrub.uid) == 1);
    CHECK(s.encryption_reminder_snoozed_until.count(scrub.uid) == 0);
    CHECK(s.save_key_reminder_dismissals[scrub.uid] == 1);
}

// ── TESSERACT_ENABLE_SILENT_RECOVERY_SETUP=OFF ───────────────────────────────

TEST_CASE("Silent setup off: a new account gets the setup dialog", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@off1:example.org"};
    silent_account(shell, scrub.uid);
    shell.silent_recovery_setup_enabled_ = false;
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    CHECK(shell.silent_enables_ == 0);
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
}

TEST_CASE("Silent setup off: no silent unlock and no sign-out offer", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@off2:example.org"};
    silent_account(shell, scrub.uid);
    shell.silent_recovery_setup_enabled_ = false;
    shell.keychain_[scrub.uid] = "KEY";
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);

    shell.recovery_state_stub_ = 3; // Incomplete
    shell.check_encryption_setup_();
    CHECK(shell.silent_recovers_.empty());
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);

    CHECK_FALSE(shell.intercept_sign_out_for_unsaved_key_([] {}));
}

TEST_CASE("'Sign out and delete the key' drops an unsaved key", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new17:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().recovery_key_unsaved.insert(scrub.uid);
    shell.keychain_[scrub.uid] = "KEY";
    REQUIRE(shell.intercept_sign_out_for_unsaved_key_(
        [&]
        {
            shell.settle_recovery_key_on_sign_out_(scrub.uid);
        }));
    shell.proceed_sign_out_(/*remove_key=*/true);
    CHECK(shell.keychain_.count(scrub.uid) == 0);
    CHECK(tesseract::Settings::instance().recovery_key_unsaved.count(scrub.uid) == 0);
}

TEST_CASE("An account whose user turned backup off isn't set up silently again",
          "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new18:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().silent_recovery_declined.insert(scrub.uid);
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    CHECK(shell.silent_enables_ == 0);
    CHECK_FALSE(shell.overlay_shown_); // the reminder strip offers it instead
}

TEST_CASE("Setting recovery up from the dialog clears 'backup turned off'", "[shell][encryption]")
{
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new19:example.org"};
    silent_account(shell, scrub.uid);
    tesseract::Settings::instance().silent_recovery_declined.insert(scrub.uid);
    shell.dialog_recovery_setup_users_.insert(scrub.uid);
    shell.handle_enable_recovery_progress_ui_(4, "KEY", 0, 0);
    CHECK(tesseract::Settings::instance().silent_recovery_declined.count(scrub.uid) == 0);
}

TEST_CASE("Backup turned off in another client stops silent setup for good", "[shell][encryption]")
{
    // The server reported the user's "backup off" marker before setup ran.
    ShellEncryptionSetupTestShell shell;
    SettingsScrub scrub{"@new20:example.org"};
    silent_account(shell, scrub.uid);
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    REQUIRE(shell.silent_enables_ == 1);
    shell.silent_recovery_declined_(scrub.uid);
    CHECK_FALSE(shell.silent_recovery_pending_(scrub.uid));
    CHECK(tesseract::Settings::instance().silent_recovery_declined.count(scrub.uid) == 1);

    // Later checks, this session or the next, don't try again.
    shell.encryption_setup_shown_ = false;
    shell.check_encryption_setup_();
    CHECK(shell.silent_enables_ == 1);
    CHECK_FALSE(shell.overlay_shown_);
}
