#include "call_tile_layout.h"

#include <algorithm>

namespace tesseract::views
{

TileGrid best_tile_grid(int n, float w, float h, float aspect)
{
    if (n <= 0)
    {
        return {};
    }
    w = std::max(w, 0.0f);
    h = std::max(h, 0.0f);

    TileGrid best{n, 1};
    float best_area = -1.0f;
    int best_empty = 0;
    for (int cols = 1; cols <= n; ++cols)
    {
        const int rows = (n + cols - 1) / cols;
        const float cw = w / static_cast<float>(cols);
        const float ch = h / static_cast<float>(rows);
        const float fw = std::min(cw, ch * aspect);
        const float area = fw * (fw / aspect);
        const int empty = cols * rows - n;

        // Relative epsilon so float noise doesn't decide between equal fits.
        const float eps = std::max(best_area, 1.0f) * 1e-4f;
        const bool better = area > best_area + eps ||
                            (area > best_area - eps &&
                             (empty < best_empty || (empty == best_empty && rows < best.rows)));
        if (better)
        {
            best = {cols, rows};
            best_area = area;
            best_empty = empty;
        }
    }
    return best;
}

std::vector<tk::Rect> layout_tiles(int n, tk::Rect area, float aspect)
{
    std::vector<tk::Rect> out;
    if (n <= 0)
    {
        return out;
    }
    out.reserve(static_cast<size_t>(n));

    const TileGrid g = best_tile_grid(n, area.w, area.h, aspect);
    const float cw = std::max(area.w, 0.0f) / static_cast<float>(g.cols);
    const float ch = std::max(area.h, 0.0f) / static_cast<float>(g.rows);

    for (int i = 0; i < n; ++i)
    {
        const int row = i / g.cols;
        const int col = i % g.cols;
        const int in_row = std::min(g.cols, n - row * g.cols);
        const float x0 = area.x + (static_cast<float>(g.cols - in_row) * cw) * 0.5f;
        out.push_back(
            {x0 + static_cast<float>(col) * cw, area.y + static_cast<float>(row) * ch, cw, ch});
    }
    return out;
}

} // namespace tesseract::views
