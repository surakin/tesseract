#pragma once

// WCAG 2.x contrast-ratio utilities. General-purpose accessibility math —
// not theme-specific — kept as a peer module to theme.h so it can also
// back a future custom-accent-color picker or other live-validated UI.

#include "canvas.h"

namespace tk
{

// Relative luminance of an *opaque* sRGB color (alpha ignored — composite
// translucent colors over their backdrop first via composite_over()).
// https://www.w3.org/TR/WCAG21/#dfn-relative-luminance
float relative_luminance(Color c);

// WCAG contrast ratio between two opaque colors, always >= 1.0.
// https://www.w3.org/TR/WCAG21/#dfn-contrast-ratio
float contrast_ratio(Color a, Color b);

// Alpha-composites `fg` over opaque `bg` (simple "over" blend), so a
// translucent token (e.g. selection, subtle_hover/pressed) can be checked
// against its real backdrop.
Color composite_over(Color fg, Color bg);

enum class ContrastLevel
{
    Text, // WCAG AA normal text (1.4.3): 4.5:1
    UI,   // WCAG AA non-text / UI components (1.4.11): 3:1
};

bool meets_wcag_aa(Color fg, Color bg, ContrastLevel level);

} // namespace tk
