#pragma once
#include <string>

namespace tesseract
{

/// Result of classifying (or resolving) composed text as a Google Maps /
/// OpenStreetMap link.
struct MapsLinkClassification
{
    bool matched = false;
    /// True only from `classify_maps_link`, when `shortlink_url` needs an
    /// HTTP redirect-follow (via `Client::resolve_maps_shortlink`) before
    /// coordinates are known. `Client::resolve_maps_shortlink`'s own return
    /// never sets this.
    bool needs_resolve = false;
    /// Valid only when `matched` is true and `needs_resolve` is false.
    double lat = 0.0;
    double lon = 0.0;
    /// Set when `needs_resolve` is true.
    std::string shortlink_url;
};

/// Classify `trimmed_text` (already trimmed of leading/trailing whitespace)
/// as a Google Maps / OpenStreetMap URL. Matches only when the ENTIRE string
/// is a single recognized URL — callers pass the whole trimmed composed
/// message body, not a substring. Cheap, synchronous, no I/O — safe to call
/// from the UI thread.
MapsLinkClassification classify_maps_link(const std::string& trimmed_text);

} // namespace tesseract
