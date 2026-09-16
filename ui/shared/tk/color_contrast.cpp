#include "color_contrast.h"

#include <algorithm>
#include <cmath>

namespace tk
{

namespace
{

float srgb_channel_to_linear(std::uint8_t byte)
{
    const float c = static_cast<float>(byte) / 255.0f;
    return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

float relative_luminance(Color c)
{
    return 0.2126f * srgb_channel_to_linear(c.r) +
           0.7152f * srgb_channel_to_linear(c.g) +
           0.0722f * srgb_channel_to_linear(c.b);
}

float contrast_ratio(Color a, Color b)
{
    const float la = relative_luminance(a);
    const float lb = relative_luminance(b);
    const float lighter = std::max(la, lb);
    const float darker = std::min(la, lb);
    return (lighter + 0.05f) / (darker + 0.05f);
}

Color composite_over(Color fg, Color bg)
{
    const float alpha = static_cast<float>(fg.a) / 255.0f;
    auto mix = [alpha](std::uint8_t f, std::uint8_t b)
    {
        return static_cast<std::uint8_t>(static_cast<float>(f) * alpha +
                                          static_cast<float>(b) * (1.0f - alpha) +
                                          0.5f);
    };
    return {mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b), 255};
}

bool meets_wcag_aa(Color fg, Color bg, ContrastLevel level)
{
    const float ratio = contrast_ratio(fg, bg);
    return ratio >= (level == ContrastLevel::Text ? 4.5f : 3.0f);
}

} // namespace tk
