#include "tk/status_icons.h"

#include "icons.h" // generated — tesseract_tk private include path

namespace tk
{

std::span<const std::uint8_t> low_power_icon_svg()
{
    return {kBatteryLowSvg, sizeof(kBatteryLowSvg)};
}

} // namespace tk
