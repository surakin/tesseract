#pragma once

#include <string>

namespace tk
{

struct DeviceListing
{
    std::string id;           // platform-specific device identifier
    std::string display_name; // human-readable label for the UI
};

} // namespace tk
