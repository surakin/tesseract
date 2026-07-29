#pragma once
#include <tesseract/autostart.h>

namespace mac
{

// IAutostart impl backed by SMAppService.mainApp (macOS 13+). The app's
// CMAKE_OSX_DEPLOYMENT_TARGET is 11.0, below SMAppService's minimum, so
// both methods fail closed (is_enabled() -> false, set_enabled() -> false)
// on macOS 11/12 — "Launch at Login" is simply unavailable there rather
// than requiring a separate SMLoginItemSetEnabled + helper-bundle path.
class MacAutostart final : public tesseract::IAutostart
{
public:
    bool is_enabled() const override;
    bool set_enabled(bool enabled) override;
};

} // namespace mac
