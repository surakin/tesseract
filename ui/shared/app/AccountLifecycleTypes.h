#pragma once

// Result/IO value types for ShellBase's account lifecycle (startup restore,
// add-account login finalize, active-account logout). Split out of ShellBase.h
// unchanged; ShellBase re-exports each as a nested alias so the shells keep
// naming them ShellBase::RestoreResult etc.

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/emoji.h>
#include <tesseract/event_handler.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{

// Outcome of restore_all_accounts_(): lets the caller decide between the
// empty-accounts login fallback (show_login_after_restore_) and finishing
// login on the active account.
struct RestoreResult
{
    bool any_accounts = false;       // at least one account restored
    bool any_restore_failed = false; // ≥1 stored account failed restore
    std::string restore_error;       // last restore failure message
    // True when any_restore_failed is true *because* the cold-start
    // pre-flight tk::Host::is_network_available() check reported no OS-
    // level connectivity, rather than a real restore/auth/server error —
    // lets each shell pick LoginView::show_offline_error() over
    // show_restore_error() so raw backend detail isn't shown for a plain
    // "you're offline" case.
    bool network_unavailable = false;
    std::string active_uid; // uid to make active (empty when none)
};

// One restored account's blocking-I/O output, computed off the UI thread
// by restore_all_accounts_blocking_(). Touches no ShellBase state — only a
// freshly-restored Client, its native event bridge, and the plain data
// read from the client — so it's safe to build on mut_pool_'s worker
// thread (bridge construction and start_sync are both confirmed
// background-safe: bridges self-marshal callbacks to the UI thread via
// post_to_ui_, and start_sync's cost is a blocking Rust-side call, not a
// UI-toolkit one). `bridge` is declared before `client` to mirror
// AccountSession's destruction-order invariant (the bridge must outlive
// the client's tokio runtime teardown). The genuinely UI-thread-affine
// remainder (notifier install, pref application, AccountManager mutation)
// happens afterwards, on the UI thread, in finish_restore_accounts_ui_().
struct RestoredAccountIO
{
    std::unique_ptr<IEventHandler> bridge;
    std::string user_id;
    std::unique_ptr<Client> client; // set_data_dir()'d + restore_session()'d
    std::string display_name;
    std::string avatar_url;
    std::string last_room;
    std::vector<std::string> open_rooms;
    std::vector<std::string> recent_rooms;
    std::vector<std::string> bridge_not_bridged_overrides;
    tesseract::emoji::SkinTone emoji_skin_tone = tesseract::emoji::SkinTone::None;
    std::string prefs_json = "{}";
};

// Output of the blocking half of startup restore.
struct RestoreIOResult
{
    std::vector<RestoredAccountIO> accounts;
    bool any_restore_failed = false;
    std::string restore_error;
    // See RestoreResult::network_unavailable above — same meaning,
    // computed here and copied through by finish_restore_accounts_ui_().
    bool network_unavailable = false;
    std::string active_user_id_hint; // index.active_user_id (may name an
                                     // account that failed to restore)
};

// Outcome of finalize_login_async_(): lets each shell run the native finish
// (or the native duplicate-reject UI) without re-deriving state. On a
// successful add,
// `ok` is true and `user_id` names the account that was added + made active;
// on a duplicate (already-signed-in) it is rejected with `rejected_duplicate`
// true and `user_id` set so the shell can show "Already signed in as <uid>";
// on any hard failure (empty user id, empty session, persist/restore error)
// `ok` is false and `error` carries a message (empty when the platform path
// had nothing to report).
struct FinalizeLoginResult
{
    bool ok = false;                 // account added + made active
    bool rejected_duplicate = false; // uid already signed in
    std::string user_id;             // the new (or duplicate) uid
    std::string error;               // failure detail (when !ok)
    // True when finalize_login_blocking_ found recovery/cross-signing not
    // set up (or incomplete on this device) and deliberately skipped
    // start_sync — the shell must show the encryption-setup overlay
    // (begin_gated_encryption_setup_if_needed_) instead of assuming sync
    // is already running. See ShellBase::release_pending_sync_gate_.
    bool needs_encryption_setup = false;
    // Which EncryptionSetupOverlay::Mode to open when needs_encryption_setup
    // is true: false = Fresh, true = Recover.
    bool encryption_setup_recover_mode = false;
};

// pending-* login data dirs whose client is retired but may still hold SQLite
// files open (Windows cannot delete open files). remove_all() runs once the
// clients are freed; the destructor does it too.
struct RetiredLoginDirs
{
    RetiredLoginDirs() = default;
    RetiredLoginDirs(const RetiredLoginDirs&) = delete;
    RetiredLoginDirs& operator=(const RetiredLoginDirs&) = delete;
    ~RetiredLoginDirs()
    {
        remove_all();
    }

    void add(std::filesystem::path dir)
    {
        if (!dir.empty())
        {
            dirs_.push_back(std::move(dir));
        }
    }
    void remove_all() noexcept
    {
        for (const auto& d : dirs_)
        {
            std::error_code ec;
            std::filesystem::remove_all(d, ec);
        }
        dirs_.clear();
    }
    bool empty() const
    {
        return dirs_.empty();
    }

private:
    std::vector<std::filesystem::path> dirs_;
};

struct FinalizeLoginIO
{
    FinalizeLoginResult result;
    std::unique_ptr<AccountSession> session; // null unless result.ok
};

// Outcome of logout_active_account_impl_(): lets each shell decide between the
// empty-accounts native login fallback and the (already-completed) switch to a
// surviving account. When `logged_out` is false the call was a no-op (no active
// account) and the shell must do nothing. When `has_remaining` is true the impl
// has ALREADY switched to `next_uid` (via switch_active_account_impl_ +
// refresh_account_ui_after_switch_), so the shell needs no native follow-up
// beyond its own status line; when false, no accounts remain and the shell must
// show its native login view.
struct LogoutResult
{
    bool logged_out = false;    // an account was actually signed out
    bool has_remaining = false; // another account exists + is now active
    std::string logged_out_uid; // the uid that was signed out
    std::string next_uid;       // the surviving uid switched to (if any)
    // No `ok` field: client_->logout() now runs on mut_pool_ (it can take
    // several seconds — see the call site's comment), so its result isn't
    // known by the time this function returns. A failure still surfaces
    // via show_status_message_ once the background call completes; no
    // caller across any of the four shells (or the tests) read this
    // field's old synchronous value, so dropping it is not a behavior
    // change for any of them.
};

} // namespace tesseract
