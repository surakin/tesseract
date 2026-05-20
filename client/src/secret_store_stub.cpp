#include "tesseract/secret_store.h"

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string&)
{
    return std::nullopt;
}

bool SecretStore::save(const std::string&, const std::string&)
{
    return false;
}

void SecretStore::remove(const std::string&)
{
}

} // namespace tesseract
