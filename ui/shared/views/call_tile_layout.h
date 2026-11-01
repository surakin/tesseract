#pragma once

// call_tile_layout — pure grid fitting for call participant tiles.
//
// ParticipantTile letterboxes its video, so the useful size of a tile is the
// largest `aspect`-ratio box that fits in its cell. best_tile_grid() tries
// every column count and keeps the one that maximises that box, which makes a
// wide strip a single row, a tall narrow area a single column, and anything
// in between a balanced grid — without count-based thresholds.

#include <vector>

#include "tk/canvas.h"

namespace tesseract::views
{

constexpr float kCallTileAspect = 16.0f / 9.0f;

struct TileGrid
{
    int cols = 1;
    int rows = 1;
};

// Column / row count for `n` tiles in a `w`×`h` area. Ties go to fewer empty
// cells, then fewer rows. n <= 0 returns {1, 1}.
TileGrid best_tile_grid(int n, float w, float h, float aspect = kCallTileAspect);

// Cell rects for `n` tiles inside `area` (row-major), using best_tile_grid.
// A partial last row keeps the full cell width and is centred horizontally.
std::vector<tk::Rect> layout_tiles(int n, tk::Rect area, float aspect = kCallTileAspect);

} // namespace tesseract::views
