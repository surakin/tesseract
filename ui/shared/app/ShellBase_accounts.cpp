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

// Arm the pending-login OAuth flow's temp directory. Installed (via a
// shell-native one-liner lambda) as the LoginView's on-begin-oauth
// callback: the user_id isn't known until await_oauth completes, so the
// OAuth round-trip runs against a per-attempt "pending-<ms>" directory
// that finalize-login later renames to accounts/<sanitized-uid>/.
//
// Idempotent: returns immediately if pending_login_temp_dir_ is already
// set. Computes a unique "pending-<ms>" dir under SessionStore::account_dir,
// creates it, and points pending_login_client_'s data dir at its
// "matrix-store" subdir. Operates on the ShellBase pending_login_* members.
void ShellBase::arm_pending_login_()
{
    if (!pending_login_temp_dir_.empty())
    {
        return;
    }
    // The timestamp only needs to make the temp dir name unique for this login
    // attempt; nothing parses it back, so a portable std::chrono source serves
    // every platform (no need for QDateTime / per-shell clocks).
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    pending_login_temp_dir_ =
        tesseract::SessionStore::account_dir("pending-" + std::to_string(ms));
    std::error_code ec;
    std::filesystem::create_directories(pending_login_temp_dir_, ec);
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());
}

// Blocking half of restore: legacy-layout migration, index load, and per-
// account Client construction / restore_session / identity+prefs fetch,
// plus make_account_bridge_ + start_sync (both confirmed background-safe
// — see RestoredAccountIO). Calls the virtual make_account_bridge_ hook
// (so it can't be static), but otherwise touches only SessionStore
// statics and locally-owned objects — no mutable ShellBase state — so
// it's safe to call from any thread, including mut_pool_'s worker
// thread. This is deliberately where the expensive per-account work
// lives: restore_session and start_sync both block on real Rust-side
// I/O/setup, so keeping them off the UI thread is the whole point of the
// async entry point below.
//
// `network_available` is a pre-computed, UI-thread result of
// tk::Host::is_network_available() (Host isn't reachable from this
// worker-thread-safe method itself — see restore_all_accounts_async_'s
// doc comment). When false, every stored account is short-circuited
// straight to a failed/network_unavailable result without attempting
// the always-network-bound Client::restore_session() call (see
// sdk/src/oauth.rs's build_configured_client(), which performs
// well-known discovery unconditionally). Defaults to true so existing
// callers (tests, restore_all_accounts_()) keep today's always-attempt
// behavior.
ShellBase::RestoreIOResult ShellBase::restore_all_accounts_blocking_(bool network_available)
{
    RestoreIOResult io;

    // One-shot migration from the legacy single-account layout. Runs once per
    // install before any Client is constructed; idempotent on every subsequent
    // launch.
    tesseract::SessionStore::migrate_legacy_layout();

    // Delete any account folder left behind by allocate_account_dir()
    // picking a fresh name instead of colliding with a still-locked
    // leftover from a previous session (see its doc comment). This process
    // has no live handle on anything from a previous run, so an orphaned
    // folder is now safe to delete outright. Same "before any Client
    // exists" timing as the migration above.
    tesseract::SessionStore::sweep_orphaned_account_dirs();

    // Restore every account on disk, in index order, so notifications fire for
    // any of them while the user works in the foreground one.
    auto index = tesseract::SessionStore::load_index();
    io.active_user_id_hint = index.active_user_id;

    for (const auto& uid : index.user_ids)
    {
        if (!network_available)
        {
            io.any_restore_failed  = true;
            io.network_unavailable = true;
            continue;
        }

        auto loaded = tesseract::SessionStore::load_account_with_key(uid);
        if (!loaded)
        {
            continue;
        }

        RestoredAccountIO acc;
        acc.client = std::make_unique<tesseract::Client>();
        acc.client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());
        // Empty store_key is a permanent, valid state for any account that
        // predates store encryption — the store just opens unencrypted, same
        // as before this existed. See SessionStore::LoadedAccount.
        if (!loaded->store_key.empty())
        {
            acc.client->set_store_key(loaded->store_key);
        }

        auto res = acc.client->restore_session(loaded->session_json);
        if (!res)
        {
            io.restore_error      = res.message;
            io.any_restore_failed = true;
            continue;
        }

        acc.user_id      = acc.client->get_user_id();
        acc.display_name = acc.client->get_display_name();
        acc.avatar_url   = acc.client->get_avatar_url();
        {
            acc.prefs_json = acc.client->load_prefs_json();
            auto prefs = tesseract::Prefs::parse(acc.prefs_json);
            acc.last_room  = prefs.last_room;
            acc.open_rooms = prefs.open_rooms;
            acc.recent_rooms = prefs.recent_rooms;
            acc.bridge_not_bridged_overrides = prefs.bridge_not_bridged_overrides;
            acc.emoji_skin_tone = tesseract::emoji::skin_tone_from_key(prefs.emoji_skin_tone);
        }

        // Bridge construction + start_sync are the expensive part of restore
        // (start_sync blocks on real Rust-side sliding-sync/room-list setup)
        // and are both confirmed background-safe, so they happen here rather
        // than in finish_restore_accounts_ui_() on the UI thread.
        acc.bridge = make_account_bridge_(acc.user_id);
        acc.client->start_sync(acc.bridge.get());

        // Also here, not in finish_restore_accounts_ui_(): measured to block
        // the UI thread for multiple seconds — Client::set_msc2545_legacy_
        // compat synchronously rebuilds the image-pack cache (a per-room
        // network fetch), it is not the cheap flag-flip its old call site
        // assumed.
        apply_search_indexing_pref_(*acc.client);
        apply_membership_events_pref_(*acc.client);
        apply_bundled_url_previews_pref_(*acc.client);
        apply_msc2545_legacy_compat_pref_(*acc.client);
        apply_low_power_pref_(*acc.client);

        io.accounts.push_back(std::move(acc));
    }

    return io;
}

// UI-thread finish half: consumes a RestoreIOResult and does the
// remaining, genuinely UI-thread-affine steps — the pref-apply calls,
// install_account_notifier_ / install_account_up_connector_, and
// account_manager_.add_account. (Bridge construction and start_sync
// already happened in restore_all_accounts_blocking_(), off the UI
// thread.) Mutates account_manager_ and other shell state; UI-thread
// only.
ShellBase::RestoreResult
ShellBase::finish_restore_accounts_ui_(RestoreIOResult&& io)
{
    RestoreResult result;
    result.any_restore_failed  = io.any_restore_failed;
    result.restore_error       = io.restore_error;
    result.network_unavailable = io.network_unavailable;

    for (auto& acc : io.accounts)
    {
        auto session         = std::make_unique<tesseract::AccountSession>();
        session->client      = std::move(acc.client);
        session->user_id     = acc.user_id;
        session->display_name = acc.display_name;
        session->avatar_url  = acc.avatar_url;
        session->last_room   = acc.last_room;
        session->open_rooms  = std::move(acc.open_rooms);
        session->recent_rooms = std::move(acc.recent_rooms);
        session->bridge_not_bridged_overrides = std::move(acc.bridge_not_bridged_overrides);
        session->emoji_skin_tone = acc.emoji_skin_tone;
        session->prefs_json = std::move(acc.prefs_json);

        // Bridge already built + sync already started on the worker thread
        // in restore_all_accounts_blocking_(), which also already applied the
        // search-indexing / membership-events / msc2545-legacy-compat prefs
        // (the last of those blocks on a real network fetch — see its header
        // doc comment — so it must not run here on the UI thread).
        session->bridge = std::move(acc.bridge);
        session->sync_started = true;

        // Per-account notifier (native) and Linux-only UnifiedPush connector.
        install_account_notifier_(*session);
        install_account_up_connector_(*session);

        if (session->user_id == io.active_user_id_hint)
        {
            result.active_uid = session->user_id;
        }
        account_manager_.add_account(std::move(session));
    }

    result.any_accounts = !account_manager_.accounts().empty();
    if (result.active_uid.empty() && result.any_accounts)
    {
        result.active_uid = account_manager_.accounts().front()->user_id;
    }
    return result;
}

// Platform-agnostic startup restore loop, shared by every shell's primary-
// window startup entry (doLogin / do_login / start_login / beginLogin) AFTER
// the is_secondary_window_startup_ gate. Runs the legacy-layout migration,
// loads the account index, and for each stored uid: restores the session
// (skipping + recording failures), caches display name / avatar / prefs,
// builds the per-account event bridge (make_account_bridge_) and starts
// sync, then installs the native per-account notifier
// (install_account_notifier_) and the Linux-only UnifiedPush connector
// (install_account_up_connector_), and adds the account to the manager.
// Returns a RestoreResult; the caller does the native empty-fallback /
// finish-login decision. UI-thread only. Implemented as a composition of
// restore_all_accounts_blocking_() + finish_restore_accounts_ui_() — kept
// as a synchronous single-call entry point for callers (e.g. tests) that
// don't need the async form below.
ShellBase::RestoreResult ShellBase::restore_all_accounts_()
{
    return finish_restore_accounts_ui_(restore_all_accounts_blocking_());
}

void ShellBase::restore_all_accounts_async_(
    std::function<void(RestoreResult)> done, bool network_available)
{
    on_startup_restore_progress_ui_(tk::tr("Restoring session\xe2\x80\xa6"));
    run_async_mut_(
        [this, done = std::move(done), network_available]() mutable
        {
            // io holds a move-only std::unique_ptr<Client> per account, so it
            // can't be captured by value into a std::function (which requires
            // its target to be copy-constructible even though it's only ever
            // invoked once here) — shared_ptr sidesteps that.
            auto io = std::make_shared<RestoreIOResult>(
                restore_all_accounts_blocking_(network_available)); // worker thread
            post_to_ui_alive_(
                [this, io, done = std::move(done)]() mutable
                {
                    auto result = finish_restore_accounts_ui_(std::move(*io));
                    on_startup_restore_progress_ui_(""); // clear
                    done(std::move(result));
                });
        });
}

ShellBase::FinalizeLoginIO ShellBase::finalize_login_blocking_(
    std::unique_ptr<tesseract::Client> pending_client,
    std::filesystem::path pending_temp_dir)
{
    FinalizeLoginIO io;
    FinalizeLoginResult& out = io.result;

    // OAuth completed on pending_client, which the LoginView drove against
    // the temp data dir (pending_temp_dir/matrix-store). The caller already
    // validated get_user_id() is non-empty before handing the client off.
    const std::string user_id = pending_client->get_user_id();
    out.user_id                = user_id;

    // Snapshot the session blob before dropping the in-flight Client — re-opening
    // it below restores from this JSON.
    const std::string session_json = pending_client->export_session();
    if (session_json.empty())
    {
        out.error = tk::tr("empty session");
        return io;
    }

    // A brand-new login always generated a fresh store-encryption key (see
    // oauth_begin / login_password); snapshot it too so the fresh Client
    // opened below can reopen the same encrypted store, and so it can be
    // persisted alongside the session.
    const std::vector<uint8_t> store_key = pending_client->store_key();

    // Drop the in-flight Client so its SQLite handles are released before we
    // rename the directory underneath it. (The shell has already cleared any raw
    // login-view alias to this client.)
    pending_client.reset();

    // Move the temp account directory into its final per-user-id home.
    // allocate_account_dir() (not plain account_dir()) picks a fresh,
    // guaranteed-not-already-existing folder — falling back to a
    // "-2"/"-3"/... suffix if the default name is still occupied by a
    // leftover from this same account's previous session (see its doc
    // comment for why that can happen and why colliding with it isn't
    // safe) — so the rename below always lands on a clean destination. The
    // rename is atomic on the same filesystem; on a cross-filesystem move
    // it fails with EXDEV, so fall back to a recursive copy + remove for
    // that unrelated case.
    const std::filesystem::path final_dir =
        tesseract::SessionStore::allocate_account_dir(user_id);
    {
        std::error_code ec;
        std::filesystem::create_directories(final_dir.parent_path(), ec);
        std::filesystem::rename(pending_temp_dir, final_dir, ec);
        if (ec)
        {
            std::error_code ec2;
            std::filesystem::copy(
                pending_temp_dir, final_dir,
                std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing,
                ec2);
            if (ec2)
            {
                out.error = tk::trf(tk::tr("couldn't persist matrix store: {0}"), {ec2.message()});
                return io;
            }
            std::filesystem::remove_all(pending_temp_dir, ec2);
        }
    }

    // Persist the session blob (and the store key, if any) into the final
    // per-account dir.
    if (!tesseract::SessionStore::save_account_with_key(user_id, session_json,
                                                        store_key))
    {
        out.error = tk::tr("couldn't persist session");
        return io;
    }

    // Open a fresh Client against the final store path and restore from the
    // just-exported session JSON (matrix-sdk reuses the moved SQLite store
    // transparently — no resync). Must set the same store_key before
    // restoring, or (when one was generated) the store fails to open.
    auto session    = std::make_unique<tesseract::AccountSession>();
    session->user_id = user_id;
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    if (!store_key.empty())
    {
        session->client->set_store_key(store_key);
    }
    auto res = session->client->restore_session(session_json);
    if (!res)
    {
        out.error = "restore: " + res.message;
        tesseract::SessionStore::clear_account(user_id);
        return io;
    }
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    {
        session->prefs_json = session->client->load_prefs_json();
        auto prefs = tesseract::Prefs::parse(session->prefs_json);
        session->last_room  = prefs.last_room;
        session->open_rooms = prefs.open_rooms;
        session->recent_rooms = prefs.recent_rooms;
        session->bridge_not_bridged_overrides = prefs.bridge_not_bridged_overrides;
        session->emoji_skin_tone = tesseract::emoji::skin_tone_from_key(prefs.emoji_skin_tone);
    }

    // Per-account event bridge (native type) + background sync. Both are
    // confirmed background-safe (see RestoredAccountIO) and are the
    // expensive part of finalize — this is why finalize_login_blocking_
    // exists as a separate, worker-thread-only step.
    session->bridge = make_account_bridge_(session->user_id);

    // Determine whether recovery/cross-signing needs setting up on this
    // device BEFORE starting sync, so the encryption-setup flow (and any
    // SAS verification the user starts from it) doesn't have to compete
    // with the initial sync for the crypto machine — the race that made
    // verification slow/flaky right after login. recovery_state() is a
    // cheap local read, but can briefly report Unknown while the crypto
    // machine finishes initializing, so poll it for a short bound; if it's
    // still Unknown after that, don't block startup on it — start_sync
    // proceeds normally and check_encryption_setup_() will catch it later,
    // same as the pre-existing (unlocked) behavior.
    uint8_t recovery_state = session->client->recovery_state();
    {
        constexpr auto kPollInterval = std::chrono::milliseconds(50);
        constexpr auto kPollTimeout  = std::chrono::milliseconds(2000);
        const auto      deadline     = std::chrono::steady_clock::now() + kPollTimeout;
        while (recovery_state == 0 /* Unknown */ &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(kPollInterval);
            recovery_state = session->client->recovery_state();
        }
    }

    if (recovery_state == 1 /* Disabled */ || recovery_state == 3 /* Incomplete */)
    {
        // Disabled is ambiguous between a truly fresh account (bootstrap a
        // new identity, Fresh) and one whose identity was set up on another
        // device without server-side secret storage (must Recover instead)
        // — same distinction check_encryption_setup_() makes; see
        // foreign_cross_signing_identity_()'s doc comment.
        const bool foreign_identity =
            session->client->own_identity_exists() &&
            !session->client->have_cross_signing_keys();
        out.needs_encryption_setup        = true;
        out.encryption_setup_recover_mode = recovery_state == 3 || foreign_identity;
        session->sync_started              = false;
        // Attach the handler (without starting the sync loop) so the
        // encryption-setup overlay's enable_recovery/recover progress
        // callbacks — routed through this same handler — actually reach the
        // UI instead of silently vanishing. release_pending_sync_gate_()
        // later calls the real start_sync(), which re-attaches (harmlessly)
        // and spawns the sync tasks this deliberately skips for now.
        session->client->attach_event_handler(session->bridge.get());
        // Keep the crypto side alive meanwhile: uploads this device's keys and
        // carries verification traffic both ways, so "Use another device"
        // and requests from the user's other devices work from the dialog
        // without the room-list sync that slows recovery operations down.
        session->client->start_encryption_sync();
    }
    else
    {
        session->client->start_sync(session->bridge.get());
        session->sync_started = true;
    }
    apply_search_indexing_pref_(*session->client);
    apply_membership_events_pref_(*session->client);
    apply_bundled_url_previews_pref_(*session->client);
    apply_msc2545_legacy_compat_pref_(*session->client);
    apply_low_power_pref_(*session->client);

    out.ok    = true;
    io.session = std::move(session);
    return io;
}

// Async, platform-agnostic core of each shell's on_login_succeeded, run
// after OAuth completes for a NEWLY added account on
// pending_login_client_. On the UI thread: fetches the user_id and
// rejects (rejected_duplicate) if account_manager_.find(uid) — resolving
// `done` synchronously in both the empty-client and duplicate cases, no
// worker hop needed. Otherwise moves pending_login_client_ /
// pending_login_temp_dir_ out and dispatches finalize_login_blocking_()
// onto mut_pool_; the post_to_ui_alive_-guarded continuation installs the
// native notifier (install_account_notifier_) and Linux-only UnifiedPush
// connector (install_account_up_connector_), adds the account, updates
// the on-disk index (active = the new uid), and invokes `done`. Does NOT
// touch native widgets (login-view dismiss, surface switch, status bar)
// — the shell does the native finish using the result passed to `done`.
// The shell must call set_client(nullptr) on its login view BEFORE
// calling this when it owns a raw alias to pending_login_client_ (it is
// moved out here). UI-thread only to call; `done` itself runs on the UI
// thread.
void ShellBase::finalize_login_async_(std::function<void(FinalizeLoginResult)> done)
{
    if (!pending_login_client_)
    {
        done(FinalizeLoginResult{}); // defensive — !ok, nothing to report
        return;
    }

    // OAuth completed on pending_login_client_, which the LoginView drove against
    // the temp data dir (pending_login_temp_dir_/matrix-store). We don't yet know
    // the user_id; ask the client now (a cheap local accessor, unlike start_sync
    // — safe to call on the UI thread).
    const std::string user_id = pending_login_client_->get_user_id();
    if (user_id.empty())
    {
        FinalizeLoginResult out;
        out.error = tk::tr("no user id");
        done(std::move(out));
        return;
    }

    // If an account with this user_id is already signed in (the user added an
    // account they're already logged into), refuse rather than colliding on disk.
    if (account_manager_.find(user_id))
    {
        FinalizeLoginResult out;
        out.rejected_duplicate = true;
        out.user_id            = user_id;
        pending_login_client_.reset();
        std::error_code ec;
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
        pending_login_temp_dir_.clear();
        done(std::move(out));
        return;
    }

    // Second line of defense, independent of how logout_active_account_impl_'s
    // own wait behaved: if this user_id's previous session is still draining
    // (its mut_pool_ teardown barrier hasn't fired yet), wait here too. A
    // no-op in the common case; it only actually blocks when a logout+re-login
    // of the same account races faster than the drain finished.
    if (account_manager_.is_draining(user_id))
    {
        account_manager_.wait_until_drained(user_id, kAccountDrainTimeout);
    }

    // pending_client holds a move-only std::unique_ptr<Client>, so it can't
    // be captured by value into a std::function (which requires its target
    // to be copy-constructible even though it's only ever invoked once here)
    // — a shared_ptr wrapper sidesteps that, mirroring the io-result trick in
    // restore_all_accounts_async_ above.
    auto pending_client = std::make_shared<std::unique_ptr<tesseract::Client>>(
        std::move(pending_login_client_));
    auto pending_temp_dir = pending_login_temp_dir_;
    pending_login_temp_dir_.clear();

    run_async_mut_(
        [this, pending_client, pending_temp_dir,
         done = std::move(done)]() mutable
        {
            auto io = std::make_shared<FinalizeLoginIO>(
                finalize_login_blocking_(std::move(*pending_client),
                                          std::move(pending_temp_dir))); // worker thread
            post_to_ui_alive_(
                [this, io, done = std::move(done)]() mutable
                {
                    if (io->result.ok && io->session)
                    {
                        // Per-account notifier (native) and Linux-only UnifiedPush
                        // connector — must run on the UI thread (native toolkit
                        // objects bound to the window/main-loop).
                        install_account_notifier_(*io->session);
                        install_account_up_connector_(*io->session);

                        const std::string added_uid = io->session->user_id;
                        account_manager_.add_account(std::move(io->session));

                        // Update the on-disk index. Active = the account we just added.
                        auto index = tesseract::SessionStore::load_index();
                        if (std::find(index.user_ids.begin(), index.user_ids.end(),
                                       added_uid) == index.user_ids.end())
                        {
                            index.user_ids.push_back(added_uid);
                        }
                        index.active_user_id = added_uid;
                        tesseract::SessionStore::save_index(index);
                    }
                    done(std::move(io->result));
                });
        });
}

// Called by each shell right after it activates the newly-added account
// (switchActiveAccount / equivalent) inside its finalize_login_async_
// `done` callback. A no-op unless `fin.needs_encryption_setup` is set, in
// which case it raises the encryption-setup overlay in the right mode and
// remembers the session so release_pending_sync_gate_() can start its
// sync once the user is done with the overlay.
void ShellBase::begin_gated_encryption_setup_if_needed_(const FinalizeLoginResult& fin)
{
    if (!fin.needs_encryption_setup)
        return;

    auto sess = account_manager_.find(fin.user_id);
    if (!sess)
        return;

    pending_sync_session_       = sess;
    encryption_setup_dismissed_ = false;
    encryption_setup_shown_     = true;
    using Mode = tesseract::views::EncryptionSetupOverlay::Mode;
    if (!fin.encryption_setup_recover_mode)
    {
        // A brand-new account: set recovery up quietly. It's quick on an
        // account with no history, so the gate needn't hold sync for it.
        silent_recovery_new_account_.insert(sess->user_id);
        if (begin_silent_recovery_setup_(sess))
        {
            release_pending_sync_gate_();
            return;
        }
    }
    if (!main_app_)
        return;
    if (fin.encryption_setup_recover_mode)
    {
        // The gate is released by a successful silent unlock, or by closing
        // the Recover dialog it falls back to.
        show_recover_or_silent_unlock_(sess);
        return;
    }
    show_encryption_setup_overlay_(Mode::Fresh);
    request_relayout_();
}

void ShellBase::release_pending_sync_gate_()
{
    auto sess = std::move(pending_sync_session_);
    pending_sync_session_.reset();
    if (!sess || !sess->client || sess->sync_started)
        return;
    run_async_mut_([sess]() {
        sess->client->start_sync(sess->bridge.get());
        sess->sync_started = true;
    });
}

// Platform-agnostic account-switch bookkeeping, shared by every shell's
// switchActiveAccount / switch_active_account / _switchActiveAccount:. Looks
// up the target AccountSession; returns false (no-op) if it isn't found or is
// already active with a bound client. Otherwise it:
//   - unsubscribes the previous account's open room when not pinned
//     (room_subscription_refs_.count(current_room_id_) == 0) so the old
//     account's timeline stops streaming after the surface swap — folded in
//     from the Phase-1.2 fix so ALL shells get it;
//   - clears per-account, room-id-keyed state (current_room_id_, tabs_,
//     active_tab_idx_, space_stack_, pagination_, reply_details_requested_)
//     so it can't bleed into the incoming account;
//   - forgets any in-progress interactive verification, resets server
//     info, swaps active_account_ + the client_ / event_handler_ aliases and
//     the my_user_id_ / my_display_name_ / my_avatar_url_ identity;
//   - computes pending_restore_rooms_ from open_rooms / last_room (rotating
//     last_room to [0]) and populate_pending_restore_popouts_();
//   - rebinds settings_controller_ (client + up_connector) when present;
//   - swaps the per_account_rooms_ / per_account_invites_ snapshots into
//     rooms_ / invites_, fires on_invites_updated_(), drops current_invite_;
//   - persists the on-disk index (active = the new uid).
// It does NOT touch native widgets (user strip, room-list view, message
// surface, status bar, tray) — the shell does that in
// refresh_account_ui_after_switch_(). UI-thread only.
bool ShellBase::switch_active_account_impl_(const std::string& user_id)
{
    auto new_session = account_manager_.find(user_id);
    if (!new_session)
    {
        return false;
    }
    if (new_session == active_account_ && client_)
    {
        return false;
    }

    // Unsubscribe every room timeline the previous account keeps live for
    // this window — the open room, its tabs, the warm LRU and any thread
    // timelines — so none of them keeps streaming events through this shell
    // once the incoming account's rooms are shown. Only the bookkeeping for
    // these used to be cleared below; the SDK subscriptions stayed live.
    // Rooms pinned by a pop-out keep their subscription (pop-outs stay open
    // across a switch). Must happen before client_ is reassigned to the
    // incoming account.
    if (client_)
    {
        auto pinned = [this](const std::string& rid)
        { return room_pinned_by_popout_(rid); };
        std::unordered_set<std::string> rooms;
        if (!current_room_id_.empty())
            rooms.insert(current_room_id_);
        for (const auto& t : tabs_)
            rooms.insert(t.room_id);
        rooms.insert(visited_lru_.begin(), visited_lru_.end());
        for (const auto& kv : room_last_active_)
            rooms.insert(kv.first);
        for (auto it = thread_last_active_.begin(); it != thread_last_active_.end();)
        {
            if (pinned(it->first.first))
            {
                ++it;
                continue;
            }
            client_->unsubscribe_thread(it->first.first, it->first.second);
            it = thread_last_active_.erase(it);
        }
        for (const auto& rid : rooms)
        {
            if (rid.empty() || pinned(rid))
                continue;
            client_->unsubscribe_room(rid);
            room_last_active_.erase(rid);
        }
    }

    // Drop per-account, room-id-keyed state so it can't bleed into the next
    // account (a room id present in both accounts would otherwise inherit stale
    // pagination / space-drill / reply-fetch state).
    current_room_id_.clear();
    tabs_.clear();
    active_tab_idx_ = 0;
    if (main_room_pane_)
        main_room_pane_->clear_compose_drafts_();
    space_stack_.clear();
    space_nav_frames_.clear();
    ++unjoined_fetch_gen_;
    unjoined_fetch_pending_.clear();
    pending_summaries_.clear();
    unjoined_fetch_retry_.clear();
    active_space_id_.clear();
    // Rooms kept open by the outgoing account's pop-outs stay subscribed, so
    // their scroll-back state stays valid: park it under that account.
    if (active_account_)
    {
        auto& parked = other_account_pagination_[active_account_->user_id];
        for (auto& [rid, st] : pagination_)
            if (room_pinned_by_popout_(rid))
                parked[rid] = st;
    }
    pagination_.clear();
    visited_lru_.clear(); // warm-subscription LRU is per-account
    reply_details_requested_.clear();
    // URL-preview / blurhash-decode / tile-fetch dedup + result caches are
    // keyed by URL, not room, so nothing else prunes them per-room — without
    // this they'd otherwise grow for the life of the process across every
    // account switch. See also clear_all_caches_() and
    // logout_active_account_impl_(), which clear the same set for the
    // explicit "Clear Cache" action and full sign-out respectively.
    url_previews_.clear();
    url_preview_data_.clear();
    url_preview_in_flight_.clear();
    blurhash_attempted_.clear();
    tile_fetch_failed_.clear();
    // Per-member gendered-pronoun cache is keyed by user_id, scoped to
    // whichever account is fetching it — drop it on account switch same as
    // the other per-account caches above.
    member_gender_cache_.clear();
    member_gender_inflight_.clear();
    pending_member_gender_requests_.clear();
    // In-flight searches belong to the outgoing account; their responses route
    // to that account's (possibly different) shell, so drop the pending map and
    // any debounced query here rather than leaking entries for them.
    cancel_debounce_(DebounceSlot::MessageSearch);
    search_pending_queries_.clear();

    // An interactive verification belongs to the outgoing account's client:
    // its events stop routing here. Cancel it on that client (still active at
    // this point) and take down the dialog if it's showing it, or its buttons
    // would act on the wrong account.
    // The same goes for a dialog about the outgoing account's recovery key.
    if (auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
        o && o->visible() &&
        (o->in_verification_step() ||
         o->step() == views::EncryptionSetupOverlay::Step::VerifyFailed ||
         o->mode() == views::EncryptionSetupOverlay::Mode::Verify ||
         o->mode() == views::EncryptionSetupOverlay::Mode::SaveKey ||
         o->mode() == views::EncryptionSetupOverlay::Mode::AutoSetupNotice))
        main_app_->show_encryption_setup(false);
    cancel_sign_out_();
    cancel_active_verification_();
    foreign_identity_known_true_ = false;
    last_device_verified_.reset();

    // Multi-window: release the outgoing account's dedicated mapping if it points
    // at this window; the incoming account is claimed at the tail of the switch.
    if (active_account_ &&
        account_manager_.dedicated_window(active_account_->user_id) == this)
    {
        account_manager_.clear_dedicated(active_account_->user_id);
    }

    // Drop per-account backoff state so it can't bleed into the incoming account.
    media_fetch_failed_.clear();

    reset_server_info_();
    push_own_status_to_strip_(); // blank the strip status until the new fetch
    // Keep the outgoing account's MRU on its session so switching back (or the
    // next save of that account's prefs) sees its latest visits.
    if (active_account_)
        active_account_->recent_rooms = recent_room_ids_;
    active_account_ = new_session;
    auto& sess = *active_account_;
    // ...and bring back the incoming account's own pop-out rooms' state.
    if (auto it = other_account_pagination_.find(sess.user_id);
        it != other_account_pagination_.end())
    {
        pagination_ = std::move(it->second);
        other_account_pagination_.erase(it);
    }

    client_ = sess.client.get();
    event_handler_ = sess.bridge.get(); // keep ShellBase's non-owning alias in sync

    // Cover the startup race where this account's room list already reached
    // Running (bridge construction + start_sync run on a worker thread ahead
    // of this call, so a fast sync can beat us here) before client_ existed
    // to make the request — push_room_list_state_/EventHandlerBase deferred
    // it in that case (begin_server_info_fetch_ already no-ops on !client_
    // without consuming its own one-shot guard, so it's safe to retry here).
    if (last_room_list_state_ == RoomListState::Running)
    {
        begin_server_info_fetch_();
        if (tesseract::Settings::instance().check_for_updates)
            trigger_update_check_();
    }

    // Restore persisted backoff state for the incoming account (DB is open by
    // the time start_sync returns, which happens before activate_account_).
    for (const auto& entry : client_->load_media_backoff())
    {
        using SC     = std::chrono::system_clock;
        using Steady = std::chrono::steady_clock;
        const auto stored    = SC::time_point{std::chrono::seconds{entry.deadline_secs}};
        const auto remaining = stored - SC::now();
        MediaFetchBackoff b;
        b.attempts   = entry.attempts;
        b.retry_after = Steady::now()
                      + std::max(std::chrono::seconds::zero(),
                                 std::chrono::duration_cast<std::chrono::seconds>(remaining));
        media_fetch_failed_[entry.url] = b;
    }
    for (const auto& entry : client_->load_room_summary_backoff())
    {
        using SC     = std::chrono::system_clock;
        using Steady = std::chrono::steady_clock;
        const auto stored    = SC::time_point{std::chrono::seconds{entry.deadline_secs}};
        const auto remaining = stored - SC::now();
        if (remaining <= std::chrono::seconds::zero())
        {
            client_->note_room_summary_backoff_ok(entry.room_id); // prune expired row
            continue;
        }
        UnjoinedRetryState rs;
        rs.attempts  = static_cast<int>(entry.attempts);
        rs.next_retry = Steady::now()
                      + std::chrono::duration_cast<Steady::duration>(remaining);
        unjoined_fetch_retry_[entry.room_id] = rs;
    }

    my_user_id_ = sess.user_id;
    recent_room_ids_ = sess.recent_rooms;
    if (recent_room_ids_.size() > kRecentRoomsMax)
        recent_room_ids_.resize(kRecentRoomsMax);
    my_display_name_ = sess.display_name;
    my_avatar_url_ = sess.avatar_url;
    own_room_avatar_.clear();
    own_room_avatar_in_flight_.clear();
    own_room_avatar_dirty_.clear();
    strip_avatar_room_.clear();
    restore_gate_ticks_ = 0;
    pending_restore_rooms_ = sess.open_rooms.empty()
        ? (sess.last_room.empty() ? std::vector<std::string>{}
                                  : std::vector<std::string>{sess.last_room})
        : sess.open_rooms;
    // Rotate last_room to [0] so it opens as the active tab.
    if (!sess.last_room.empty() && !pending_restore_rooms_.empty() &&
        pending_restore_rooms_[0] != sess.last_room)
    {
        auto it = std::find(pending_restore_rooms_.begin(),
                            pending_restore_rooms_.end(), sess.last_room);
        if (it != pending_restore_rooms_.end())
            std::rotate(pending_restore_rooms_.begin(), it, it + 1);
    }
    pending_restore_popouts_.clear();
    populate_pending_restore_popouts_();

    if (settings_controller_)
    {
        settings_controller_->set_client(client_, active_account_);
        settings_controller_->set_up_connector(sess.up_connector.get());
    }
    // Same for history export: an export started on the outgoing account is
    // cancelled there, and new requests go through the incoming client.
    if (history_export_controller_)
        history_export_controller_->set_client(client_, active_account_);

    // Use this account's last-known rooms snapshot if cached; otherwise leave
    // rooms_ empty and wait for the next on_rooms_updated_ callback. The native
    // room-list refresh happens in refresh_account_ui_after_switch_().
    auto it = per_account_rooms_.find(my_user_id_);
    rooms_ = (it != per_account_rooms_.end()) ? it->second
                                              : std::vector<tesseract::RoomInfo>{};
    mark_room_index_dirty_();
    // The known-user roster belongs to the previous account — drop it.
    invalidate_known_users_();
    // The unread-prefetch and bridge-check fingerprints are per-account; reset
    // so the incoming account re-fires on the first push_rooms_ after the switch.
    unread_prefetch_fingerprint_ = 0;
    bridge_check_fingerprint_    = 0;

    // Restore the invite snapshot for the incoming account (parallel to rooms_).
    auto inv_it = per_account_invites_.find(my_user_id_);
    invites_ = (inv_it != per_account_invites_.end())
                   ? inv_it->second
                   : std::vector<tesseract::InviteInfo>{};
    on_invites_updated_();

    // Dismiss any stale InviteCard from the previous account.
    current_invite_.reset();

    // Restore the pending-knock snapshot for the incoming account (parallel
    // to invites_ above).
    auto knocks_it = per_account_my_knocks_.find(my_user_id_);
    my_knocks_ = (knocks_it != per_account_my_knocks_.end())
                     ? knocks_it->second
                     : std::vector<tesseract::KnockedRoomInfo>{};
    on_my_knocks_updated_();

    // Dismiss any stale KnockStatusCard from the previous account. Also
    // drop (without unsubscribing — client_ already points at the new
    // account by this point) any admin-side knock-requests panel state;
    // the previous account's Rust-side watcher, if any, is harmlessly
    // cleaned up when that account's ClientFfi is eventually dropped.
    current_knock_status_room_id_.clear();
    knock_requests_panel_room_id_.clear();
    current_room_knock_requests_.clear();

    // Persist the active selection on disk (active = the new uid).
    auto index = tesseract::SessionStore::load_index();
    index.active_user_id = my_user_id_;
    tesseract::SessionStore::save_index(index);

    // Multi-window: this window now owns the incoming account — register it so the
    // account picker raises this window instead of switching in place elsewhere.
    claim_dedicated_for_active_();

    // This window's tray label (if it owns the tray) tracks active_account_,
    // which just changed — refresh every window's menu to pick it up.
    broadcast_rebuild_tray_();

    return true;
}

void ShellBase::rebind_account_bridge_(tesseract::AccountSession& session,
                                       ShellBase* win)
{
    // The bridge is stored type-erased as IEventHandler* but is always an
    // EventHandlerBase (every shell's bridge derives it); safe downcast.
    if (session.bridge)
        static_cast<EventHandlerBase*>(session.bridge.get())->set_shell(win);
}

void ShellBase::seed_account_caches_from_(ShellBase* src, const std::string& uid)
{
    if (!src || src == this)
        return;
    auto r = src->per_account_rooms_.find(uid);
    if (r != src->per_account_rooms_.end())
        per_account_rooms_[uid] = r->second;
    auto i = src->per_account_invites_.find(uid);
    if (i != src->per_account_invites_.end())
        per_account_invites_[uid] = i->second;
}

// Shared spawn wiring: hand ownership of `session`'s account to a freshly
// constructed window `win` (whose set_initial_account() has already run, but
// whose deferred doLogin() has NOT). Called from spawn_main_window_() on the
// spawning window. It (1) re-points the account's sole event bridge at `win`
// so every SDK callback now reaches it, (2) seeds `win`'s room/invite caches
// from this window so its list paints immediately instead of waiting for the
// next sync push, (3) marks `win` pinned, and (4) registers `win` as the
// dedicated window for the account.
void ShellBase::hand_account_to_spawned_window_(
    ShellBase* win, const std::shared_ptr<tesseract::AccountSession>& session)
{
    if (!win || !session)
        return;
    // Re-point the account's sole bridge so its SDK callbacks now reach the new
    // owner window instead of this (the spawning) window.
    rebind_account_bridge_(*session, win);
    // Seed the new window's caches from ours so its room list paints immediately;
    // the deferred doLogin()→switch reads per_account_rooms_ for this uid.
    win->seed_account_caches_from_(this, session->user_id);
    win->mark_pinned_window_();
    account_manager_.set_dedicated(session->user_id, win);
}

void ShellBase::claim_dedicated_for_active_()
{
    if (!active_account_)
        return;
    const std::string& uid = active_account_->user_id;
    // Don't steal the mapping from another live window already showing this
    // account (e.g. a pinned pop-out). A switch can target an owned account via
    // a path that bypasses the picker's raise-existing check — a notification
    // click or logout-to-survivor — and overwriting here would make the picker
    // raise the wrong window and strand the pop-out (whose bridge still owns the
    // account). Only claim when the account is unowned or already ours.
    ShellBase* cur = account_manager_.dedicated_window(uid);
    if (cur && cur != this)
        return;
    account_manager_.set_dedicated(uid, this);
}

void ShellBase::release_dedicated_for_active_()
{
    if (!active_account_)
        return;
    const std::string uid = active_account_->user_id;
    if (account_manager_.dedicated_window(uid) != this)
        return;
    account_manager_.clear_dedicated(uid);
    // Re-point the mapping to another live window still showing this account
    // (prefer a non-pinned / primary window so the picker raises the long-lived
    // one), if any.
    ShellBase* fallback = nullptr;
    for (ShellBase* w : account_manager_.all_windows())
    {
        if (w == this)
            continue;
        auto a = w->active_account_;
        if (a && a->user_id == uid)
        {
            fallback = w;
            if (!w->is_pinned_window())
                break;
        }
    }
    if (fallback)
        account_manager_.set_dedicated(uid, fallback);
}

void ShellBase::on_window_closing_()
{
    // Flush any pending room-layout save synchronously — but only when this
    // is the last open window (about to end the process): all windows share
    // one UI thread, so blocking here would otherwise stall every other
    // still-open window (e.g. a secondary per-account window) for the sake
    // of a save that isn't actually racing shutdown yet. window_count() still
    // counts `this` at this point (unregister_window runs after), so <= 1
    // means nobody survives this close.
    //
    // schedule_account_data_save_() already covers the steady-state case
    // (debounced saves as the user switches rooms/tabs, so a crash or forced
    // kill loses at most a couple of seconds of changes), but the save that
    // matters most is the very last one — and save_prefs_json's fire-and-
    // forget spawn has no guard against losing its race with the process
    // exit right behind this call. blocking=true skips the write entirely
    // when nothing changed since the last debounced save, and otherwise
    // waits (bounded) for the PUT to actually land before returning.
    const bool is_last_window = account_manager_.window_count() <= 1;
    persist_room_layout_pref_(/*blocking=*/is_last_window);
    // Hand this window's account's sole event bridge back to the primary window so
    // its SDK callbacks keep reaching a live window after we're destroyed. The
    // primary uses hide-to-tray and is never destroyed while secondaries live, so
    // it is always a valid target.
    ShellBase* primary = account_manager_.primary_window();
    if (primary && primary != this && active_account_)
        rebind_account_bridge_(*active_account_, primary);
    release_dedicated_for_active_();
    account_manager_.release_tray_owner(this);
}

// Platform-agnostic teardown for each shell's logoutActiveAccount /
// logout_active_account / _logoutActiveAccount. Run on the active account; a
// no-op (logged_out=false) when there is none. It:
//   - captures the active uid;
//   - calls client_->request_stop() FIRST, so any run_async_mut_ worker
//     already queued or mid-flight against this client (a cancellable
//     block_on — poll_presence_now, subscribe_room, send_message, ...)
//     unblocks immediately instead of running its own HTTP timeout/retry
//     budget while the drain below waits on it;
//   - unsubscribes the current open room when not pinned by a pop-out
//     (room_subscription_refs_.count(current_room_id_) == 0) — same guard as
//     switch_active_account_impl_, folded in so Qt/Win get it too;
//   - logs out the UnifiedPush connector (when present) and presence;
//   - calls client_->logout() and SURFACES a failure via show_status_message_
//     ("Sign out failed: <msg>") — converged so every shell reports it;
//   - stop_sync() (BEFORE remove_account, per Phase-1 lifetime ordering);
//   - clears the on-disk account (SessionStore::clear_account) and the
//     per_account_rooms_ / per_account_invites_ snapshots;
//   - refreshes the tray aggregate (notify_tray_unread_) so a stale unread dot
//     clears — converged so every shell does it;
//   - marks the uid draining (AccountManager::mark_draining) BEFORE removing
//     it from AccountManager, so there is no window where it's neither
//     findable nor flagged; removes the account, resets active_account_ / the
//     client_ / event_handler_ aliases, and the agnostic visible state
//     (rooms_/invites_/current_invite_/space_stack_/identity/pagination/…);
//   - posts a barrier task to mut_pool_ (via run_async_mut_) that drops the
//     session's last reference and clears the draining flag — mut_pool_ is a
//     strict single-thread FIFO, so this is guaranteed to run only after every
//     earlier-queued-or-in-flight task that captured the session has finished
//     — then bound-waits on it (AccountManager::wait_until_drained,
//     kAccountDrainTimeout) so the old session's SQLite-backed store is either
//     fully closed, or the wait has at least given request_stop() a fair
//     chance to unblock it, before this function returns;
//   - updates the on-disk index (removes the logged-out uid; clears
//     active_user_id when none remain);
//   - BRANCHES: if other accounts remain it switches to accounts().front()
//     via switch_active_account_impl_ + refresh_account_ui_after_switch_ (the
//     shared Task-3.3 path) and returns has_remaining=true / next_uid set;
//     otherwise returns has_remaining=false and leaves the native login-view
//     swap to the shell.
// Does NOT touch native widgets in the empty-accounts branch (login view,
// surface visibility) — the shell does that using the returned result.
// UI-thread only.
ShellBase::LogoutResult ShellBase::logout_active_account_impl_()
{
    LogoutResult out;
    if (!active_account_)
    {
        return out; // logged_out=false → shell does nothing
    }

    const std::string uid = active_account_->user_id;
    auto& sess = *active_account_;
    out.logged_out     = true;
    out.logged_out_uid = uid;

    settle_recovery_key_on_sign_out_(uid);

    // Deferred "New messages" moves go out first: request_stop() below
    // would cancel them.
    flush_pending_fully_read_now_(uid);

    // Signal any run_async_mut_ worker already queued or mid-flight against
    // this client (a cancellable block_on — poll_presence_now, subscribe_room,
    // send_message, ...) to give up immediately, rather than running its own
    // HTTP timeout/retry budget while the drain further down waits on it.
    // SH_FFI — does not itself queue behind whatever it's trying to interrupt.
    if (client_)
    {
        client_->request_stop();
    }

    // Unsubscribe the current open room unless it's pinned by a pop-out — the
    // same guard switch_active_account_impl_ uses. Folded in so Qt/Win get it
    // too (they previously skipped this, leaking a streaming timeline sub).
    if (client_ && !current_room_id_.empty() &&
        !room_pinned_by_popout_(current_room_id_))
    {
        client_->unsubscribe_room(current_room_id_);
    }
    current_room_id_.clear();

    // Tear down the per-account UnifiedPush connector + presence, then sign the
    // session out of the homeserver. Surface a failure on every shell.
    if (sess.up_connector)
    {
        sess.up_connector->logout();
    }
    notify_presence_logout_();
    // client_->logout() now also explicitly closes the SQLite-backed stores
    // (state/event-cache/media — see the long comment on ClientFfi::logout in
    // sdk/src/client/session.rs) before returning, which can take several
    // seconds. Dispatch it to mut_pool_ instead of calling it inline: calling
    // it synchronously here froze the UI thread for that whole duration,
    // which is what let a stale queued UI event (e.g. an image-packs-updated
    // notification for this same account, sitting in the queue since before
    // logout was even clicked) get processed only *after* the old Client had
    // already been destroyed — a genuine access-violation use-after-free, not
    // just a slow logout.
    //
    // logout() internally calls stop_sync() as its own first step, so the
    // separate synchronous sess.client->stop_sync() this function used to
    // make right after is now not just redundant but actively harmful: it
    // would contend with the just-backgrounded logout() call for the same
    // exclusive FFI lock, blocking the UI thread anyway while it waits its
    // turn. Removed below in favor of request_stop() (already called above,
    // fast, SH_FFI) having already told every task to give up.
    //
    // Captures its own shared_ptr copy of the session — not the raw client_
    // alias, which this function nulls out further down — so the Client
    // stays alive for exactly as long as this task needs it, independent of
    // whatever ShellBase does with active_account_/client_ afterward.
    if (client_)
    {
        auto sess_for_logout = active_account_;
        run_async_mut_(
            [this, sess_for_logout]()
            {
                const auto res = sess_for_logout->client->logout();
                if (!res)
                {
                    post_to_ui_alive_(
                        [this, message = res.message]()
                        {
                            show_status_message_(
                                tk::trf(tk::tr("Sign out failed: {0}"), {message}));
                        });
                }
            });
    }
    sess.sync_started = false;

    per_account_rooms_.erase(uid);
    per_account_invites_.erase(uid);

    // Recompute the tray aggregate so the dot clears (or rolls over to the
    // surviving accounts) immediately; without this the indicator can stick
    // when the only account with unreads was the one we just signed out.
    notify_tray_unread_();

    // Drop any dedicated-window mapping for the now-removed account so the picker
    // can't try to raise a window for it.
    account_manager_.clear_dedicated(uid);
    // Mark draining BEFORE removing so there is no window where uid is
    // neither findable via AccountManager::find() nor flagged as draining.
    account_manager_.mark_draining(uid);
    account_manager_.remove_account(uid);
    // Move (not reset) so the barrier task posted below — not this function
    // — drops the session's last ShellBase-held reference, off the UI
    // thread. `sess` is not referenced again after this point.
    // Pop-outs act through their own account's Client, so this account's
    // must close before that Client goes; other accounts' pop-outs stay.
    close_popouts_for_account_(uid);

    // Cancel any export running on this account and drop the controller's
    // pointer before the Client is handed off for destruction; the
    // remaining-account branch rebinds it in switch_active_account_impl_.
    if (history_export_controller_)
        history_export_controller_->set_client(nullptr);
    // Same for the settings controller: its jobs hold the session while they
    // run, and the drain below waits for exactly those references.
    if (settings_controller_)
        settings_controller_->set_client(nullptr);
    auto draining_sess = std::move(active_account_);
    client_        = nullptr;
    event_handler_ = nullptr;

    // Reset the agnostic visible state (native widget clearing stays in the
    // shell: the remaining-account branch repaints via
    // refresh_account_ui_after_switch_, the empty branch via the login view).
    space_stack_.clear();
    space_nav_frames_.clear();
    ++unjoined_fetch_gen_;
    unjoined_fetch_pending_.clear();
    pending_summaries_.clear();
    unjoined_fetch_retry_.clear();
    active_space_id_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    own_room_avatar_.clear();
    own_room_avatar_in_flight_.clear();
    own_room_avatar_dirty_.clear();
    strip_avatar_room_.clear();
    rooms_.clear();
    mark_room_index_dirty_();
    invites_.clear();
    current_invite_.reset();
    my_knocks_.clear();
    current_knock_status_room_id_.clear();
    knock_requests_panel_room_id_.clear();
    current_room_knock_requests_.clear();
    pagination_.clear();
    visited_lru_.clear(); // warm-subscription LRU is per-account
    reply_details_requested_.clear();
    // See the matching comment in switch_active_account_impl_().
    url_previews_.clear();
    url_preview_data_.clear();
    url_preview_in_flight_.clear();
    blurhash_attempted_.clear();
    tile_fetch_failed_.clear();
    member_gender_cache_.clear();
    member_gender_inflight_.clear();
    pending_member_gender_requests_.clear();
    reset_server_info_();
    push_own_status_to_strip_(); // blank the strip status on logout

    // mut_pool_ is a strict single-thread FIFO (see run_async_mut_'s doc
    // comment), so a barrier posted there is guaranteed to run only after
    // every mut_pool_ task that could hold a stray reference to draining_sess
    // has finished — no polling needed for that half.
    //
    // But plenty of run_async_(...) call sites elsewhere in this file also
    // capture `sess` (a shared_ptr<AccountSession>) by value and run on
    // pool_ (2 threads, shared across every account). mut_pool_ ordering
    // says nothing about those: if one is still executing right now,
    // draining_sess.reset() below just decrements a refcount that never
    // reaches zero, the old Client (and its SQLite handles) never actually
    // gets destroyed, and clear_account()'s fs::remove_all silently fails to
    // delete anything — confirmed via Resource Monitor showing dozens of
    // open handles on the account's -wal/-shm files that persisted past
    // logout.
    if (account_manager_.accounts().empty())
    {
        // No other account can be affected by pool_ going idle, so wait for
        // it exactly like full app shutdown already does (see e.g.
        // MacShell::drain_pools / the Windows/Qt/GTK shutdown paths) — except
        // wait_idle() doesn't stop the pool or join its threads, so a
        // re-login within the same process still has a working pool_/
        // mut_pool_ afterward.
        pool_.wait_idle(kAccountDrainTimeout);
        mut_pool_.wait_idle(kAccountDrainTimeout);
        // media_prefetch_pool_ deliberately not waited on here: its tasks
        // only ever capture `this` and a MediaPrefetchBatch, never an
        // AccountSession, so there's nothing for this drain step to release.
    }
    else
    {
        // Other accounts remain logged in, so pool_ can't be waited on as a
        // whole without stalling their unrelated in-flight work. There's no
        // per-account tracking inside pool_ to wait on instead, so fall back
        // to polling draining_sess's own refcount — the same bounded,
        // proceed-anyway-on-timeout philosophy as everything else here, just
        // without the ordering trick mut_pool_ gets for free.
        const auto deadline =
            std::chrono::steady_clock::now() + kAccountDrainTimeout;
        while (draining_sess.use_count() > 1 &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // This is where the old Client (and its SQLite-backed store) actually
    // gets destroyed, off the UI thread.
    //
    // clear_account() (fs::remove_all on the account dir) runs AFTER
    // draining_sess.reset(), not before: it used to run synchronously up in
    // this function while sess.client was still alive, so remove_all raced
    // an open SQLite handle and silently failed to delete on Windows
    // (its error code was discarded) — leaving stale store files that a
    // later re-login's rename-then-copy-fallback would merge with a fresh
    // store, corrupting it (SQLITE_CORRUPT: "database disk image is
    // malformed"). clear_draining() only fires once the directory is
    // actually gone, so wait_until_drained() callers (finalize_login_async_'s
    // second-line-of-defense check included) never observe "drained" while
    // the delete is still in flight.
    run_async_mut_(
        [this, uid, draining_sess = std::move(draining_sess)]() mutable
        {
            draining_sess.reset();
            tesseract::SessionStore::clear_account(uid);
            account_manager_.clear_draining(uid);
        });
    // Bounded: fires within microseconds in the common case (nothing was
    // queued against this account). A genuinely stuck worker just means we
    // proceed anyway — the flag stays set and a later wait_until_drained()
    // call (this function's next invocation, or finalize_login_async_'s
    // second-line-of-defense check) still observes the truth.
    account_manager_.wait_until_drained(uid, kAccountDrainTimeout);

    // Update the on-disk index: drop the logged-out uid.
    auto index = tesseract::SessionStore::load_index();
    index.user_ids.erase(
        std::remove(index.user_ids.begin(), index.user_ids.end(), uid),
        index.user_ids.end());

    if (account_manager_.accounts().empty())
    {
        index.active_user_id.clear();
        tesseract::SessionStore::save_index(index);
        // Unlike the has_remaining branch below, there's no incoming account
        // to re-point this at via switch_active_account_impl_, so it must be
        // nulled out explicitly — otherwise it keeps referencing the Client
        // this function just handed to draining_sess for async destruction.
        // Already null-safe (see test_settings_controller.cpp's stale-result
        // test), so this just prevents a permanently-stale cached pointer,
        // not a crash.
        if (settings_controller_)
        {
            settings_controller_->set_client(nullptr);
        }
        out.has_remaining = false;
        // No survivor to switch_active_account_impl_ into (which would
        // otherwise refresh the tray itself) — this window's now-empty
        // active_account_ still needs to be reflected in every menu.
        broadcast_rebuild_tray_();
        return out;
    }

    // Other accounts remain → switch to the first survivor via the shared
    // Task-3.3 path (it persists the index's active_user_id itself). The native
    // post-switch UI runs inside refresh_account_ui_after_switch_().
    const std::string next_uid = account_manager_.accounts().front()->user_id;
    tesseract::SessionStore::save_index(index); // persist the uid removal first
    if (switch_active_account_impl_(next_uid))
    {
        refresh_account_ui_after_switch_();
    }
    out.has_remaining = true;
    out.next_uid      = next_uid;
    return out;
}

// The sign-out sequence shared by every shell's entry point
// (logoutActiveAccount / logout_active_account / _logoutActiveAccount):
// offer to save an unsaved recovery key first (`retry` re-enters the shell's
// own sign-out once the user has decided), run logout_active_account_impl_(),
// and — when no accounts remain — clear the native-widget-free main UI.
// `on_signed_out` then runs only when an account was actually signed out and
// carries the shell's native follow-up (room-list refresh, relayout, status
// text, login-view swap). login_view_ is native per shell, so it is never
// touched here. UI-thread only.
void ShellBase::sign_out_active_account_(std::function<void()> retry,
                                         const std::function<void(const LogoutResult&)>& on_signed_out)
{
    // An unsaved recovery key is offered for saving first; this re-enters
    // (via `retry`) once the user has saved it or chosen to sign out anyway.
    if (intercept_sign_out_for_unsaved_key_(std::move(retry)))
        return;

    // Platform-agnostic teardown (unsubscribe the room, up_connector/presence
    // logout, client_->logout() + failure surface, stop_sync, clear account
    // state, tray refresh, index update, and — when other accounts remain — the
    // switch to a survivor) lives in logout_active_account_impl_.
    const auto result = logout_active_account_impl_();
    if (!result.logged_out)
    {
        return;
    }

    // Cleanup of the now-empty surface (the remaining-account branch already
    // repainted via refresh_account_ui_after_switch_).
    if (!result.has_remaining)
    {
        clear_main_ui_after_last_logout_();
    }

    if (on_signed_out)
    {
        on_signed_out(result);
    }
}

void ShellBase::clear_main_ui_after_last_logout_()
{
    if (room_view_)
    {
        room_view_->clear_room();
        room_view_->set_messages({});
        // Drop RoomView's (and its EmojiPicker/StickerPicker's) cached raw
        // Client* — it's never re-pointed once there's no survivor to
        // switch to, and the old Client is about to be destroyed
        // asynchronously by logout_active_account_impl_'s drain barrier.
        room_view_->set_client(nullptr);
    }
    if (main_app_)
    {
        main_app_->clear_content();
        main_app_->show_encryption_reminder(false);
    }
}

Client* ShellBase::reset_pending_login_client_()
{
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<Client>();
    return pending_login_client_.get();
}

void ShellBase::restart_sdk_begin_(
    std::function<void(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t)> recompute_callback)
{
    if (!client_ || my_user_id_.empty())
        return;
    // load_account_with_key(), not load_account(): the latter returns the raw
    // SecretStore blob, which for a store-key account is the wrapper
    // {"tesseract_store_key_wrapper":…,"session":…,"store_key":…} — restore_session
    // then chokes on it ("missing field `client_id`"). Mirror the startup
    // restore path (restore_accounts_blocking_).
    auto loaded = tesseract::SessionStore::load_account_with_key(my_user_id_);
    if (!loaded)
    {
        show_status_message_(
            tk::tr("Couldn't clear the cache: this account's session is missing."));
        return;
    }

    // ── Phase A (UI thread): tear the account's UI state down ────────────────

    // Deferred "New messages" moves go out first: request_stop() would
    // cancel them.
    flush_pending_fully_read_now_(my_user_id_);

    // Tell any in-flight run_async_mut_ task to give up before Phase B queues
    // behind it for the exclusive FFI lock.
    client_->request_stop();

    // Deselect the active room / thread panel — cached timeline data is going.
    {
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
    }

    // Forget the open-tab layout entirely: clear it locally now, and Phase B
    // pushes an empty im.gnomos.tesseract account-data event (while sync is
    // still live) so the resync can't bring the tabs back.
    // Only the layout (and the recent-rooms history) is forgotten — bridge
    // overrides, the emoji skin tone
    // and any keys this build doesn't write are carried over.
    auto empty_layout_data = tesseract::Prefs::room_layout(std::string{}, {});
    if (active_account_)
    {
        empty_layout_data.bridge_not_bridged_overrides =
            active_account_->bridge_not_bridged_overrides;
        empty_layout_data.emoji_skin_tone =
            tesseract::emoji::skin_tone_key(active_account_->emoji_skin_tone);
    }
    const std::string empty_layout = tesseract::Prefs::serialize(
        empty_layout_data, active_account_ ? active_account_->prefs_json : std::string{});
    current_room_id_.clear();
    tabs_.clear();
    active_tab_idx_ = 0;
    recent_room_ids_.clear();
    if (active_account_)
    {
        active_account_->open_rooms.clear();
        active_account_->last_room.clear();
        active_account_->recent_rooms.clear();
    }
    account_data_dirty_ = false;
    pending_restore_rooms_.clear();

    // Close every pop-out window for this account and forget them.
    close_all_popouts_();

    // Drop every per-account, room-keyed cache (superset of the account-switch
    // reset in switch_active_account_impl_).
    if (main_room_pane_)
        main_room_pane_->clear_compose_drafts_();
    space_stack_.clear();
    space_nav_frames_.clear();
    ++unjoined_fetch_gen_;
    unjoined_fetch_pending_.clear();
    pending_summaries_.clear();
    unjoined_fetch_retry_.clear();
    active_space_id_.clear();
    pagination_.clear();
    other_account_pagination_.clear(); // every pop-out was closed above
    visited_lru_.clear();
    reply_details_requested_.clear();
    // MSC4278 per-account gating state.
    room_preview_overrides_.clear();
    room_preview_override_in_flight_.clear();
    pending_preview_overrides_.clear();
    revealed_events_.clear();
    url_previews_.clear();
    url_preview_data_.clear();
    url_preview_in_flight_.clear();
    blurhash_attempted_.clear();
    tile_fetch_failed_.clear();
    media_fetch_failed_.clear();
    member_gender_cache_.clear();
    member_gender_inflight_.clear();
    pending_member_gender_requests_.clear();
    cancel_debounce_(DebounceSlot::MessageSearch);
    search_pending_queries_.clear();

    // Empty the visible room list; sliding sync repopulates it from scratch.
    rooms_.clear();
    per_account_rooms_.erase(my_user_id_);
    mark_room_index_dirty_();

    on_tab_state_changed_ui_();
    show_status_message_(tk::tr("Clearing cache…"), /*auto_clear_ms=*/0);

    // ── Phase B (mut_pool_): blocking SDK wipe + in-place re-restore ─────────

    auto sess = active_account_;
    run_async_mut_(
        [this, sess, session_json = std::move(loaded->session_json),
         store_key = std::move(loaded->store_key), empty_layout,
         recalc = std::move(recompute_callback)]() mutable
        {
            // Push the empty tab layout while the client is still synced.
            sess->client->save_prefs_json_blocking(empty_layout);

            // clear_caches() is self-contained (expires the sliding-sync
            // sessions, stops sync, closes + deletes the stores) — do NOT
            // stop_sync() first.
            sess->client->clear_caches();

            // The store key persists on the reused ClientFfi, but re-set it
            // defensively so a fresh encrypted store opens with the right key
            // (mirrors the startup restore path).
            if (!store_key.empty())
                sess->client->set_store_key(store_key);

            auto res = sess->client->restore_session(session_json);
            if (!res.ok)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                res = sess->client->restore_session(session_json); // retry once
            }
            const bool ok = res.ok;
            if (ok)
            {
                sess->client->start_sync(sess->bridge.get());
                // clear_caches() wiped search_index.db and reset the SDK's
                // indexing gate; re-apply the user's search-indexing
                // preference against the fresh store exactly as the startup
                // restore path does (restore_all_accounts_blocking_). This is
                // now a real off->on transition, so an enabled index re-runs
                // its one-time history backfill.
                apply_search_indexing_pref_(*sess->client);
            }

            // ── Phase C (UI thread): rebuild the UI ──────────────────────────
            post_to_ui_alive_(
                [this, sess, ok, err = res.message,
                 recalc = std::move(recalc)]() mutable
                {
                    sess->sync_started = ok;
                    if (ok)
                    {
                        show_status_message_(tk::tr("Cache cleared."));
                    }
                    else
                    {
                        show_status_message_(
                            tk::trf(tk::tr("Cache cleared, but reloading failed: "
                                           "{0}"),
                                    {err}),
                            /*auto_clear_ms=*/0);
                    }
                    refresh_account_ui_after_switch_();
                    if (recalc)
                        compute_cache_sizes_(std::move(recalc));
                });
        });
}

void ShellBase::set_initial_account(std::shared_ptr<AccountSession> account)
{
    active_account_ = std::move(account);
}

// True when this window's startup should reuse the already-restored,
// already-syncing accounts from the shared AccountManager instead of
// re-restoring from disk. A spawned (secondary) window finds the manager
// already populated, has a pinned active_account_ (via set_initial_account),
// and has not bound a client yet. The first (primary) window finds the
// manager empty; the primary re-login path runs with client_ already set.
// Platform startup entries (doLogin / do_login / start_login / beginLogin)
// check this first and, if true, bind the pinned account without restoring.
bool ShellBase::is_secondary_window_startup_() const
{
    return !account_manager_.accounts().empty() && active_account_ && !client_;
}

void ShellBase::on_account_picker_select_(const std::string& uid)
{
    if (auto* win = account_manager_.dedicated_window(uid))
    {
        // Already shown in some window: raise it (unless that window is us).
        if (win != this)
            win->raise_and_activate_();
        return;
    }
    if (is_ctrl_held_())
    {
        auto session = account_manager_.find(uid);
        if (session)
            spawn_main_window_(session);
        return;
    }
    if (is_pinned_window_)
    {
        // A popped-out window is bound to one account; open the chosen account in
        // its own window rather than hijacking this one.
        if (auto session = account_manager_.find(uid))
            spawn_main_window_(session);
        return;
    }
    switch_active_account_(uid);
}

// ---------------------------------------------------------------------------
// Sync-error handling (reconnect / soft-logout recovery / relogin)
// ---------------------------------------------------------------------------

void ShellBase::restart_account_sync_(const std::string& user_id)
{
    auto sess = account_manager_.find(user_id);
    if (sess && !sess->sync_started && sess->client)
    {
        sess->sync_started = true;
        run_async_mut_([sess]()
        {
            if (sess && sess->client)
                sess->client->start_sync(sess->bridge.get());
        });
    }
}

void ShellBase::schedule_sync_restart_(const std::string& user_id, int delay_ms)
{
    // The timer is native (post_to_ui_after_ → QTimer / g_timeout_add /
    // SetTimer / dispatch_after); the body is the shared restart helper.
    post_to_ui_after_(delay_ms,
                      [this, user_id]()
                      {
                          auto activity_scope =
                              activity_.begin("sync-restart", "Sync", "one-shot");
                          restart_account_sync_(user_id);
                      });
}

// Agnostic sync-error state machine, shared by every shell. Reacts to the
// SDK sync-error callback's three contexts:
//   - "sync_reconnect"   (transient): stop the affected account's sync and
//     schedule a delayed restart via schedule_sync_restart_().
//   - "sync_auth_error"  + soft_logout: restore the soft-logged-out session
//     (refresh-token flow), re-fetch display_name / avatar_url onto the
//     AccountSession, re-bind this window's identity strip when the affected
//     account is the active one, and restart sync. If the session can't be
//     restored (or this isn't a soft logout), clear the stored account, stop
//     sync, and ask the shell to relogin via request_relogin_().
//   - else: surface `description` in the status bar.
// Centralizing this fixes prior per-shell drift (notably macOS, which
// skipped the post-refresh display-name/avatar re-fetch + strip re-bind).
void ShellBase::handle_sync_error_impl_(std::string context,
                                        std::string user_id,
                                        std::string description,
                                        bool soft_logout)
{
    auto affected = account_manager_.find(user_id);

    if (context == "sync_reconnect")
    {
        show_status_message_(tk::tr("Sync error: reconnecting\xe2\x80\xa6"));
        if (affected && affected->client)
        {
            affected->client->stop_sync();
            affected->sync_started = false;
            schedule_sync_restart_(affected->user_id, 5000);
        }
        return;
    }

    if (context == "sync_auth_error")
    {
        if (soft_logout && affected && affected->client)
        {
            if (auto saved =
                    tesseract::SessionStore::load_account(affected->user_id))
            {
                show_status_message_(tk::tr("Reconnecting session\xe2\x80\xa6"));
                if (affected->client->restore_session(*saved))
                {
                    // Re-fetch identity onto the AccountSession. This is the
                    // piece macOS used to skip, leaving a stale display name.
                    affected->display_name =
                        affected->client->get_display_name();
                    affected->avatar_url = affected->client->get_avatar_url();
                    // Re-bind this window's identity strip when the affected
                    // account is the one currently shown here.
                    if (active_account_ && affected == active_account_)
                    {
                        my_user_id_ = affected->user_id;
                        my_display_name_ = affected->display_name;
                        my_avatar_url_ = affected->avatar_url;
                        refresh_user_strip_();
                    }
                    affected->sync_started = true;
                    affected->client->start_sync(affected->bridge.get());
                    show_status_message_(tk::tr("Reconnected"));
                    return;
                }
            }
        }
        // Unrecoverable: clear the stored account, stop sync, relogin.
        if (affected)
        {
            tesseract::SessionStore::clear_account(affected->user_id);
            if (affected->client)
            {
                affected->client->stop_sync();
            }
            affected->sync_started = false;
        }
        show_status_message_(tk::tr("Session expired; please log in again."));
        request_relogin_(user_id);
        return;
    }

    // Any other sync error: surface the description.
    show_status_message_(std::move(description));
}
} // namespace tesseract
