#include "tesseract/secret_store.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

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

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    auto account = cf_string(user_id);
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

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    auto account = cf_string(user_id);
    CFOwned<CFDataRef> value(CFDataCreate(
        nullptr,
        reinterpret_cast<const UInt8*>(json.data()),
        static_cast<CFIndex>(json.size())));

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

void SecretStore::remove(const std::string& user_id)
{
    auto account = cf_string(user_id);
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

} // namespace tesseract
