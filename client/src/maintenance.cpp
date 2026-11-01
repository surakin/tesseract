#include "tesseract/maintenance.h"

#include "tesseract/client.h"
#include "tesseract/paths.h"
#include "tesseract/secret_store.h"
#include "tesseract/session_store.h"
#include "tesseract/settings.h"

namespace tesseract
{

LogoutAllReport logout_all_accounts()
{
    LogoutAllReport report;

    // Same startup housekeeping restore_all_accounts_blocking_ does, so a
    // pre-multi-account install is found at its current location.
    SessionStore::migrate_legacy_layout();

    SessionStore::AccountIndex index = SessionStore::load_index();
    if (index.corrupt)
    {
        report.index_corrupt = true;
        return report;
    }

    // Recovery keys Tesseract made and the user hasn't saved yet are the only
    // copy: like a sign-out from the app, keep those (and their reminders).
    auto& settings = Settings::instance();
    settings.load_from_disk(config_dir());
    bool settings_changed = false;

    for (const auto& uid : index.user_ids)
    {
        LogoutOutcome outcome;
        outcome.user_id = uid;

        // Mirrors the per-account restore in
        // ShellBase::restore_all_accounts_blocking_, minus sync: the session
        // only needs to be live long enough to revoke it.
        if (auto loaded = SessionStore::load_account_with_key(uid))
        {
            Client client;
            client.set_data_dir(SessionStore::sdk_store_dir(uid).string());
            if (!loaded->store_key.empty())
            {
                client.set_store_key(loaded->store_key);
            }
            if (Result res = client.restore_session(loaded->session_json); !res)
            {
                outcome.error = res.message;
            }
            else if (Result out = client.logout(); !out)
            {
                outcome.error = out.message;
            }
            else
            {
                outcome.server_ok = true;
            }
        }
        else
        {
            outcome.error = "no stored session";
        }

        // Wipe locally regardless: the user asked for these accounts to be
        // gone from this machine even if the homeserver is unreachable.
        SessionStore::clear_account(uid);
        if (!settings.recovery_key_unsaved.count(uid))
        {
            SecretStore::remove_recovery_key(uid);
            settings_changed |= settings.save_key_reminder_dismissals.erase(uid) > 0;
            settings_changed |= settings.save_key_reminder_snoozed_until.erase(uid) > 0;
        }
        report.accounts.push_back(std::move(outcome));
    }
    if (settings_changed)
    {
        settings.save_to_disk(config_dir());
    }

    SessionStore::save_index({});
    return report;
}

} // namespace tesseract
