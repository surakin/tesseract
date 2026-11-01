#include "tesseract/secret_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>

namespace
{

// Build the Credential Manager target name: "Tesseract:<key>", where key is
// the backend key (SecretStore::key_for(user_id), the bare MXID in the default
// profile, or SecretStore::recovery_key_for(user_id)). Matrix IDs and profile
// names are ASCII-compatible; widen one byte at a time.
std::wstring make_target(const std::string& storage_key)
{
    std::wstring target = L"Tesseract:";
    for (unsigned char c : storage_key)
    {
        target.push_back(static_cast<wchar_t>(c));
    }
    return target;
}

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load_entry_(const std::string& storage_key, bool* failed)
{
    auto target = make_target(storage_key);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
    {
        if (failed && GetLastError() != ERROR_NOT_FOUND)
        {
            *failed = true;
        }
        return std::nullopt;
    }

    std::string result(reinterpret_cast<const char*>(cred->CredentialBlob),
                       static_cast<std::size_t>(cred->CredentialBlobSize));
    CredFree(cred);
    return result;
}

bool SecretStore::save_entry_(const std::string& storage_key, const std::string& json,
                              const char* /*label*/)
{
    auto target = make_target(storage_key);
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(json.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(json.data()));
    // CRED_PERSIST_LOCAL_MACHINE: persists across the user's logon sessions on
    // this machine but does NOT roam via domain/Azure AD profiles (that would
    // be CRED_PERSIST_ENTERPRISE).  Despite the "machine" in the flag name,
    // CRED_TYPE_GENERIC credentials are stored per-user: the blob is DPAPI-
    // encrypted under the user's profile and is inaccessible to other Windows
    // users on the same machine (and, because DPAPI's machine key is device-
    // specific, undecryptable on a different machine).  This is the intended
    // analog of the macOS kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
    // scope: persistent, device-local, and bound to the individual user.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) == TRUE;
}

void SecretStore::remove_entry_(const std::string& storage_key)
{
    auto target = make_target(storage_key);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

} // namespace tesseract
