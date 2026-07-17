#include "tesseract/maps_link.h"

#include "tesseract_sdk_bridge_cxx/bridge.h"

namespace tesseract
{

MapsLinkClassification classify_maps_link(const std::string& trimmed_text)
{
    if (trimmed_text.empty())
        return {};

    auto r = tesseract_ffi::classify_maps_link(trimmed_text);
    MapsLinkClassification out;
    out.matched = r.matched;
    out.needs_resolve = r.needs_resolve;
    out.lat = r.lat;
    out.lon = r.lon;
    out.shortlink_url = std::string(r.shortlink_url);
    return out;
}

} // namespace tesseract
