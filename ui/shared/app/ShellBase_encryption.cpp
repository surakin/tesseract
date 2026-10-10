#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/Launch.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/MediaPlaybackHub.h"
#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/SearchBackend.h"
#include "app/SlashCommands.h"
#include "app/shell_helpers.h"
#include "app/UnreadPrefetch.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/JoinRoomView.h"
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/InviteDialog.h"
#include "views/text_util.h"
#include "views/RoomSearchBar.h"
#include "views/SettingsView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "tk/image_sniff.h"
#include "views/html_spans.h"
#include "views/image_pack_order.h"
#include "views/map_tiles.h"
#include "views/pronoun_utils.h"
#include "views/thread_unread.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/secret_store.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace tesseract
{

// ── Encryption setup detection ───────────────────────────────────────────────

uint8_t ShellBase::read_recovery_state_() const
{
    return client_ ? static_cast<uint8_t>(client_->recovery_state()) : 0u;
}

bool ShellBase::read_own_identity_exists_() const
{
    return client_ ? client_->own_identity_exists() : false;
}

bool ShellBase::read_device_verified_() const
{
    return client_ ? client_->device_verified() : false;
}

bool ShellBase::read_have_cross_signing_keys_() const
{
    return client_ ? client_->have_cross_signing_keys() : false;
}

// True when a cross-signing identity exists for our user but its private
// keys are NOT held locally — i.e. the identity was created on another
// device and this one must verify/recover against it (vs. a fresh first
// device whose own login-time bootstrap holds the keys). Shared by
// check_encryption_setup_ (Fresh vs Recover) and the verification-banner
// gating in the platform shells.
bool ShellBase::foreign_cross_signing_identity_() const
{
    // An identity exists (public part synced) but we don't hold its private
    // keys → it was set up elsewhere; this device must verify/recover against
    // it. On a fresh first device our own login-time bootstrap holds the keys,
    // so this is false even before verification_state() has flipped to Verified.
    return read_own_identity_exists_() && !read_have_cross_signing_keys_();
}

void ShellBase::handle_offline_ui_()
{
    offline_ = true;
    if (main_app_) main_app_->set_offline(true);
    request_relayout_();
}

void ShellBase::handle_online_ui_()
{
    offline_ = false;
    if (main_app_) main_app_->set_offline(false);
    request_relayout_();
}

void ShellBase::handle_enable_recovery_progress_ui_(uint8_t  step,
                                                    std::string recovery_key,
                                                    uint32_t backed_up,
                                                    uint32_t total)
{
    const std::string uid = event_account_.empty() ? my_user_id_ : event_account_;
    if (silent_recovery_in_flight_.count(uid))
    {
        handle_silent_recovery_progress_(uid, step, recovery_key);
        return;
    }
    // Only the dialog's own setup (with a key it shows, or a passphrase)
    // replaces the account's recovery; recover() reports its success as the
    // same step 4 and must leave Tesseract's key alone.
    const bool dialog_setup = dialog_recovery_setup_users_.count(uid) > 0;
    if (step == 4 || step == 5) dialog_recovery_setup_users_.erase(uid);
    if (step == 4 && dialog_setup)
    {
        // Any key Tesseract was holding for this account no longer unlocks
        // anything.
        forget_stored_recovery_key_(uid);
        clear_unsaved_recovery_key_state_(uid);
        // Set up by the user: backup is wanted again.
        auto& s = Settings::instance();
        if (s.silent_recovery_declined.erase(uid) > 0) s.save_to_disk(tesseract::config_dir());
    }
    if (auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr)
        ov->advance_progress(step, recovery_key, backed_up, total);
}

namespace
{
// Backstop for the dialog's Confirming step (after "They match").
constexpr int kConfirmTimeoutMs = 30'000;
} // namespace

void ShellBase::wire_encryption_setup_callbacks_(
    views::EncryptionSetupOverlay& ov, tk::Host& host)
{
    tk::Host* host_ptr = &host;

    ov.on_enable_recovery = [this](std::string passphrase) {
        auto sess = active_account_;
        if (!sess) return;
        dialog_recovery_setup_users_.insert(sess->user_id);
        run_async_mut_([this, sess, passphrase]() {
            if (sess->client) sess->client->enable_recovery(passphrase);
            // Its progress events (a final step 4 or 5 included) were posted
            // before it returned; an early error posts none, so clear the
            // mark here rather than relying on them.
            post_to_ui_alive_([this, uid = sess->user_id]() {
                dialog_recovery_setup_users_.erase(uid);
            });
        });
    };

    ov.on_recover = [this](std::string key) {
        auto sess = active_account_;
        run_async_mut_([this, sess, key]() {
            if (!sess || !sess->client) return;
            auto res = sess->client->recover(key);
            if (!res.ok)
            {
                post_to_ui_alive_([this, msg = std::string(res.message)]() {
                    if (auto* o =
                            main_app_ ? main_app_->encryption_setup() : nullptr)
                        o->advance_progress(5, msg, 0, 0);
                });
            }
        });
    };

    // "Use another device" / "Try again": the dialog has already moved to
    // its waiting step. A gated first sync stays gated — the encryption-only
    // presync (Client::start_encryption_sync) carries the to-device traffic.
    ov.on_request_sas = [this]() { start_self_verification_(); };
    ov.on_retry_verification = [this]() { start_outgoing_verification_(); };

    ov.on_cancel_verification = [this]() { cancel_active_verification_(); };

    ov.on_accept_request = [this]() {
        if (!encryption_flow_.has_flow()) return;
        const std::string fid = encryption_flow_.flow().id;
        auto sess = active_account_;
        run_async_mut_([this, sess, fid]() {
            if (!sess || !sess->client) return;
            auto r = sess->client->accept_verification(fid);
            if (r.ok) r = sess->client->start_sas(fid);
            if (r.ok) return;
            post_to_ui_alive_([this, fid, msg = std::string(r.message)]() {
                if (!encryption_flow_.is_flow(fid)) return;
                encryption_flow_.clear();
                if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
                    o->verification_failed(msg, false);
                request_relayout_();
            });
        });
    };

    ov.on_decline_request = [this]() {
        cancel_active_verification_();
        // Back to where the user was (Recover's chooser), or close.
        if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
            o->return_to_start();
        request_relayout_();
    };

    // The dialog is now on Confirming, which has no button of its own: every
    // way out of it must come from here (a failed confirm, or the backstop
    // timeout) or from the SDK's done / cancelled events.
    ov.on_sas_match = [this]() {
        auto fail = [this](const std::string& fid, std::string msg) {
            if (!fid.empty() && !encryption_flow_.is_flow(fid)) return; // resolved
            cancel_active_verification_();
            if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
                o && o->step() == views::EncryptionSetupOverlay::Step::Confirming)
                o->verification_failed(std::move(msg), false);
            request_relayout_();
        };
        if (!encryption_flow_.has_flow())
        {
            fail({}, tk::tr("This verification is no longer active."));
            return;
        }
        const std::string fid = encryption_flow_.flow().id;
        auto sess = active_account_;
        run_async_mut_([this, sess, fid, fail]() {
            if (!sess || !sess->client) return;
            auto r = sess->client->confirm_sas(fid);
            if (r.ok) return;
            post_to_ui_alive_([fid, fail, msg = std::string(r.message)]() { fail(fid, msg); });
        });
        post_to_ui_after_(kConfirmTimeoutMs, guarded([fid, fail]() {
            fail(fid, tk::tr("The other device didn't finish confirming in time."));
        }));
    };

    // The dialog shows its own "didn't match" explanation; the SDK's cancel
    // echo that follows is then ignored (verification_failed is idempotent).
    ov.on_sas_mismatch = [this]() { cancel_active_verification_(); };

    ov.on_reset_encryption = [this]() { begin_crypto_identity_reset_(); };

    ov.on_key_saved = [this](bool keep_on_device) {
        mark_recovery_key_saved_(save_key_dialog_uid_, keep_on_device);
    };
    ov.on_sign_out = [this](bool remove_key) {
        proceed_sign_out_(remove_key);
        request_relayout_();
    };
    ov.on_turn_off_backup = [this]() { turn_off_silent_recovery_(); };

    ov.on_close = [this]() {
        auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
        // Closing mid-verification abandons it on both sides.
        if (o && o->in_verification_step()) cancel_active_verification_();
        // A dismissed *setup* isn't raised again automatically this session
        // (the reminder strip takes over); answering a request isn't setup.
        if (!o || o->mode() != views::EncryptionSetupOverlay::Mode::Verify)
            encryption_setup_dismissed_ = true;
        if (o && o->mode() == views::EncryptionSetupOverlay::Mode::SaveKey)
        {
            if (o->before_sign_out())
                cancel_sign_out_(); // closing it cancels the sign-out
            else if (o->step() != views::EncryptionSetupOverlay::Step::Done)
                snooze_save_key_reminder_(save_key_dialog_uid_); // "Remind me later"
        }
        if (main_app_) main_app_->show_encryption_setup(false);
        release_pending_sync_gate_();
        refresh_encryption_reminder_();
        request_relayout_();
    };

    ov.on_copy_to_clipboard = [host_ptr](std::string text) {
        host_ptr->set_clipboard_text(text);
    };

    if (has_save_file_dialog_())
    {
        ov.on_save_to_file = [this](std::string key) {
            pick_save_file_(
                tk::tr("Save recovery key"), "tesseract-recovery-key.txt",
                [this, key = std::move(key)](std::string path) {
                    std::string error;
                    const bool ok = shell_helpers::write_private_text_file(path, key + "\n", error);
                    if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
                        o->key_save_result(ok, error);
                    request_relayout_();
                });
        };
    }

    ov.on_layout_changed = [this]() { request_relayout_(); };
}

void ShellBase::start_qr_grant_overlay()
{
    if (!client_ || !main_app_) return;
    if (!server_info_.supports_qr_grant) return;
    auto* view = main_app_->qr_grant_view();
    if (!view) return;

    view->set_client(client_);
    view->set_post_to_ui([this](auto fn) { post_to_ui_(std::move(fn)); });
    view->set_run_async([this](auto fn) { run_async_mut_(std::move(fn)); });
    view->set_relayout([this] { request_relayout_(); });
    view->set_open_browser([](const std::string& url) {
        tesseract::Client::open_in_browser(url);
    });
    view->set_on_done([this] {
        if (main_app_) main_app_->show_qr_grant(false);
        request_relayout_();
    });
    view->set_on_cancel([this] {
        if (main_app_) main_app_->show_qr_grant(false);
        request_relayout_();
    });

    if (main_app_) main_app_->show_qr_grant(true);
    request_relayout_();
    view->start();
}

void ShellBase::check_encryption_setup_()
{
    if (encryption_setup_shown_ || encryption_setup_dismissed_)
        return;
    // "Remind me later" on the reminder strip covers the automatic dialog too.
    {
        const auto& snoozes = Settings::instance().encryption_reminder_snoozed_until;
        auto it = snoozes.find(my_user_id_);
        if (it != snoozes.end() &&
            EncryptionFlowController::snoozed(it->second, wall_clock_s_()))
            return;
    }

    // A silent setup is running or waiting to retry: nothing to look up.
    if (active_account_ && silent_recovery_pending_(active_account_->user_id))
        return;

    using Mode      = tesseract::views::EncryptionSetupOverlay::Mode;
    const uint8_t state = read_recovery_state_();

    if (state == 1) // Disabled → secret storage not set up on this account
    {
        // Disabled is ambiguous: it covers a truly fresh account (no
        // cross-signing anywhere → bootstrap a new identity via the Fresh
        // path) AND an account whose cross-signing identity was set up on
        // another device but without server-side secret storage. In the
        // latter case this device can't bootstrap over the existing identity;
        // the Fresh path would only create an inconsistent secret store and
        // would never verify this device. Route those to Recover (enter the
        // recovery key, or hand off to SAS) instead.
        //
        // The identity is "foreign" when it exists but we don't hold its
        // private cross-signing keys — it was bootstrapped on another device.
        // Using key-presence (not device_verified()) avoids a false Recover on
        // a fresh first device: our own login-time bootstrap stores the private
        // keys locally immediately, whereas verification_state() may not have
        // flipped to Verified yet at this point.
        const bool foreign_identity = foreign_cross_signing_identity_();
        // A brand-new account: no dialog, Tesseract sets recovery up itself
        // and later reminds the user to save the key.
        if (!foreign_identity && begin_silent_recovery_setup_(active_account_))
            return;
        // The user turned backup off after a silent setup: only the
        // reminder strip offers it again, no dialog.
        if (!foreign_identity && silent_recovery_setup_enabled_ &&
            Settings::instance().silent_recovery_declined.count(my_user_id_))
        {
            encryption_setup_shown_ = true;
            return;
        }
        if (foreign_identity)
        {
            encryption_setup_shown_ = true;
            show_recover_or_silent_unlock_(active_account_);
            return;
        }
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(Mode::Fresh);
    }
    else if (state == 3) // Incomplete → existing encryption, device needs secrets
    {
        encryption_setup_shown_ = true;
        show_recover_or_silent_unlock_(active_account_);
    }
    else if (state == 2 && !read_device_verified_() && foreign_identity_cached_())
    {
        // Recovery is fine account-wide but this device was never confirmed
        // against the identity (what the old "verify this device" banner
        // used to prompt for) — same unlock choices.
        encryption_setup_shown_ = true;
        show_recover_or_silent_unlock_(active_account_);
    }
    // Unknown (0), or Enabled (2) on a confirmed device: nothing to do;
    // re-checked on the next tick.
}

void ShellBase::reopen_encryption_setup_()
{
    encryption_setup_dismissed_ = false;
    encryption_setup_shown_     = false;
    // Tesseract is setting recovery up by itself right now.
    if (silent_recovery_pending_(my_user_id_))
        return;
    // User-initiated: bypass the snooze check_encryption_setup_ honours.
    using Reminder = EncryptionFlowController::Reminder;
    const Reminder kind = EncryptionFlowController::reminder_for(
        read_recovery_state_(), read_device_verified_(),
        foreign_cross_signing_identity_(),
        silent_recovery_setup_enabled_ &&
            Settings::instance().recovery_key_unsaved.count(my_user_id_) > 0);
    if (kind == Reminder::None)
    {
        check_encryption_setup_();
        return;
    }
    if (kind == Reminder::SaveKey)
    {
        open_save_key_dialog_();
        return;
    }
    encryption_setup_shown_ = true;
    show_encryption_setup_overlay_(kind == Reminder::SetupNeeded
                                       ? views::EncryptionSetupOverlay::Mode::Fresh
                                       : views::EncryptionSetupOverlay::Mode::Recover);
}

// ── Silent encryption setup (new accounts) ────────────────────────────────────

bool ShellBase::silent_recovery_exhausted_(const std::string& uid) const
{
    auto f = silent_recovery_failures_.find(uid);
    return f != silent_recovery_failures_.end() && f->second >= kSilentRecoveryMaxFailures;
}

bool ShellBase::silent_recovery_pending_(const std::string& uid) const
{
    if (silent_recovery_in_flight_.count(uid)) return true;
    if (silent_recovery_exhausted_(uid)) return false; // the dialog takes over
    auto r = silent_recovery_retry_after_.find(uid);
    return r != silent_recovery_retry_after_.end() && wall_clock_s_() < r->second;
}

// A brand-new account (recovery Disabled, no identity made elsewhere)
// never sees the setup dialog: Tesseract enables recovery itself, keeps
// the generated key in the OS keychain and flags it in
// Settings::recovery_key_unsaved until the user saves it from the
// SaveKey reminder. Returns false when setup should fall back to the
// dialog (no session, or silent attempts keep failing); true when it
// started or is waiting out its retry backoff.
bool ShellBase::begin_silent_recovery_setup_(const std::shared_ptr<AccountSession>& sess)
{
    if (!silent_recovery_setup_enabled_ || !sess || !sess->client) return false;
    const std::string& uid = sess->user_id;
    if (silent_recovery_exhausted_(uid))
        return false; // keeps failing: let the user drive it from the dialog
    if (Settings::instance().silent_recovery_declined.count(uid))
        return false; // the user turned backup off
    encryption_setup_shown_ = true;
    if (silent_recovery_pending_(uid))
        return true; // in flight, or a later sync tick retries
    silent_recovery_in_flight_.insert(uid);
    run_silent_enable_recovery_(sess);
    return true;
}

void ShellBase::run_silent_enable_recovery_(std::shared_ptr<AccountSession> sess)
{
    // Progress (and the key) arrive through handle_enable_recovery_progress_ui_.
    run_async_mut_("silent-recovery-setup", [this, sess]() {
        if (!sess || !sess->client) return;
        // Respect a "turn backup off" made in any client: enable() would
        // overwrite that marker on the server. If it can't be read, don't
        // risk it now; a later sync tick tries again.
        const auto opted_out = sess->client->backup_disabled_by_user();
        if (!opted_out || *opted_out)
        {
            post_to_ui_alive_([this, uid = sess->user_id, declined = opted_out.has_value()]() {
                if (declined)
                    silent_recovery_declined_(uid);
                else
                    silent_recovery_failed_(uid);
            });
            return;
        }
        const auto res = sess->client->enable_recovery(std::string());
        if (res.ok) return;
        post_to_ui_alive_([this, uid = sess->user_id]() { silent_recovery_failed_(uid); });
    });
}

void ShellBase::handle_silent_recovery_progress_(const std::string& uid, uint8_t step,
                                                 const std::string& key_or_error)
{
    if (step == 5)
    {
        std::fprintf(stderr, "[encryption] silent recovery setup failed: %s\n",
                     key_or_error.c_str());
        silent_recovery_failed_(uid);
        return;
    }
    if (step != 4 || !silent_recovery_in_flight_.count(uid))
        return;
    silent_recovery_failures_.erase(uid);
    silent_recovery_retry_after_.erase(uid);
    if (key_or_error.empty())
    {
        silent_recovery_in_flight_.erase(uid);
        silent_recovery_new_account_.erase(uid);
        return; // recovery is on, but there's no key to hand the user
    }

    // Still "in flight" until the key is stored: in between, the account can
    // still read as having no recovery, and the strip mustn't offer setup.
    store_recovery_key_(uid, key_or_error, [this, uid, key = key_or_error](bool stored) {
        silent_recovery_in_flight_.erase(uid);
        // An existing account (not one just registered) is told what happened.
        const bool tell_user = silent_recovery_new_account_.erase(uid) == 0;
        auto& s = Settings::instance();
        s.recovery_key_unsaved.insert(uid);
        s.save_key_reminder_dismissals.erase(uid);
        // The "save your recovery key" strip shows right away and stays
        // until the user dismisses it.
        s.save_key_reminder_snoozed_until.erase(uid);
        s.save_to_disk(tesseract::config_dir());
        if (!stored)
        {
            // No secure storage to hold it: this is the only copy, so show it
            // right away.
            unstored_recovery_keys_[uid] = key;
            if (my_user_id_ == uid)
                open_save_key_dialog_();
            return;
        }
        refresh_encryption_reminder_();
        if (tell_user && my_user_id_ == uid)
        {
            encryption_setup_shown_ = true;
            show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode::AutoSetupNotice);
        }
    });
}

void ShellBase::silent_recovery_declined_(const std::string& uid)
{
    if (!silent_recovery_in_flight_.erase(uid)) return;
    silent_recovery_new_account_.erase(uid);
    // The user turned backup off (in some client): never set it up silently;
    // the "set up recovery" strip still offers it.
    auto& s = Settings::instance();
    if (s.silent_recovery_declined.insert(uid).second)
        s.save_to_disk(tesseract::config_dir());
    refresh_encryption_reminder_();
}

void ShellBase::silent_recovery_failed_(const std::string& uid)
{
    if (!silent_recovery_in_flight_.erase(uid))
        return; // already handled (the progress event and the result both report it)
    ++silent_recovery_failures_[uid];
    silent_recovery_retry_after_[uid] =
        wall_clock_s_() + kSilentRecoveryRetrySeconds;
    // Let check_encryption_setup_ try again once the backoff is over — or,
    // once the attempts run out, fall back to the dialog.
    if (active_account_ && active_account_->user_id == uid)
        encryption_setup_shown_ = false;
}

bool ShellBase::try_silent_unlock_(const std::shared_ptr<AccountSession>& sess,
                                   std::function<void()> on_failed)
{
    if (!silent_recovery_setup_enabled_ || !sess || !sess->client) return false;
    const std::string uid = sess->user_id;
    if (!silent_unlock_tried_.insert(uid).second) return false;

    using Lookup = tesseract::SecretStore::RecoveryKeyLookup;
    load_stored_recovery_key_(uid, [this, sess, on_failed = std::move(on_failed)](
                                       Lookup found) mutable {
        if (found.status != Lookup::Status::Found || found.key.empty())
        {
            if (on_failed) on_failed();
            return;
        }
        run_silent_recover_(sess, found.key, [this, sess, on_failed](bool ok) {
            if (!ok)
            {
                if (on_failed) on_failed();
                return;
            }
            // A gated first login waits on this; nothing else is left to
            // set up.
            if (pending_sync_session_ == sess)
                release_pending_sync_gate_();
            refresh_encryption_reminder_();
        });
    });
    return true;
}

void ShellBase::show_recover_or_silent_unlock_(const std::shared_ptr<AccountSession>& sess)
{
    auto show_recover = [this, sess] {
        if (active_account_ != sess)
        {
            // Switched away meanwhile: don't leave that account unsynced
            // behind a dialog it can no longer show; its own sync ticks offer
            // unlocking once it's active again.
            if (pending_sync_session_ == sess) release_pending_sync_gate_();
            return;
        }
        show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode::Recover);
        request_relayout_();
    };
    if (!try_silent_unlock_(sess, show_recover))
        show_recover();
}

void ShellBase::run_silent_recover_(std::shared_ptr<AccountSession> sess, std::string key,
                                    std::function<void(bool ok)> done)
{
    run_async_mut_("silent-unlock", [this, sess, key = std::move(key),
                                     done = std::move(done)]() mutable {
        const bool ok = sess && sess->client && sess->client->recover(key).ok;
        post_to_ui_alive_([ok, done = std::move(done)]() { done(ok); });
    });
}

void ShellBase::open_save_key_dialog_(bool before_sign_out)
{
    const std::string uid = my_user_id_;
    auto show = [this, uid, before_sign_out](const std::string& key, bool only_in_memory) {
        if (my_user_id_ != uid) return; // switched accounts meanwhile
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode::SaveKey);
        save_key_dialog_uid_ = uid;
        if (auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr)
        {
            ov->set_recovery_key(key);
            ov->set_before_sign_out(before_sign_out);
            // Not in secure storage: it mustn't be dismissed before it's saved.
            if (only_in_memory) ov->show_key_now();
        }
        request_relayout_();
    };
    if (auto it = unstored_recovery_keys_.find(uid); it != unstored_recovery_keys_.end())
    {
        show(it->second, true);
        return;
    }
    using Lookup = tesseract::SecretStore::RecoveryKeyLookup;
    load_stored_recovery_key_(uid, [this, uid, show, before_sign_out](Lookup found) {
        switch (found.status)
        {
            case Lookup::Status::Found:
                show(found.key, false);
                return;
            case Lookup::Status::Missing:
                // The entry is gone (cleared outside Tesseract): there's no
                // key left to show, so stop asking for it.
                std::fprintf(stderr, "[encryption] stored recovery key missing for %s\n",
                             uid.c_str());
                clear_unsaved_recovery_key_state_(uid);
                refresh_encryption_reminder_();
                break;
            case Lookup::Status::Unreadable:
                // Locked keyring, dismissed unlock prompt, …: the key may well
                // still be there, so keep everything as it is.
                show_status_message_(
                    tk::tr("Couldn't read your recovery key from this computer's "
                           "secure storage."));
                break;
        }
        // Signing out still goes ahead: an unsaved key is kept for the next
        // sign-in (settle_recovery_key_on_sign_out_).
        if (before_sign_out) proceed_sign_out_();
    });
}

void ShellBase::clear_unsaved_recovery_key_state_(const std::string& uid)
{
    unstored_recovery_keys_.erase(uid);
    auto& s = Settings::instance();
    bool changed = s.recovery_key_unsaved.erase(uid) > 0;
    changed |= s.save_key_reminder_dismissals.erase(uid) > 0;
    changed |= s.save_key_reminder_snoozed_until.erase(uid) > 0;
    if (changed) s.save_to_disk(tesseract::config_dir());
}

void ShellBase::mark_recovery_key_saved_(const std::string& uid, bool keep_on_device)
{
    if (uid.empty()) return;
    // The user has their own copy now; Tesseract's goes unless they asked
    // to keep it.
    if (!keep_on_device) forget_stored_recovery_key_(uid);
    clear_unsaved_recovery_key_state_(uid);
    refresh_encryption_reminder_();
}

bool ShellBase::intercept_sign_out_for_unsaved_key_(std::function<void()> proceed)
{
    if (!active_account_) return false;
    const std::string uid = active_account_->user_id;
    if (sign_out_confirmed_uid_ == uid)
    {
        sign_out_confirmed_uid_.clear();
        return false; // proceed_sign_out_ re-entering, or an expired session
    }
    if (!silent_recovery_setup_enabled_ ||
        !Settings::instance().recovery_key_unsaved.count(uid))
        return false;
    pending_sign_out_     = std::move(proceed);
    pending_sign_out_uid_ = uid;
    open_save_key_dialog_(/*before_sign_out=*/true);
    return true;
}

void ShellBase::skip_unsaved_key_check_for_next_sign_out_()
{
    if (active_account_) sign_out_confirmed_uid_ = active_account_->user_id;
}

void ShellBase::proceed_sign_out_(bool remove_key)
{
    auto proceed      = std::move(pending_sign_out_);
    pending_sign_out_ = nullptr;
    const std::string uid = std::move(pending_sign_out_uid_);
    pending_sign_out_uid_.clear();
    if (main_app_) main_app_->show_encryption_setup(false);
    // Only the account the user asked to sign out of.
    if (!proceed || !active_account_ || active_account_->user_id != uid) return;
    sign_out_confirmed_uid_  = uid;
    sign_out_remove_key_uid_ = remove_key ? uid : std::string();
    proceed();
    sign_out_confirmed_uid_.clear(); // in case it never reached the guard
    sign_out_remove_key_uid_.clear();
}

void ShellBase::turn_off_silent_recovery_()
{
    auto sess = active_account_;
    if (!sess) return;
    // Record the choice first: if turning off fails partway (backup deleted,
    // markers not written), silent setup must still not turn it back on.
    auto& s = Settings::instance();
    if (s.silent_recovery_declined.insert(sess->user_id).second)
        s.save_to_disk(tesseract::config_dir());
    run_async_mut_("recovery-disable", [this, sess]() {
        const auto res = sess->client ? sess->client->disable_recovery()
                                      : tesseract::Result{false, "not logged in"};
        post_to_ui_alive_([this, sess, ok = res.ok, msg = std::string(res.message)]() {
            auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr;
            if (!ok)
            {
                // The backup may still be there, so the key and its reminder
                // stay; "Turn off backup" again finishes the job.
                if (ov && sess == active_account_) ov->advance_progress(5, msg, 0, 0);
                request_relayout_();
                return;
            }
            const std::string& uid = sess->user_id;
            forget_stored_recovery_key_(uid);
            clear_unsaved_recovery_key_state_(uid);
            if (ov && sess == active_account_) ov->backup_turned_off();
            refresh_encryption_reminder_();
            request_relayout_();
        });
    });
}

void ShellBase::cancel_sign_out_()
{
    pending_sign_out_ = nullptr;
    pending_sign_out_uid_.clear();
}

void ShellBase::settle_recovery_key_on_sign_out_(const std::string& uid)
{
    silent_recovery_in_flight_.erase(uid);
    silent_recovery_failures_.erase(uid);
    silent_recovery_retry_after_.erase(uid);
    silent_recovery_new_account_.erase(uid);
    silent_unlock_tried_.erase(uid);
    const bool only_in_memory = unstored_recovery_keys_.count(uid) > 0;

    // Signed out without saving it: the secure-storage copy is the only one
    // left. It stays, with its reminder, for the next sign-in (which can
    // also unlock with it silently) — unless the user chose to delete it
    // (a shared computer).
    const bool delete_anyway = sign_out_remove_key_uid_ == uid;
    if (Settings::instance().recovery_key_unsaved.count(uid) > 0 && !only_in_memory &&
        !delete_anyway)
        return;

    forget_stored_recovery_key_(uid);
    clear_unsaved_recovery_key_state_(uid);
}

void ShellBase::load_stored_recovery_key_(
    const std::string& uid,
    std::function<void(tesseract::SecretStore::RecoveryKeyLookup)> done)
{
    // The OS keyring can block (D-Bus, an unlock prompt): never on the UI thread.
    run_async_mut_("recovery-key-load", [this, uid, done = std::move(done)]() mutable {
        auto found = tesseract::SecretStore::load_recovery_key(uid);
        post_to_ui_alive_([found = std::move(found), done = std::move(done)]() mutable {
            done(std::move(found));
        });
    });
}

void ShellBase::store_recovery_key_(const std::string& uid, const std::string& key,
                                    std::function<void(bool)> done)
{
    run_async_mut_("recovery-key-store", [this, uid, key, done = std::move(done)]() mutable {
        const bool ok = tesseract::SecretStore::save_recovery_key(uid, key);
        post_to_ui_alive_([ok, done = std::move(done)]() { done(ok); });
    });
}

void ShellBase::forget_stored_recovery_key_(const std::string& uid)
{
    run_async_mut_("recovery-key-forget",
                   [uid]() { tesseract::SecretStore::remove_recovery_key(uid); });
}

// ── Encryption flow ───────────────────────────────────────────────────────────

void ShellBase::show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode mode)
{
    if (!main_app_) return;
    auto* ov = main_app_->encryption_setup();
    if (!ov || !main_app_->host()) return;

    // Reconfigure the overlay (clears prior callbacks + field text) before
    // wiring the shared callbacks.
    ov->reset(mode);
    wire_encryption_setup_callbacks_(*ov, *main_app_->host());
    if (mode == views::EncryptionSetupOverlay::Mode::Recover)
        refresh_other_device_availability_();

    main_app_->show_encryption_setup(true);
    request_relayout_();
}

std::int64_t ShellBase::wall_clock_s_() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool ShellBase::foreign_identity_cached_()
{
    if (!foreign_identity_known_true_)
        foreign_identity_known_true_ = foreign_cross_signing_identity_();
    return foreign_identity_known_true_;
}

void ShellBase::handle_recovery_state_changed_ui_()
{
    foreign_identity_known_true_ = false;
    refresh_encryption_reminder_();
}

void ShellBase::refresh_encryption_reminder_(std::optional<bool> device_verified)
{
    if (!main_app_) return;
    auto* banner = main_app_->encryption_reminder();
    if (!banner) return;

    using Reminder = EncryptionFlowController::Reminder;
    Reminder kind = Reminder::None;
    if (active_account_ && client_)
    {
        // Runs on every sync tick: only pay for the identity lookups when
        // reminder_for() would look at the answer.
        const bool    verified = device_verified.value_or(read_device_verified_());
        const uint8_t state    = read_recovery_state_();
        const bool    foreign  = (!verified || state == 1) && foreign_identity_cached_();
        const bool    unsaved  = silent_recovery_setup_enabled_ &&
            Settings::instance().recovery_key_unsaved.count(my_user_id_) > 0;
        kind = EncryptionFlowController::reminder_for(state, verified, foreign, unsaved);
        // Recovery is being set up silently: "set it up" would start a
        // second, competing setup.
        if (silent_recovery_pending_(my_user_id_)) kind = Reminder::None;
        // Tesseract sets recovery up by itself; asking the user to do it only
        // makes sense once that has given up.
        if (kind == Reminder::SetupNeeded && silent_recovery_setup_enabled_ &&
            !silent_recovery_exhausted_(my_user_id_) &&
            !Settings::instance().silent_recovery_declined.count(my_user_id_))
            kind = Reminder::None;
    }

    bool show = kind != Reminder::None;
    if (show)
    {
        // "Save your recovery key" has its own snooze, so dismissing it never
        // hides a more urgent reminder.
        const auto& s       = Settings::instance();
        const auto& snoozes = kind == Reminder::SaveKey ? s.save_key_reminder_snoozed_until
                                                        : s.encryption_reminder_snoozed_until;
        auto it = snoozes.find(my_user_id_);
        if (it != snoozes.end() &&
            EncryptionFlowController::snoozed(it->second, wall_clock_s_()))
            show = false;
    }
    using BannerKind    = views::EncryptionReminderBanner::Kind;
    const auto new_kind = kind == Reminder::SetupNeeded ? BannerKind::SetupNeeded
                        : kind == Reminder::SaveKey     ? BannerKind::SaveKey
                                                        : BannerKind::Locked;
    if (show == main_app_->encryption_reminder_requested() &&
        (!show || banner->kind() == new_kind))
        return; // unchanged — the common case on a sync tick
    if (show) banner->set_kind(new_kind);
    main_app_->show_encryption_reminder(show);
    request_relayout_();
}

void ShellBase::snooze_encryption_reminder_()
{
    if (my_user_id_.empty()) return;
    if (main_app_ && main_app_->encryption_reminder() &&
        main_app_->encryption_reminder()->kind() ==
            views::EncryptionReminderBanner::Kind::SaveKey)
    {
        snooze_save_key_reminder_(my_user_id_);
        return;
    }
    auto& s = Settings::instance();
    s.encryption_reminder_snoozed_until[my_user_id_] =
        wall_clock_s_() + EncryptionFlowController::kSnoozeSeconds;
    s.save_to_disk(tesseract::config_dir());
    refresh_encryption_reminder_();
}

void ShellBase::snooze_save_key_reminder_(const std::string& uid)
{
    if (uid.empty()) return;
    // Comes back sooner at first, then less often, but never stops while the
    // key is unsaved.
    auto& s     = Settings::instance();
    const int n = ++s.save_key_reminder_dismissals[uid];
    s.save_key_reminder_snoozed_until[uid] =
        wall_clock_s_() + EncryptionFlowController::save_key_snooze_seconds(n);
    s.save_to_disk(tesseract::config_dir());
    refresh_encryption_reminder_();
}

void ShellBase::refresh_other_device_availability_()
{
    auto sess = active_account_;
    run_async_mut_([this, sess]() {
        if (!sess || !sess->client) return;
        // Not list_devices()'s Verified flag: for our own devices that also
        // requires *this* device to be trusted, so it's always false exactly
        // where this question is asked.
        const bool has = sess->client->has_devices_to_verify_against();
        post_to_ui_alive_([this, sess, has]() {
            if (sess != active_account_) return;
            if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
                o->set_has_verified_other_device(has);
            request_relayout_();
        });
    });
}

void ShellBase::start_self_verification_()
{
    encryption_flow_.set_outgoing_target({});
    start_outgoing_verification_();
}

void ShellBase::start_user_verification_(const std::string& user_id,
                                         const std::string& name)
{
    if (user_id.empty() || !main_app_) return;
    auto* ov = main_app_->encryption_setup();
    if (!ov) return;
    if (encryption_flow_.has_flow() || (ov->visible() && ov->busy()))
    {
        show_status_message_(tk::tr("Finish the current verification first."));
        return;
    }
    if (!ov->visible())
        show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode::Verify);
    ov->show_outgoing_user_request(name.empty() ? user_id : name);
    request_relayout_();
    encryption_flow_.set_outgoing_target(user_id);
    start_outgoing_verification_();
}

void ShellBase::start_outgoing_verification_()
{
    const std::string target = encryption_flow_.outgoing_target();
    encryption_flow_.clear();
    encryption_flow_.set_awaiting_outgoing(true);
    auto sess = active_account_;
    run_async_mut_([this, sess, target]() {
        if (!sess || !sess->client) return;
        auto r = target.empty() ? sess->client->request_self_verification()
                                : sess->client->request_user_verification(target);
        post_to_ui_alive_([this, sess, target, ok = r.ok, msg = std::string(r.message)]() {
            if (sess != active_account_)
            {
                // Switched away while it was being sent: don't leave it
                // pending on the other devices.
                if (ok)
                    run_async_mut_([sess, fid = msg]() {
                        if (sess && sess->client) sess->client->cancel_verification(fid);
                    });
                return;
            }
            if (!ok)
            {
                encryption_flow_.set_awaiting_outgoing(false);
                if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
                    o->verification_failed(msg, true);
                request_relayout_();
                return;
            }
            if (!encryption_flow_.awaiting_outgoing())
            {
                // The user cancelled while the request was being sent.
                run_async_mut_([sess, fid = msg]() {
                    if (sess && sess->client) sess->client->cancel_verification(fid);
                });
                return;
            }
            // Track the request from now on, not only once a device accepts:
            // a decline or timeout arrives as a cancel for this id before any
            // Ready does.
            encryption_flow_.begin({.id = msg,
                                    .user_id = target.empty() ? my_user_id_ : target,
                                    .incoming = false, .own_user = target.empty(),
                                    .this_device_unverified = !read_device_verified_()});
        });
    });
}

void ShellBase::cancel_active_verification_()
{
    const std::string fid = encryption_flow_.flow().id;
    encryption_flow_.clear(); // also drops a still-pending outgoing request
    if (fid.empty()) return;
    auto sess = active_account_;
    run_async_mut_([sess, fid]() {
        if (sess && sess->client) sess->client->cancel_verification(fid);
    });
}

void ShellBase::handle_verification_request_ui_(std::string account_uid, std::string flow_id,
                                                std::string user_id, std::string device_id,
                                                bool incoming)
{
    auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr;

    // A background account of this window received it: answer it on that
    // account, not in the active one's dialog (whose client doesn't know
    // the flow).
    if (!active_account_ || account_uid != active_account_->user_id)
    {
        auto target = account_manager_.find(account_uid);
        // Only requests *to* us matter; an accepted outgoing one can't
        // belong to a background account's (never shown) dialog.
        if (!incoming || !target || !target->client)
            return;
        // Switching accounts and raising the window is only for the user's
        // own other device. Anyone sharing a room could otherwise do it at
        // will; leave their request pending (it times out, or is answered
        // from another client) rather than cancel it on the user's behalf.
        if (user_id != account_uid)
        {
            std::fprintf(stderr,
                         "[verify] request from %s to background account %s left pending\n",
                         user_id.c_str(), account_uid.c_str());
            return;
        }
        if (ov && ov->visible() && ov->busy())
        {
            // Switching now would throw away e.g. a just-created recovery key
            // on screen; turn the request away on its own account instead.
            run_async_mut_([target, flow_id]() {
                if (target->client) target->client->cancel_verification(flow_id);
            });
            return;
        }
        switch_active_account_(account_uid);
        if (!active_account_ || active_account_->user_id != account_uid)
            return; // switch refused
        raise_and_activate_(); // the user just started this on their other device
        ov = main_app_ ? main_app_->encryption_setup() : nullptr;
    }

    auto sess = active_account_;

    if (!incoming)
    {
        // Our outgoing request was accepted. Only adopt it if the dialog is
        // still waiting for it (tracked since it was sent, or — if Ready beat
        // the send result here — still awaited); anything else is cancelled.
        const bool ours = (encryption_flow_.is_flow(flow_id) &&
                           !encryption_flow_.flow().incoming) ||
                          (!encryption_flow_.has_flow() && encryption_flow_.awaiting_outgoing());
        if (!ours || !ov || !ov->visible())
        {
            run_async_mut_([sess, flow_id]() {
                if (sess && sess->client) sess->client->cancel_verification(flow_id);
            });
            return;
        }
        encryption_flow_.begin({.id = flow_id, .user_id = user_id,
                                .device_id = device_id, .incoming = false,
                                .own_user = user_id == my_user_id_,
                                .this_device_unverified = !read_device_verified_()});
        run_async_mut_([sess, flow_id]() {
            if (sess && sess->client) sess->client->start_sas(flow_id);
        });
        ov->show_waiting();
        request_relayout_();
        return;
    }

    if (!ov) return;
    using Action = EncryptionFlowController::IncomingAction;
    if (EncryptionFlowController::on_incoming(ov->visible(), ov->busy()) ==
            Action::RefuseBusy ||
        encryption_flow_.has_flow())
    {
        // Mid-setup (the new recovery key may be on screen) or already
        // verifying: turn the newcomer away rather than yank the dialog.
        run_async_mut_([sess, flow_id]() {
            if (sess && sess->client) sess->client->cancel_verification(flow_id);
        });
        return;
    }

    const bool own = user_id == my_user_id_;
    encryption_flow_.begin({.id = flow_id, .user_id = user_id, .device_id = device_id,
                            .incoming = true, .own_user = own,
                            .this_device_unverified = !read_device_verified_()});
    if (!ov->visible())
        show_encryption_setup_overlay_(views::EncryptionSetupOverlay::Mode::Verify);
    ov->show_incoming_request(own ? device_id : user_id, own);
    request_relayout_();

    // Name the device the way the user named it, once known.
    if (own)
    {
        run_async_mut_([this, sess, flow_id, device_id]() {
            if (!sess || !sess->client) return;
            std::string name;
            for (const auto& d : sess->client->list_devices())
                if (d.id == device_id && !d.display_name.empty()) name = d.display_name;
            if (name.empty()) return;
            post_to_ui_alive_([this, flow_id, name]() {
                if (!encryption_flow_.is_flow(flow_id)) return;
                if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr)
                    o->set_peer(name);
                request_relayout_();
            });
        });
    }
}

void ShellBase::handle_sas_ready_ui_(std::string flow_id, VerificationSas sas)
{
    if (!encryption_flow_.is_flow(flow_id)) return;
    auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr;
    if (!ov || !ov->visible()) return;
    ov->show_sas(std::move(sas));
    request_relayout_();
}

void ShellBase::handle_verification_done_ui_(std::string flow_id)
{
    if (!encryption_flow_.is_flow(flow_id)) return;
    const auto flow = encryption_flow_.flow();
    encryption_flow_.clear();

    using DoneKind = views::EncryptionSetupOverlay::DoneKind;
    const DoneKind kind = !flow.own_user                ? DoneKind::UserVerified
                        : flow.this_device_unverified   ? DoneKind::Unlocked
                                                        : DoneKind::OtherDeviceConfirmed;
    if (kind == DoneKind::Unlocked)
        encryption_setup_dismissed_ = true; // nothing left to set up here

    if (auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr; ov && ov->visible())
        ov->verification_done(kind);
    // A just-verified user's open profile should say so. The flow ran on the
    // active account, which is who untagged dispatches address.
    if (!flow.own_user)
        handle_user_identities_changed_ui_({flow.user_id});
    refresh_encryption_reminder_();
    request_relayout_();
}

void ShellBase::handle_verification_cancelled_ui_(std::string flow_id, std::string reason)
{
    if (!encryption_flow_.is_flow(flow_id)) return;
    const bool we_started = !encryption_flow_.flow().incoming;
    encryption_flow_.clear();
    auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr;
    if (!ov || !ov->visible() || !ov->in_verification_step()) return;
    ov->verification_failed(std::move(reason), we_started);
    request_relayout_();
}

void ShellBase::handle_verification_state_ui_(bool is_verified)
{
    if (last_device_verified_ != is_verified)
    {
        last_device_verified_        = is_verified;
        foreign_identity_known_true_ = false;
    }
    if (main_app_ && main_app_->user_info())
        main_app_->user_info()->set_warning_dot(!is_verified);
    refresh_encryption_reminder_(is_verified);
}

// Callback for build_user_menu_items_'s verify_session parameter above:
// null when the active account is already verified (the item is then
// omitted), otherwise reopens the same encryption-setup dialog shown
// right after login (recovery-key entry, or Fresh bootstrap — whichever
// check_encryption_setup_ picks; it doesn't require another device).
// Every shell wants the exact same condition and action here, so it's
// consolidated instead of duplicated four times.
std::function<void()> ShellBase::verify_session_menu_callback_()
{
    // Deliberately reads read_device_verified_() live instead of
    // active_account_->unverified: that cached field is only ever written by
    // EventHandlerBase::on_verification_state_changed, driven by a watcher
    // task spawned inside the full start_sync() — it can lag well behind (or
    // never catch up to) the actual state, exactly like the warning-dot badge
    // would if it read the same field instead of calling read_device_verified_()
    // directly (see check_encryption_setup_()'s handle_verification_state_ui_
    // call). Matching that same live read keeps the menu item's visibility
    // consistent with the badge.
    if (!active_account_ || read_device_verified_())
        return nullptr;
    return [this] { reopen_encryption_setup_(); };
}

void ShellBase::begin_crypto_identity_reset_()
{
    if (!client_ || !main_app_)
        return;

    // User-initiated, so bypass the recovery_state gating in
    // check_encryption_setup_ and drive the overlay directly. Reuse the
    // per-shell overlay setup (Fresh mode wires the post-approval recovery-key
    // flow + native fields), then put it into the reset-approval wait.
    encryption_setup_dismissed_ = false;
    encryption_setup_shown_     = true;
    show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode::Fresh);

    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // show_encryption_setup_overlay_ → reset() cleared callbacks, so set the
    // cancel hook now (after wiring).
    ov->on_cancel_reset = [this]() {
        auto sess = active_account_;
        run_async_mut_([sess]() {
            if (!sess || !sess->client) return;
            sess->client->cancel_reset_crypto_identity();
        });
        // Same exit as any other close: in particular it releases a gated
        // first sync — the reset is reachable from the gated login dialog
        // (Recover › I've lost… › Reset encryption).
        auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
        if (o && o->on_close)
            o->on_close();
    };
    ov->set_reset_account(my_user_id_);
    ov->begin_reset_wait();
    request_relayout_();

    auto sess = active_account_;
    run_async_mut_([this, sess]() {
        if (!sess || !sess->client) return;
        auto r = sess->client->begin_reset_crypto_identity();
        post_to_ui_alive_([this, ok = r.ok, needs = r.needs_approval,
                     msg = std::string(r.message),
                     url = std::string(r.approval_url)]() {
            auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
            if (!o)
                return;
            if (!ok)
            {
                o->report_reset_error(msg);
            }
            else if (!needs)
            {
                // Reset completed with no browser approval needed — go straight
                // to recovery-key setup.
                o->reset_approved();
            }
            else
            {
                // Wait for the user to approve in the browser; the SDK polls
                // and fires on_crypto_reset_result when it resolves.
                o->set_reset_approval_url(url);
                tesseract::Client::open_in_browser(url);
            }
            request_relayout_();
        });
    });
}

void ShellBase::handle_crypto_reset_result_ui_(bool ok, std::string message)
{
    auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
    if (!o)
        return;
    if (ok)
        o->reset_approved(); // → Fresh recovery-key setup (Intro)
    else
        o->report_reset_error(message);
    request_relayout_();
}
} // namespace tesseract
