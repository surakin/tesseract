#include "views/launch_baseline.h"

#include <string>
#include <utility>

namespace tesseract
{

namespace
{

std::string g_launch_language;
Settings::ProxyMode g_launch_proxy_mode = Settings::ProxyMode::System;
std::string g_launch_proxy_url;

} // namespace

void set_launch_language(std::string language)
{
    g_launch_language = std::move(language);
}

void set_launch_proxy(Settings::ProxyMode mode, std::string url)
{
    g_launch_proxy_mode = mode;
    g_launch_proxy_url = std::move(url);
}

const std::string& launch_language()
{
    return g_launch_language;
}

bool proxy_changed_since_launch(Settings::ProxyMode mode, const std::string& url)
{
    if (mode != g_launch_proxy_mode)
    {
        return true;
    }
    return mode == Settings::ProxyMode::Manual && url != g_launch_proxy_url;
}

} // namespace tesseract
