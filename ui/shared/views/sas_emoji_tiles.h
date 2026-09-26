#pragma once

// The 7-emoji SAS comparison grid (Matrix short authentication string),
// painted as 4 tiles over 3 — the arrangement other Matrix clients use, so the
// user can compare row by row against the other device.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <vector>

namespace tesseract::views
{

// Height the grid occupies (independent of width).
float sas_emoji_grid_height();

// Paint `emojis` (normally exactly 7) into `area`, top-aligned.
void paint_sas_emoji_grid(tk::PaintCtx& ctx, tk::Rect area,
                          const std::vector<VerificationEmoji>& emojis);

} // namespace tesseract::views
