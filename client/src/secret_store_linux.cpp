#include "tesseract/secret_store.h"
#include <libsecret/secret.h>

namespace
{

const SecretSchema kSchema = {
    "im.gnomos.tesseract.session",
    SECRET_SCHEMA_NONE,
    {{"user-id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load_entry_(const std::string& storage_key, bool* failed)
{
    GError* err = nullptr;
    gchar* secret = secret_password_lookup_sync(&kSchema, nullptr, &err, "user-id",
                                                storage_key.c_str(), nullptr);
    if (err)
    {
        g_error_free(err);
        if (failed)
        {
            *failed = true;
        }
        return std::nullopt;
    }
    if (!secret)
    {
        return std::nullopt;
    }

    std::string result(secret);
    secret_password_free(secret);
    return result;
}

bool SecretStore::save_entry_(const std::string& storage_key, const std::string& value,
                              const char* label)
{
    GError* err = nullptr;
    gboolean ok =
        secret_password_store_sync(&kSchema, SECRET_COLLECTION_DEFAULT, label, value.c_str(),
                                   nullptr, &err, "user-id", storage_key.c_str(), nullptr);
    if (err)
    {
        g_error_free(err);
        return false;
    }
    return static_cast<bool>(ok);
}

void SecretStore::remove_entry_(const std::string& storage_key)
{
    GError* err = nullptr;
    secret_password_clear_sync(&kSchema, nullptr, &err, "user-id", storage_key.c_str(), nullptr);
    if (err)
    {
        g_error_free(err);
    }
}

} // namespace tesseract
