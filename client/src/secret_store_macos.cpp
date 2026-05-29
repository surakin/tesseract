#include "tesseract/secret_store.h"

#include <nlohmann/json.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <mutex>

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
static constexpr std::string_view kService    = "lan.westeros.tesseract";
static constexpr std::string_view kConsAcct   = "_sessions"; // fixed sentinel account key

static std::mutex g_lock;

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
// Consolidated-item helpers (must be called with g_lock held)
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> load_consolidated_map()
{
    auto blob = keychain_load(std::string(kConsAcct));
    if (!blob)
        return std::nullopt;
    try
    {
        auto j = nlohmann::json::parse(*blob);
        if (!j.is_object())
            return std::nullopt;
        return j;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }
}

bool save_consolidated_map(const nlohmann::json& map)
{
    return keychain_save(std::string(kConsAcct), map.dump());
}

} // namespace

namespace tesseract
{

// load() tries the consolidated item first; if the user is absent there it
// falls back to the old per-user item so sessions created before this change
// continue to work until they are saved again (which migrates them).
std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(g_lock);

    if (auto map = load_consolidated_map())
    {
        auto it = map->find(user_id);
        if (it != map->end() && it->is_string())
            return it->get<std::string>();
    }

    // Migration fallback: old per-user item.
    return keychain_load(user_id);
}

// save() always writes to the consolidated item. If an old per-user item
// exists for this account it is deleted, completing the one-shot migration.
bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    std::lock_guard<std::mutex> lock(g_lock);

    nlohmann::json map = nlohmann::json::object();
    if (auto m = load_consolidated_map())
        map = std::move(*m);

    map[user_id] = json;

    if (!save_consolidated_map(map))
        return false;

    keychain_remove(user_id); // remove old per-user item if present
    return true;
}

void SecretStore::remove(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(g_lock);

    if (auto map = load_consolidated_map())
    {
        map->erase(user_id);
        save_consolidated_map(*map);
    }

    keychain_remove(user_id); // remove old per-user item if present
}

} // namespace tesseract
