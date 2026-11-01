#include "tesseract/secret_store.h"

namespace tesseract
{

std::optional<std::string> SecretStore::load_entry_(const std::string&, bool* failed)
{
    if (failed)
    {
        *failed = true; // no secure storage to read
    }
    return std::nullopt;
}

bool SecretStore::save_entry_(const std::string&, const std::string&, const char*)
{
    return false;
}

void SecretStore::remove_entry_(const std::string&)
{
}

} // namespace tesseract
