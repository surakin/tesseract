#include "tesseract/secret_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>

namespace
{

// Build the Credential Manager target name: "Tesseract:<user_id>".
// Matrix IDs are ASCII-compatible; widen one byte at a time.
std::wstring make_target(const std::string& user_id)
{
    std::wstring target = L"Tesseract:";
    for (unsigned char c : user_id)
        target.push_back(static_cast<wchar_t>(c));
    return target;
}

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    auto target = make_target(user_id);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return std::nullopt;

    std::string result(
        reinterpret_cast<const char*>(cred->CredentialBlob),
        static_cast<std::size_t>(cred->CredentialBlobSize));
    CredFree(cred);
    return result;
}

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    auto target = make_target(user_id);
    CREDENTIALW cred        = {};
    cred.Type               = CRED_TYPE_GENERIC;
    cred.TargetName         = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(json.size());
    cred.CredentialBlob     =
        reinterpret_cast<LPBYTE>(const_cast<char*>(json.data()));
    cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) == TRUE;
}

void SecretStore::remove(const std::string& user_id)
{
    auto target = make_target(user_id);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

} // namespace tesseract
