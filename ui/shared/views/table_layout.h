#pragma once

// table_layout — pure column-fitting for Markdown tables rendered in the
// message timeline. Turns a parsed BodyTable (see html_spans.h) into a grid
// of shaped per-cell text layouts plus the column / row rectangles the
// renderer strokes borders along.
//
// It is a free function of (table, available width, CanvasFactory, base
// TextStyle) with no view or window state, so MessageListView can call it
// from its cached body-layout builder and the tests can drive it against a
// headless CanvasFactory.
//
// Column widths: each column takes its natural (unwrapped) content width if
// the whole table fits `avail_width`. Otherwise columns are shrunk — first
// by consuming the slack above a per-column floor, then (last resort)
// proportionally below the floor — and their cells re-shaped with wrapping,
// so a wide table degrades to taller rows and finally to clipped cells
// rather than overflowing the message column.
//
// Horizontal cell alignment is applied here as a per-cell draw-x offset
// (`TableCellBox::text_dx`), NOT via TextStyle::halign: the Qt and GTK
// rich-text backends ignore halign. A wrapped multi-line cell in a
// centre/right column therefore left-aligns its ragged lines.

#include <vector>

#include "tk/canvas.h"
#include "views/html_spans.h"

namespace tesseract::views
{

// One shaped table cell, positioned relative to the table's top-left (0,0).
struct TableCellBox
{
    std::shared_ptr<tk::TextLayout> layout;  // null for an empty cell
    std::vector<tk::TextSpan>       spans;   // for the bg / inline-image passes
    tk::Rect rect{};      // content box (padding already removed)
    float    text_dx = 0.0f; // +x inside rect for Center / Right columns
    int      row = 0;
    int      col = 0;
};

// The laid-out grid. col_x/col_w and row_y/row_h are the *outer* cell
// rectangles (including padding); the 1px borders sit in the gaps between
// them and around the outside. All coordinates are relative to the table
// origin; `size` is the table's bounding box.
struct TableGrid
{
    std::vector<TableCellBox> cells;   // row-major, rows*cols entries
    std::vector<float>        col_x;
    std::vector<float>        col_w;
    std::vector<float>        row_y;
    std::vector<float>        row_h;
    tk::Size                  size{};
    int                       header_rows = 0;
};

TableGrid compute_table_layout(const BodyTable& table, float avail_width,
                               tk::CanvasFactory& factory,
                               const tk::TextStyle& base);

// Serialize a parsed table back to canonical GitHub-Flavored Markdown
// (`| a | b |` rows with a `| --- | :-: |` delimiter after the first row),
// for plain-text / clipboard extraction.
std::string table_to_gfm(const BodyTable& table);

// Grid metrics — exposed for the renderer and tests.
inline constexpr float kTableCellPadX = 8.0f;
inline constexpr float kTableCellPadY = 4.0f;
inline constexpr float kTableBorder   = 1.0f;

} // namespace tesseract::views
