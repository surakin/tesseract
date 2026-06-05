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
// Raw Keychain helpers (parameterised on account key)
// ---------------------------------------------------------------------------

std::optional<std::string> keychain_load(const std::string& account_key)
{
    auto account = cf_string(account_key);
    const void* keys[] = {
        kSecClass, kSecAttrService, kSecAttrAccount,
        kSecReturnData, kSecMatchLimit};
    const void* vals[] = {
        kSecClassGenericPassword,
        CFSTR("lan.westeros.tesseract"),
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

bool keychain_save(const std::string& account_key, const std::string& blob)
{
    auto account = cf_string(account_key);
    CFOwned<CFDataRef> value(CFDataCreate(
        nullptr,
        reinterpret_cast<const UInt8*>(blob.data()),
        static_cast<CFIndex>(blob.size())));

    // Attempt update first; fall through to add if the item does not exist.
    {
        const void* qk[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void* qv[] = {
            kSecClassGenericPassword,
            CFSTR("lan.westeros.tesseract"),
            account.ref};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
            nullptr, qk, qv, 3,
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

    // Item not found — add it.
    const void* ak[] = {
        kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* av[] = {
        kSecClassGenericPassword,
        CFSTR("lan.westeros.tesseract"),
        account.ref,
        value.ref};
    CFOwned<CFDictionaryRef> add(CFDictionaryCreate(
        nullptr, ak, av, 4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    return SecItemAdd(add, nullptr) == errSecSuccess;
}

void keychain_remove(const std::string& account_key)
{
    auto account = cf_string(account_key);
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* vals[] = {
        kSecClassGenericPassword,
        CFSTR("lan.westeros.tesseract"),
        account.ref};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    SecItemDelete(query);
}

// ---------------------------------------------------------------------------
// g_map helpers (g_lock must be held by the caller)
// ---------------------------------------------------------------------------

// Ensures g_map is populated. Calls SecItemCopyMatching at most once per
// process lifetime (on the first load() or save() that needs it). All
// subsequent operations use the in-memory cache, so token-refresh saves never
// trigger Keychain prompts at runtime.
void ensure_map_loaded()
{
    if (g_map.has_value())
        return;

    if (auto blob = keychain_load(std::string(kConsAcct)))
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
    g_map = nlohmann::json::object(); // consolidated item absent or unreadable
}

bool flush_map()
{
    return keychain_save(std::string(kConsAcct), g_map->dump());
}

} // namespace

namespace tesseract
{

// load() populates g_map from the Keychain on the first call (one
// SecItemCopyMatching), then serves subsequent callers from the cache.
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
