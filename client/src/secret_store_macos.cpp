#include "tesseract/secret_store.h"

#include <nlohmann/json.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstdio>
#include <mutex>
#include <optional>

namespace
{

// Thin RAII wrapper for CF objects that arrive with a +1 retain count.
template <typename T>
struct CFOwned
{
    T ref = nullptr;
    explicit CFOwned(T r) : ref(r)
    {
    }
    ~CFOwned()
    {
        if (ref)
        {
            CFRelease(ref);
        }
    }
    CFOwned(const CFOwned&) = delete;
    CFOwned& operator=(const CFOwned&) = delete;
    operator T() const
    {
        return ref;
    }
};

CFOwned<CFStringRef> cf_string(const std::string& s)
{
    return CFOwned<CFStringRef>(
        CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false));
}

// All sessions are stored in a single Keychain item so that the OS shows at
// most one access dialog regardless of how many accounts are logged in.
// Old per-user items (kSecAttrAccount = MXID) are kept as a migration fallback
// and removed the first time their owner calls save().
static constexpr std::string_view kConsAcct = "_sessions"; // fixed sentinel; MXIDs start with '@'

// g_lock serialises access to g_map and the Keychain operations below.
static std::mutex g_lock;

// Process-lifetime mirror of the consolidated Keychain item.
// nullopt = not yet populated from Keychain (populated lazily on first load or save).
// Once set, save() and remove() update it directly without calling
// SecItemCopyMatching, avoiding unexpected Keychain prompts during token
// refreshes that happen at runtime.
static std::optional<nlohmann::json> g_map;

// Set when the consolidated item couldn't be read (an error other than "not
// found"), so a missing entry can be reported as unreadable rather than absent.
static bool g_map_unreadable = false;

// Set once any data-protection keychain call fails with errSecMissingEntitlement
// (-34018): this process cannot use that keychain, so the traditional login
// keychain is the primary store. Happens for unentitled builds (ad-hoc signed,
// and Developer ID without a provisioning profile carrying a
// keychain-access-groups / application-identifier entitlement). Guarded by g_lock.
static bool g_dp_unavailable = false;

// Records that the data-protection keychain is unusable (logs once per process).
void note_dp_unavailable()
{
    if (!g_dp_unavailable)
    {
        g_dp_unavailable = true;
        std::fprintf(stderr, "[tesseract] data-protection keychain unavailable (missing "
                             "entitlement); sessions are stored in the login keychain.\n");
    }
}

// ---------------------------------------------------------------------------
// Raw Keychain helpers
//
// Entitled builds write to the data-protection keychain
// (kSecUseDataProtectionKeychain, macOS 10.15+).  It has no ACL-based
// authorization dialogs: the creating app always has silent access to its own
// items regardless of code-signing identity changes between builds.  The
// traditional login keychain is then only a read-only migration source for
// sessions persisted by older builds.
//
// Unentitled builds get errSecMissingEntitlement from every data-protection
// call.  Then g_dp_unavailable is set and the traditional login keychain is the
// primary store (the consolidated item is neither migrated nor deleted).  Trade-
// off: an item's access list is tied to the code signature, so an ad-hoc-signed
// development build can see a Keychain access prompt after a rebuild; a
// Developer ID build keeps access across updates.
// ---------------------------------------------------------------------------

// Query only the data-protection keychain.  Never triggers a user dialog.
// `status_out` (optional) receives the SecItemCopyMatching status.
std::optional<std::string> keychain_load_dp(const std::string& account_key,
                                            OSStatus* status_out = nullptr)
{
    auto account = cf_string(account_key);
    const void* keys[] = {kSecClass,      kSecAttrService, kSecAttrAccount,
                          kSecReturnData, kSecMatchLimit,  kSecUseDataProtectionKeychain};
    const void* vals[] = {kSecClassGenericPassword,
                          CFSTR("im.gnomos.tesseract"),
                          account.ref,
                          kCFBooleanTrue,
                          kSecMatchLimitOne,
                          kCFBooleanTrue};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFTypeRef raw = nullptr;
    OSStatus status = SecItemCopyMatching(query, &raw);
    if (status_out)
    {
        *status_out = status;
    }
    if (status != errSecSuccess || !raw)
    {
        return std::nullopt;
    }

    CFOwned<CFDataRef> data(static_cast<CFDataRef>(raw));
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                       static_cast<std::size_t>(CFDataGetLength(data)));
}

// Query only the traditional login keychain.  May trigger an authorization
// dialog on first access by a new binary.  Migration source, or the primary
// store when g_dp_unavailable.
std::optional<std::string> keychain_load_traditional(const std::string& account_key,
                                                     OSStatus* status_out = nullptr)
{
    auto account = cf_string(account_key);
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                          kSecMatchLimit};
    const void* vals[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account.ref,
                          kCFBooleanTrue, kSecMatchLimitOne};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFTypeRef raw = nullptr;
    OSStatus status = SecItemCopyMatching(query, &raw);
    if (status_out)
    {
        *status_out = status;
    }
    if (status != errSecSuccess || !raw)
    {
        return std::nullopt;
    }

    CFOwned<CFDataRef> data(static_cast<CFDataRef>(raw));
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                       static_cast<std::size_t>(CFDataGetLength(data)));
}

// Try data-protection keychain first; fall back to traditional for migration.
std::optional<std::string> keychain_load(const std::string& account_key)
{
    if (!g_dp_unavailable)
    {
        OSStatus st = errSecSuccess;
        if (auto blob = keychain_load_dp(account_key, &st))
        {
            return blob;
        }
        if (st == errSecMissingEntitlement)
        {
            note_dp_unavailable();
        }
    }
    return keychain_load_traditional(account_key);
}

// Update-or-add in the data-protection keychain.  kSecAttrAccessibleAfterFirstUnlock-
// ThisDeviceOnly makes the item accessible for background sync after the first
// boot-unlock without syncing it to iCloud Keychain.  Returns the OSStatus.
OSStatus keychain_save_dp(CFStringRef account, CFDataRef value)
{
    const void* qk[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecUseDataProtectionKeychain};
    const void* qv[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account,
                        kCFBooleanTrue};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, qk, qv, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const void* uk[] = {kSecValueData};
    const void* uv[] = {value};
    CFOwned<CFDictionaryRef> update(CFDictionaryCreate(
        nullptr, uk, uv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    OSStatus st = SecItemUpdate(query, update);
    if (st != errSecItemNotFound)
    {
        return st;
    }

    // Item not found in the data-protection keychain — add it.
    const void* ak[] = {kSecClass,
                        kSecAttrService,
                        kSecAttrAccount,
                        kSecValueData,
                        kSecUseDataProtectionKeychain,
                        kSecAttrAccessible};
    const void* av[] = {kSecClassGenericPassword,
                        CFSTR("im.gnomos.tesseract"),
                        account,
                        value,
                        kCFBooleanTrue,
                        kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    CFOwned<CFDictionaryRef> add(CFDictionaryCreate(
        nullptr, ak, av, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    return SecItemAdd(add, nullptr);
}

// Update-or-add in the traditional login keychain.  kSecAttrAccessible applies
// only to the data-protection keychain, so it is not set here.
bool keychain_save_traditional(CFStringRef account, CFDataRef value)
{
    const void* qk[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* qv[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, qk, qv, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const void* uk[] = {kSecValueData};
    const void* uv[] = {value};
    CFOwned<CFDictionaryRef> update(CFDictionaryCreate(
        nullptr, uk, uv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    OSStatus st = SecItemUpdate(query, update);
    if (st == errSecSuccess)
    {
        return true;
    }
    if (st != errSecItemNotFound)
    {
        return false;
    }

    const void* ak[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* av[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account, value};
    CFOwned<CFDictionaryRef> add(CFDictionaryCreate(
        nullptr, ak, av, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    return SecItemAdd(add, nullptr) == errSecSuccess;
}

// Write to the data-protection keychain, or to the traditional one when this
// process cannot use the former (g_dp_unavailable, set here on the first
// errSecMissingEntitlement, in which case the write is retried once).
bool keychain_save(const std::string& account_key, const std::string& blob)
{
    auto account = cf_string(account_key);
    CFOwned<CFDataRef> value(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(blob.data()),
                                          static_cast<CFIndex>(blob.size())));

    if (!g_dp_unavailable)
    {
        OSStatus st = keychain_save_dp(account, value);
        if (st == errSecSuccess)
        {
            return true;
        }
        if (st != errSecMissingEntitlement)
        {
            return false;
        }
        note_dp_unavailable();
    }
    return keychain_save_traditional(account, value);
}

// Delete from the data-protection keychain.  Also deletes from the traditional
// keychain (the primary store when g_dp_unavailable, otherwise lingering
// pre-migration items) with kSecUseAuthenticationUISkip so it is silent
// (errSecInteractionNotAllowed is acceptable — an orphaned item causes no
// functional harm).  Only ever called with a per-user key, never "_sessions".
void keychain_remove(const std::string& account_key)
{
    auto account = cf_string(account_key);

    // Data-protection keychain — app always has silent access to its own items.
    {
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                              kSecUseDataProtectionKeychain};
        const void* vals[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account.ref,
                              kCFBooleanTrue};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(nullptr, keys, vals, 4,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks));
        SecItemDelete(query);
    }

    // Traditional keychain — skip UI to avoid prompts for pre-migration items.
    {
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecUseAuthenticationUI};
        const void* vals[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"), account.ref,
                              kSecUseAuthenticationUISkip};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(nullptr, keys, vals, 4,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks));
        SecItemDelete(query);
    }
}

// ---------------------------------------------------------------------------
// g_map helpers (g_lock must be held by the caller)
// ---------------------------------------------------------------------------

bool flush_map()
{
    // The consolidated item could not be read, so the in-memory map holds only
    // what this run has added. Writing it would replace every other account's
    // stored session with that partial map.
    if (g_map_unreadable)
    {
        return false;
    }
    return keychain_save(std::string(kConsAcct), g_map->dump());
}

// Ensures g_map is populated.  Reads from the data-protection keychain first
// (no dialog).  If not found, falls back to the traditional keychain for a
// one-time migration: the item is immediately re-saved to the data-protection
// keychain and silently deleted from the traditional one so future launches
// need no dialog at all.  When the data-protection keychain is unavailable
// (missing entitlement) the traditional item is the primary store instead and
// is read in place, never migrated or deleted.
void ensure_map_loaded()
{
    if (g_map.has_value())
    {
        return;
    }

    // Fast path: data-protection keychain — no authorization dialog.
    OSStatus dp_status = errSecSuccess;
    if (!g_dp_unavailable)
    {
        if (auto blob = keychain_load_dp(std::string(kConsAcct), &dp_status))
        {
            try
            {
                auto j = nlohmann::json::parse(*blob);
                if (j.is_object())
                {
                    g_map = std::move(j);
                    return;
                }
            }
            catch (const nlohmann::json::exception&)
            {
            }
        }
        if (dp_status == errSecMissingEntitlement)
        {
            note_dp_unavailable();
        }
    }

    // Unentitled: the traditional keychain is the primary store.  Keep the item
    // where it is.  A read error leaves the map unreadable so writes are refused;
    // an item that reads fine but is corrupt holds nothing recoverable and is replaced.
    if (g_dp_unavailable)
    {
        OSStatus trad_status = errSecSuccess;
        bool corrupt = false;
        if (auto blob = keychain_load_traditional(std::string(kConsAcct), &trad_status))
        {
            try
            {
                auto j = nlohmann::json::parse(*blob);
                if (j.is_object())
                {
                    g_map = std::move(j);
                    return;
                }
            }
            catch (const nlohmann::json::exception&)
            {
            }
            corrupt = true;
        }
        g_map_unreadable = trad_status != errSecSuccess && trad_status != errSecItemNotFound;
        if (corrupt)
        {
            std::fprintf(stderr,
                         "[tesseract] stored session list in the keychain is corrupt and will be "
                         "replaced\n");
        }
        g_map = nlohmann::json::object();
        return;
    }

    // Migration path: traditional keychain (at most one dialog on first run
    // after upgrade).  Immediately flush to the data-protection keychain and
    // silently delete the traditional item so future launches take the fast
    // path above.
    if (auto blob = keychain_load_traditional(std::string(kConsAcct)))
    {
        try
        {
            auto j = nlohmann::json::parse(*blob);
            if (j.is_object())
            {
                g_map = std::move(j);
                // A flush that fell back to the traditional keychain (g_dp_unavailable)
                // wrote the only copy there; deleting it would destroy the sessions.
                if (flush_map() && !g_dp_unavailable)
                {
                    // Delete the stale traditional item.  Use kSecUseAuthenticationUISkip
                    // so a failed delete (e.g. ACL mismatch) is silent — the orphan is
                    // harmless since future launches find the DP item first.
                    // Do NOT call keychain_remove() here: that would also delete the DP
                    // item we just created via flush_map().
                    auto acct = cf_string(std::string(kConsAcct));
                    const void* dk[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                                        kSecUseAuthenticationUI};
                    const void* dv[] = {kSecClassGenericPassword, CFSTR("im.gnomos.tesseract"),
                                        acct.ref, kSecUseAuthenticationUISkip};
                    CFOwned<CFDictionaryRef> del(
                        CFDictionaryCreate(nullptr, dk, dv, 4, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks));
                    SecItemDelete(del);
                }
                return;
            }
        }
        catch (const nlohmann::json::exception&)
        {
        }
    }

    g_map_unreadable = dp_status != errSecSuccess && dp_status != errSecItemNotFound;
    g_map = nlohmann::json::object(); // consolidated item absent or unreadable
}

} // namespace

namespace tesseract
{

// Every entry point receives the final backend key — SecretStore::key_for(
// user_id) (the bare MXID in the default profile, profile-scoped otherwise) or
// SecretStore::recovery_key_for(user_id) — for both the consolidated map and
// the legacy per-user items.

// load() populates g_map from the Keychain on the first call (one
// SecItemCopyMatching at most), then serves subsequent callers from the cache.
// If the user is not in the consolidated item, falls back to the old
// per-user format so sessions survive the first post-upgrade launch.
std::optional<std::string> SecretStore::load_entry_(const std::string& user_id, bool* failed)
{
    std::lock_guard<std::mutex> lock(g_lock);

    ensure_map_loaded();

    auto it = g_map->find(user_id);
    if (it != g_map->end() && it->is_string())
    {
        return it->get<std::string>();
    }

    // Migration fallback: old per-user item. Migrate eagerly into the
    // consolidated item here so the next startup reads only "_sessions" and
    // sees at most one Keychain access dialog regardless of token-refresh
    // cadence (previously migration only ran inside save(), so accounts that
    // never refreshed their token kept prompting twice on every launch).
    auto blob = keychain_load(user_id);
    if (blob)
    {
        (*g_map)[user_id] = *blob;
        if (flush_map())
        {
            keychain_remove(user_id);
        }
    }
    if (!blob && failed && g_map_unreadable)
    {
        *failed = true;
    }
    return blob;
}

// save() updates the in-memory map and flushes to the Keychain with
// SecItemUpdate/SecItemAdd — never SecItemCopyMatching — so token-refresh
// calls that arrive at runtime cannot trigger Keychain access dialogs.
bool SecretStore::save_entry_(const std::string& user_id, const std::string& json,
                              const char* /*label*/)
{
    std::lock_guard<std::mutex> lock(g_lock);

    // Defensive: if save() somehow races ahead of the first load() (not
    // expected in normal startup flow), read once rather than clobber other
    // users' sessions with an empty map.
    ensure_map_loaded();

    // Roll the in-memory map back if the flush fails, so a later load() can't
    // return a secret that was never persisted.
    std::optional<nlohmann::json> previous;
    if (auto it = g_map->find(user_id); it != g_map->end())
    {
        previous = *it;
    }

    (*g_map)[user_id] = json;

    if (!flush_map())
    {
        if (previous)
        {
            (*g_map)[user_id] = *previous;
        }
        else
        {
            g_map->erase(user_id);
        }
        return false;
    }

    keychain_remove(user_id); // remove old per-user item if present
    return true;
}

void SecretStore::remove_entry_(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(g_lock);

    ensure_map_loaded();

    // Put the entry back if the flush fails: the persisted item still holds
    // it, so load() must keep returning it (save_account relies on that to
    // detect a remove that didn't take).
    auto it = g_map->find(user_id);
    if (it == g_map->end())
    {
        keychain_remove(user_id); // remove old per-user item if present
        return;
    }
    nlohmann::json previous = std::move(*it);
    g_map->erase(it);

    if (!flush_map())
    {
        (*g_map)[user_id] = std::move(previous);
    }

    keychain_remove(user_id); // remove old per-user item if present
}

} // namespace tesseract
