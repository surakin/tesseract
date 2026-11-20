#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace tesseract::views
{

// Bundled SVG for a legacy-SSO identity-provider brand ("apple", "facebook",
// "github", "gitlab", "google", "twitter"). Matching is case-sensitive; an
// unknown brand yields an empty span.
std::span<const std::uint8_t> brand_icon_svg(std::string_view brand);

} // namespace tesseract::views
