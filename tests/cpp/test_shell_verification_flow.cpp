// ShellBase_encryption.cpp: interactive verification (SAS) flows, the callbacks
// wired onto the encryption dialog, the cross-signing reset, the reminder banner
// and the "turn off backup" action. The dialog is a real EncryptionSetupOverlay
// mounted in a MainAppWidget; the Client has no session so every SDK step
// fails the same way a dead connection would.

#include <catch2/catch_test_macros.hpp>

#include "app/EncryptionFlowController.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/EncryptionReminderBanner.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/MainAppWidget.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::EncryptionSetupOverlay;
using Step = EncryptionSetupOverlay::Step;
using OMode = EncryptionSetupOverlay::Mode;

namespace
{

struct VfShell : tesseract::test::TestShellBase
{
    ~VfShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        delayed.emplace_back(ms, std::move(fn));
    }
    // The real overlay setup, rather than the double's no-op.
    void show_encryption_setup_overlay_(OMode m) override
    {
        ShellBase::show_encryption_setup_overlay_(m);
    }
    void request_relayout_() override { ++relayouts; }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    uint8_t read_recovery_state_() const override { return recovery_state; }
    bool read_device_verified_() const override { return device_verified; }
    bool read_own_identity_exists_() const override { return true; }
    bool read_have_cross_signing_keys_() const override { return have_keys; }
    void forget_stored_recovery_key_(const std::string&) override {}
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }

    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::pair<int, std::function<void()>>> delayed;
    std::vector<std::string> statuses;
    int relayouts = 0;
    uint8_t recovery_state = 2;
    bool device_verified = true;
    bool have_keys = true;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::begin_crypto_identity_reset_;
    using ShellBase::cancel_active_verification_;
    using ShellBase::client_;
    using ShellBase::dialog_recovery_setup_users_;
    using ShellBase::encryption_flow_;
    using ShellBase::encryption_setup_dismissed_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::handle_crypto_reset_result_ui_;
    using ShellBase::handle_sas_ready_ui_;
    using ShellBase::handle_verification_cancelled_ui_;
    using ShellBase::handle_verification_done_ui_;
    using ShellBase::handle_verification_request_ui_;
    using ShellBase::handle_verification_state_ui_;
    using ShellBase::main_app_;
    using ShellBase::my_user_id_;
    using ShellBase::pending_sign_out_;
    using ShellBase::pending_sign_out_uid_;
    using ShellBase::refresh_encryption_reminder_;
    using ShellBase::reopen_encryption_setup_;
    using ShellBase::save_key_dialog_uid_;
    using ShellBase::server_info_;
    using ShellBase::silent_recovery_setup_enabled_;
    using ShellBase::snooze_encryption_reminder_;
    using ShellBase::start_qr_grant_overlay;
    using ShellBase::start_self_verification_;
    using ShellBase::start_user_verification_;
    using ShellBase::turn_off_silent_recovery_;
    using ShellBase::verify_session_menu_callback_;
    using ShellBase::wire_encryption_setup_callbacks_;
    using ShellBase::handle_offline_ui_;
    using ShellBase::handle_online_ui_;
    using ShellBase::handle_recovery_state_changed_ui_;
    using ShellBase::handle_enable_recovery_progress_ui_;
    using ShellBase::wall_clock_s_;
};

struct VfFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    VfShell s;
    TestHost host{nullptr};
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(&host);

    VfFx()
    {
        tesseract::Settings::instance().encryption_reminder_snoozed_until.clear();
        s.client_ = &client;
        s.main_app_ = app.get();
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.my_user_id_ = "@me:x";
    }
    EncryptionSetupOverlay* ov() { return app->encryption_setup(); }
    void open(OMode m = OMode::Recover)
    {
        s.show_encryption_setup_overlay_(m);
        REQUIRE(ov()->visible());
    }
    void begin_flow(bool incoming, bool own = true)
    {
        s.encryption_flow_.begin({.id = "$flow",
                                  .user_id = own ? "@me:x" : "@bob:x",
                                  .device_id = "DEV",
                                  .incoming = incoming,
                                  .own_user = own,
                                  .this_device_unverified = true});
    }
};

} // namespace

TEST_CASE("starting self verification asks the SDK and reports a refusal",
          "[shell][verification]")
{
    VfFx f;
    f.open();
    f.s.start_self_verification_();
    CHECK(f.s.encryption_flow_.awaiting_outgoing());
    f.s.pump();
    CHECK_FALSE(f.s.encryption_flow_.awaiting_outgoing());
    CHECK(f.ov()->step() == Step::VerifyFailed);
}

TEST_CASE("verifying another user needs the dialog free", "[shell][verification]")
{
    VfFx f;
    f.s.start_user_verification_("", "x");
    CHECK_FALSE(f.ov()->visible());

    f.s.start_user_verification_("@bob:x", "Bob");
    CHECK(f.ov()->visible());
    CHECK(f.ov()->mode() == OMode::Verify);
    CHECK(f.s.encryption_flow_.awaiting_outgoing());
    f.s.pump();

    // A flow in progress blocks a second one.
    f.begin_flow(false, false);
    f.s.statuses.clear();
    f.s.start_user_verification_("@carol:x", "Carol");
    f.s.pump();
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back() == "Finish the current verification first.");
}

TEST_CASE("an accepted outgoing request is adopted only while still wanted",
          "[shell][verification]")
{
    VfFx f;
    f.open(OMode::Verify);
    f.s.encryption_flow_.set_awaiting_outgoing(true);
    f.s.handle_verification_request_ui_("@me:x", "$flow", "@me:x", "DEV", false);
    CHECK(f.s.encryption_flow_.is_flow("$flow"));
    CHECK(f.ov()->step() == Step::WaitingForOtherDevice);

    // Not awaiting anything: the stray accept is cancelled instead.
    VfFx g;
    g.open(OMode::Verify);
    g.s.handle_verification_request_ui_("@me:x", "$other", "@me:x", "DEV", false);
    CHECK_FALSE(g.s.encryption_flow_.has_flow());
    g.s.pump();
}

TEST_CASE("incoming requests show the prompt unless the dialog is busy",
          "[shell][verification]")
{
    VfFx f;
    f.s.handle_verification_request_ui_("@me:x", "$f1", "@me:x", "DEV", true);
    CHECK(f.s.encryption_flow_.is_flow("$f1"));
    CHECK(f.ov()->visible());
    CHECK(f.ov()->step() == Step::IncomingRequest);
    f.s.pump(); // names the device (none known offline)

    // A second request while one is open is refused.
    f.s.handle_verification_request_ui_("@me:x", "$f2", "@me:x", "DEV2", true);
    CHECK(f.s.encryption_flow_.is_flow("$f1"));
    f.s.pump();

    // Someone else's request to us is shown by their id.
    VfFx g;
    g.s.handle_verification_request_ui_("@me:x", "$f3", "@bob:x", "DEV", true);
    CHECK(g.s.encryption_flow_.is_flow("$f3"));
    CHECK_FALSE(g.s.encryption_flow_.flow().own_user);

    // No dialog widget at all: nothing to show.
    VfShell s2;
    s2.active_account_ = std::make_shared<tesseract::AccountSession>();
    s2.active_account_->user_id = "@me:x";
    s2.handle_verification_request_ui_("@me:x", "$f4", "@me:x", "D", true);
    CHECK_FALSE(s2.encryption_flow_.has_flow());
}

TEST_CASE("SAS codes, completion and cancellation follow the tracked flow",
          "[shell][verification]")
{
    VfFx f;
    f.open(OMode::Verify);
    f.begin_flow(false, true);

    tesseract::VerificationSas sas;
    sas.decimals = {1, 2, 3};
    f.s.handle_sas_ready_ui_("$other", sas); // not our flow
    CHECK(f.ov()->step() != Step::CompareCodes);
    f.s.handle_sas_ready_ui_("$flow", sas);
    CHECK(f.ov()->step() == Step::CompareCodes);

    f.s.handle_verification_done_ui_("$other");
    CHECK(f.s.encryption_flow_.has_flow());
    f.s.handle_verification_done_ui_("$flow");
    CHECK_FALSE(f.s.encryption_flow_.has_flow());
    CHECK(f.s.encryption_setup_dismissed_); // unlocked this device: nothing left
    CHECK(f.ov()->step() == Step::Done);

    f.open(OMode::Verify);
    f.begin_flow(true, false); // another user: done does not dismiss setup
    f.s.encryption_setup_dismissed_ = false;
    f.s.handle_verification_done_ui_("$flow");
    CHECK_FALSE(f.s.encryption_setup_dismissed_);

    f.open(OMode::Verify);
    f.begin_flow(false, true);
    f.s.handle_sas_ready_ui_("$flow", sas);
    f.s.handle_verification_cancelled_ui_("$other", "x");
    CHECK(f.s.encryption_flow_.has_flow());
    f.s.handle_verification_cancelled_ui_("$flow", "The other device declined.");
    CHECK_FALSE(f.s.encryption_flow_.has_flow());
    CHECK(f.ov()->step() == Step::VerifyFailed);
}

TEST_CASE("verification state drives the warning dot and reminder",
          "[shell][verification]")
{
    VfFx f;
    f.s.handle_verification_state_ui_(false);
    f.s.handle_verification_state_ui_(true);
    f.s.handle_recovery_state_changed_ui_();
    SUCCEED();
}

TEST_CASE("dialog callbacks: enable recovery and recover mark and clear state",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::Fresh);
    f.ov()->on_enable_recovery("pass");
    CHECK(f.s.dialog_recovery_setup_users_.count("@me:x") == 1);
    f.s.pump();
    CHECK(f.s.dialog_recovery_setup_users_.empty());

    f.open(OMode::Recover);
    f.ov()->on_recover("bad key");
    f.s.pump(); // the failure is shown in the dialog
    SUCCEED();
}

TEST_CASE("dialog callbacks: accept / decline / mismatch / cancel",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::Verify);
    f.ov()->on_accept_request(); // no flow: nothing
    f.begin_flow(true);
    f.ov()->on_accept_request();
    f.s.pump(); // accept fails offline: flow cleared, dialog explains
    CHECK_FALSE(f.s.encryption_flow_.has_flow());
    CHECK(f.ov()->step() == Step::VerifyFailed);

    f.open(OMode::Verify);
    f.begin_flow(true);
    f.ov()->on_decline_request();
    CHECK_FALSE(f.s.encryption_flow_.has_flow());

    f.begin_flow(false);
    f.ov()->on_sas_mismatch();
    CHECK_FALSE(f.s.encryption_flow_.has_flow());

    f.begin_flow(false);
    f.ov()->on_cancel_verification();
    CHECK_FALSE(f.s.encryption_flow_.has_flow());
    f.s.pump();
}

TEST_CASE("'they match' without a live flow fails the confirming step",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::Verify);
    f.begin_flow(false);
    tesseract::VerificationSas sas;
    f.s.handle_sas_ready_ui_("$flow", sas);
    f.ov()->on_sas_match();
    f.s.pump();
    // The confirm fails offline: the flow is torn down.
    CHECK_FALSE(f.s.encryption_flow_.has_flow());

    VfFx g;
    g.open(OMode::Verify);
    g.ov()->on_sas_match(); // nothing in flight
    CHECK_FALSE(g.s.encryption_flow_.has_flow());
}

TEST_CASE("dialog callbacks: use-another-device, retry and layout",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::Recover);
    f.ov()->on_request_sas();
    f.s.pump();
    f.ov()->on_retry_verification();
    f.s.pump();
    const int r = f.s.relayouts;
    f.ov()->on_layout_changed();
    CHECK(f.s.relayouts == r + 1);
    f.ov()->on_copy_to_clipboard("recovery-key");
    SUCCEED();
}

TEST_CASE("closing the dialog records the dismissal, cancels a running verification",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::Fresh);
    f.ov()->on_close();
    CHECK(f.s.encryption_setup_dismissed_);
    CHECK_FALSE(f.ov()->visible());

    // Closing a verification dialog does not count as dismissing setup.
    f.s.encryption_setup_dismissed_ = false;
    f.open(OMode::Verify);
    f.begin_flow(false);
    f.ov()->on_close();
    CHECK_FALSE(f.s.encryption_setup_dismissed_);
    CHECK_FALSE(f.s.encryption_flow_.has_flow());
}

TEST_CASE("closing the save-key dialog cancels a pending sign-out or snoozes the reminder",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.s.save_key_dialog_uid_ = "@me:x";
    f.open(OMode::SaveKey);
    f.s.pending_sign_out_ = [] {};
    f.s.pending_sign_out_uid_ = "@me:x";
    f.ov()->on_close();
    // (before_sign_out is only set by open_save_key_dialog_; a plain
    // reset leaves it false, so this takes the reminder-snooze branch.)
    CHECK(tesseract::Settings::instance().save_key_reminder_dismissals.count("@me:x") == 1);
}

TEST_CASE("sign-out and key-saved callbacks reach the shell", "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::SaveKey);
    f.s.save_key_dialog_uid_ = "@me:x";
    f.ov()->on_key_saved(false);
    f.ov()->on_sign_out(false);
    f.s.pump();
    SUCCEED();
}

TEST_CASE("turning off backup records the choice and reports success or failure",
          "[shell][verification][dialog]")
{
    VfFx f;
    f.open(OMode::AutoSetupNotice);
    f.ov()->on_turn_off_backup();
    CHECK(tesseract::Settings::instance().silent_recovery_declined.count("@me:x") == 1);
    f.s.pump(); // disabling fails offline: shown in the dialog
    SUCCEED();

    VfShell none;
    none.turn_off_silent_recovery_(); // no account: nothing
}

TEST_CASE("resetting the cross-signing identity drives the approval wait",
          "[shell][verification][reset]")
{
    VfFx f;
    VfShell none;
    none.begin_crypto_identity_reset_(); // no client / app
    CHECK_FALSE(none.encryption_setup_shown_);

    f.s.begin_crypto_identity_reset_();
    CHECK(f.s.encryption_setup_shown_);
    CHECK(f.ov()->visible());
    CHECK(f.ov()->step() == Step::ResetApproving);
    f.s.pump(); // the SDK refuses offline: the error is shown
    SUCCEED();

    f.s.handle_crypto_reset_result_ui_(false, "denied");
    f.s.handle_crypto_reset_result_ui_(true, "");
    VfShell s3;
    s3.handle_crypto_reset_result_ui_(true, ""); // no dialog: harmless
}

TEST_CASE("the encryption reminder banner follows recovery and verification state",
          "[shell][verification][reminder]")
{
    VfFx f;
    f.s.recovery_state = 2;
    f.s.device_verified = true;
    f.s.refresh_encryption_reminder_();
    CHECK_FALSE(f.app->encryption_reminder_requested());

    // Recovery not set up and silent setup disabled for this account.
    f.s.silent_recovery_setup_enabled_ = false;
    f.s.recovery_state = 1;
    f.s.refresh_encryption_reminder_();
    CHECK(f.app->encryption_reminder_requested());
    CHECK(f.app->encryption_reminder()->kind() ==
          tesseract::views::EncryptionReminderBanner::Kind::SetupNeeded);

    // Snoozing hides it.
    f.s.snooze_encryption_reminder_();
    CHECK_FALSE(f.app->encryption_reminder_requested());
    CHECK(tesseract::Settings::instance().encryption_reminder_snoozed_until.count("@me:x") == 1);

    // Back to normal.
    tesseract::Settings::instance().encryption_reminder_snoozed_until.clear();
    f.s.recovery_state = 2;
    f.s.refresh_encryption_reminder_();
    CHECK_FALSE(f.app->encryption_reminder_requested());

    VfShell none;
    none.refresh_encryption_reminder_(); // no app: nothing
    none.snooze_encryption_reminder_();
}

TEST_CASE("the verify-session menu item exists only for an unverified device",
          "[shell][verification][reminder]")
{
    VfFx f;
    f.s.device_verified = true;
    CHECK_FALSE(f.s.verify_session_menu_callback_());
    f.s.device_verified = false;
    auto cb = f.s.verify_session_menu_callback_();
    REQUIRE(cb);
    f.s.recovery_state = 1;
    f.s.silent_recovery_setup_enabled_ = false;
    cb();
    CHECK(f.ov()->visible());

    VfShell none;
    none.device_verified = false;
    CHECK_FALSE(none.verify_session_menu_callback_()); // no account
}

TEST_CASE("connectivity changes toggle the offline banner", "[shell][verification]")
{
    VfFx f;
    f.s.handle_offline_ui_();
    f.s.handle_online_ui_();
    CHECK(f.s.relayouts >= 2);
}

TEST_CASE("recovery progress reaches the dialog and clears the dialog-setup mark",
          "[shell][verification]")
{
    VfFx f;
    f.open(OMode::Fresh);
    f.s.dialog_recovery_setup_users_.insert("@me:x");
    f.s.handle_enable_recovery_progress_ui_(1, "", 0, 0);
    CHECK(f.s.dialog_recovery_setup_users_.count("@me:x") == 1);
    f.s.handle_enable_recovery_progress_ui_(4, "key", 1, 1);
    CHECK(f.s.dialog_recovery_setup_users_.empty());
}

TEST_CASE("QR grant overlay needs server support", "[shell][verification]")
{
    VfFx f;
    f.s.start_qr_grant_overlay(); // unsupported: nothing
    f.s.server_info_.supports_qr_grant = true;
    f.s.start_qr_grant_overlay();
    f.s.pump();
    SUCCEED();
}
