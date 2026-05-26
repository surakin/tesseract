#pragma once
#include "tk/canvas.h"

namespace tesseract::views
{

// Warm-yellow banner tint shared by RecoveryBanner and VerificationBanner —
// always a notice, not a decorative element.
inline constexpr float kBannerPadX       = 12.0f;
inline constexpr float kBannerPadY       = 8.0f;
inline constexpr float kBannerGap        = 8.0f;
inline constexpr float kBannerDismissSide = 24.0f;

inline const tk::Color kBannerBg        = tk::Color::rgb(0xFFF4D6);
inline const tk::Color kBannerBorder    = tk::Color::rgb(0xE0C97A);
inline const tk::Color kBannerLabelText = tk::Color::rgb(0x5C4500);

} // namespace tesseract::views
