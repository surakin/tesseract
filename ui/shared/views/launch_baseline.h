#pragma once

#include "tesseract/settings.h"

#include <string>

namespace tesseract
{

/// Records the language this process applied at startup. Called once by
/// prepare_launch().
void set_launch_language(std::string language);

/// Records the proxy this process applied at startup. Called once by
/// prepare_launch().
void set_launch_proxy(Settings::ProxyMode mode, std::string url);

/// The Settings::language value this process applied at startup ("auto",
/// "en", ...). Changing the setting takes effect only after a restart, so
/// the UI compares against this to know whether one is pending.
const std::string& launch_language();

/// True if the given proxy setting differs from the one this process applied
/// at startup (the proxy only takes effect after a restart). The URL is
/// compared only in Manual mode.
bool proxy_changed_since_launch(Settings::ProxyMode mode, const std::string& url);

} // namespace tesseract
