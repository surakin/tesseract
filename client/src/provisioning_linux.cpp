#include "tesseract/provisioning.h"

#include <fstream>
#include <iterator>

namespace tesseract::detail
{

ProvisionedValues read_platform_provisioning_raw()
{
    std::ifstream in("/etc/tesseract/provisioning.json");
    if (!in)
        return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_provisioning_values(text);
}

} // namespace tesseract::detail
