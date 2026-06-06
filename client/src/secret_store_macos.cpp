#include "tesseract/secret_store.h"

#include <nlohmann/json.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <mutex>
#include <optional>

namespace
{

// Thin RAII wrapper for CF objects that arrive with a +1 retain count.
template <typename T>
struct CFOwned
{
    T ref = nullptr;
    explicit CFOwned(T r) : ref(r) {}
    ~CFOwned() { if (ref) CFRelease(ref); }
    CFOwned(const CFOwned&) = delete;
    CFOwned& operator=(const CFOwned&) = delete;
    operator T() const { return ref; }
};

CFOwned<CFStringRef> cf_string(const std::string& s)
{
    return CFOwned<CFStringRef>(CFStringCreateWithBytes(
        nullptr,
        reinterpret_cast<const UInt8*>(s.data()),
        static_cast<CFIndex>(s.size()),
        kCFStringEncodingUTF8,
        false));
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

// ---------------------------------------------------------------------------
// Raw Keychain helpers
//
// Items are written to the data-protection keychain (kSecUseDataProtectionKeychain,
// available since macOS 10.15).  Unlike the traditional login keychain, the
// data-protection keychain does not use ACL-based authorization dialogs: the
// creating app always has silent access to its own items regardless of
// code-signing identity changes between builds.  This eliminates the two-prompt
// pattern seen with the traditional keychain (separate "confidential information"
// and "access key" dialogs for read vs. write operations).
//
// The traditional keychain is kept as a read-only migration source so that
// sessions persisted by older builds survive the first post-upgrade launch.
// ---------------------------------------------------------------------------

// Query only the data-protection keychain.  Never triggers a user dialog.
std::optional<std::string> keychain_load_dp(const std::string& account_key)
{
    auto account = cf_string(account_key);
    const void* keys[] = {
        kSecClass, kSecAttrService, kSecAttrAccount,
        kSecReturnData, kSecMatchLimit, kSecUseDataProtectionKeychain};
    const void* vals[] = {
        kSecClassGenericPassword,
        CFSTR("im.gnomos.tesseract"),
        account.ref,
        kCFBooleanTrue,
        kSecMatchLimitOne,
        kCFBooleanTrue};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 6,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));

    CFTypeRef raw = nullptr;
    OSStatus status = SecItemCopyMatching(query, &raw);
    if (status != errSecSuccess || !raw)
        return std::nullopt;

    CFOwned<CFDataRef> data(static_cast<CFDataRef>(raw));
    return std::string(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
        static_cast<std::size_t>(CFDataGetLength(data)));
}

// Query only the traditional login keychain.  May trigger an authorization
// dialog on first access by a new binary — used only for migration.
std::optional<std::string> keychain_load_traditional(const std::string& account_key)
{
    auto account = cf_string(account_key);
    const void* keys[] = {
        kSecClass, kSecAttrService, kSecAttrAccount,
        kSecReturnData, kSecMatchLimit};
    const void* vals[] = {
        kSecClassGenericPassword,
        CFSTR("im.gnomos.tesseract"),
        account.ref,
        kCFBooleanTrue,
        kSecMatchLimitOne};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 5,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));

    CFTypeRef raw = nullptr;
    OSStatus status = SecItemCopyMatching(query, &raw);
    if (status != errSecSuccess || !raw)
        return std::nullopt;

    CFOwned<CFDataRef> data(static_cast<CFDataRef>(raw));
    return std::string(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
        static_cast<std::size_t>(CFDataGetLength(data)));
}

// Try data-protection keychain first; fall back to traditional for migration.
std::optional<std::string> keychain_load(const std::string& account_key)
{
    if (auto blob = keychain_load_dp(account_key))
        return blob;
    return keychain_load_traditional(account_key);
}

// Write to the data-protection keychain only.  kSecAttrAccessibleAfterFirstUnlock-
// ThisDeviceOnly makes the item accessible for background sync after the first
// boot-unlock without syncing it to iCloud Keychain.
bool keychain_save(const std::string& account_key, const std::string& blob)
{
    auto account = cf_string(account_key);
    CFOwned<CFDataRef> value(CFDataCreate(
        nullptr,
        reinterpret_cast<const UInt8*>(blob.data()),
        static_cast<CFIndex>(blob.size())));

    // Attempt update in the data-protection keychain first.
    {
        const void* qk[] = {
            kSecClass, kSecAttrService, kSecAttrAccount,
            kSecUseDataProtectionKeychain};
        const void* qv[] = {
            kSecClassGenericPassword,
            CFSTR("im.gnomos.tesseract"),
            account.ref,
            kCFBooleanTrue};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
            nullptr, qk, qv, 4,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        const void* uk[] = {kSecValueData};
        const void* uv[] = {value.ref};
        CFOwned<CFDictionaryRef> update(CFDictionaryCreate(
            nullptr, uk, uv, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        OSStatus st = SecItemUpdate(query, update);
        if (st == errSecSuccess)
            return true;
        if (st != errSecItemNotFound)
            return false;
    }

    // Item not found in the data-protection keychain — add it.
    const void* ak[] = {
        kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData,
        kSecUseDataProtectionKeychain, kSecAttrAccessible};
    const void* av[] = {
        kSecClassGenericPassword,
        CFSTR("im.gnomos.tesseract"),
        account.ref,
        value.ref,
        kCFBooleanTrue,
        kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    CFOwned<CFDictionaryRef> add(CFDictionaryCreate(
        nullptr, ak, av, 6,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    return SecItemAdd(add, nullptr) == errSecSuccess;
}

// Delete from the data-protection keychain.  Also attempts to delete from the
// traditional keychain with kSecUseAuthenticationUISkip so that lingering
// pre-migration items are cleaned up silently (errSecInteractionNotAllowed is
// acceptable — the orphaned item causes no functional harm).
void keychain_remove(const std::string& account_key)
{
    auto account = cf_string(account_key);

    // Data-protection keychain — app always has silent access to its own items.
    {
        const void* keys[] = {
            kSecClass, kSecAttrService, kSecAttrAccount,
            kSecUseDataProtectionKeychain};
        const void* vals[] = {
            kSecClassGenericPassword,
            CFSTR("im.gnomos.tesseract"),
            account.ref,
            kCFBooleanTrue};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
            nullptr, keys, vals, 4,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        SecItemDelete(query);
    }

    // Traditional keychain — skip UI to avoid prompts for pre-migration items.
    {
        const void* keys[] = {
            kSecClass, kSecAttrService, kSecAttrAccount,
            kSecUseAuthenticationUI};
        const void* vals[] = {
            kSecClassGenericPassword,
            CFSTR("im.gnomos.tesseract"),
            account.ref,
            kSecUseAuthenticationUISkip};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
            nullptr, keys, vals, 4,
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
    return keychain_save(std::string(kConsAcct), g_map->dump());
}

// Ensures g_map is populated.  Reads from the data-protection keychain first
// (no dialog).  If not found, falls back to the traditional keychain for a
// one-time migration: the item is immediately re-saved to the data-protection
// keychain and silently deleted from the traditional one so future launches
// need no dialog at all.
void ensure_map_loaded()
{
    if (g_map.has_value())
        return;

    // Fast path: data-protection keychain — no authorization dialog.
    if (auto blob = keychain_load_dp(std::string(kConsAcct)))
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
        catch (const nlohmann::json::exception&) {}
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
                if (flush_map())
                {
                    // Delete the stale traditional item.  Use kSecUseAuthenticationUISkip
                    // so a failed delete (e.g. ACL mismatch) is silent — the orphan is
                    // harmless since future launches find the DP item first.
                    // Do NOT call keychain_remove() here: that would also delete the DP
                    // item we just created via flush_map().
                    auto acct = cf_string(std::string(kConsAcct));
                    const void* dk[] = {
                        kSecClass, kSecAttrService, kSecAttrAccount,
                        kSecUseAuthenticationUI};
                    const void* dv[] = {
                        kSecClassGenericPassword,
                        CFSTR("im.gnomos.tesseract"),
                        acct.ref,
                        kSecUseAuthenticationUISkip};
                    CFOwned<CFDictionaryRef> del(CFDictionaryCreate(
                        nullptr, dk, dv, 4,
                        &kCFTypeDictionaryKeyCallBacks,
                        &kCFTypeDictionaryValueCallBacks));
                    SecItemDelete(del);
                }
                return;
            }
        }
        catch (const nlohmann::json::exception&) {}
    }

    g_map = nlohmann::json::object(); // consolidated item absent or unreadable
}

} // namespace

namespace tesseract
{

// load() populates g_map from the Keychain on the first call (one
// SecItemCopyMatching at most), then serves subsequent callers from the cache.
// If the user is not in the consolidated item, falls back to the old
// per-user format so sessions survive the first post-upgrade launch.
std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(g_lock);

    ensure_map_loaded();

    auto it = g_map->find(user_id);
    if (it != g_map->end() && it->is_string())
        return it->get<std::string>();

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
            keychain_remove(user_id);
    }
    return blob;
}

// save() updates the in-memory map and flushes to the Keychain with
// SecItemUpdate/SecItemAdd — never SecItemCopyMatching — so token-refresh
// calls that arrive at runtime cannot trigger Keychain access dialogs.
bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    std::lock_guard<std::mutex> lock(g_lock);

    // Defensive: if save() somehow races ahead of the first load() (not
    // expected in normal startup flow), read once rather than clobber other
    // users' sessions with an empty map.
    ensure_map_loaded();

    (*g_map)[user_id] = json;

    if (!flush_map())
        return false;

    keychain_remove(user_id); // remove old per-user item if present
    return true;
}

void SecretStore::remove(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(g_lock);

    ensure_map_loaded();

    g_map->erase(user_id);
    flush_map();

    keychain_remove(user_id); // remove old per-user item if present
}

} // namespace tesseract
