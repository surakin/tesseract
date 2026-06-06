#include "tesseract/secret_store.h"
#include <libsecret/secret.h>

namespace
{

const SecretSchema kSchema = {
    "im.gnomos.tesseract.session",
    SECRET_SCHEMA_NONE,
    {{"user-id", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
};

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    GError* err = nullptr;
    gchar* secret = secret_password_lookup_sync(
        &kSchema, nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
    {
        g_error_free(err);
        return std::nullopt;
    }
    if (!secret)
        return std::nullopt;

    std::string result(secret);
    secret_password_free(secret);
    return result;
}

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    GError* err = nullptr;
    gboolean ok = secret_password_store_sync(
        &kSchema,
        SECRET_COLLECTION_DEFAULT,
        "Tesseract session",
        json.c_str(),
        nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
    {
        g_error_free(err);
        return false;
    }
    return static_cast<bool>(ok);
}

void SecretStore::remove(const std::string& user_id)
{
    GError* err = nullptr;
    secret_password_clear_sync(
        &kSchema, nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
        g_error_free(err);
}

} // namespace tesseract
