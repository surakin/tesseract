#include "views/table_layout.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{
namespace
{

// A column never keeps more than this inner width when the table is being
// shrunk to fit — unless its natural content is already narrower, in which
// case the natural width is the floor (a column is never padded wider than
// its content). A long unbreakable token (e.g. a URL) in an over-wide table
// therefore ends up char-wrapped inside ~this width.
constexpr float kColMaxFloorInner = 96.0f;

std::string cell_plain(const std::vector<tk::TextSpan>& spans)
{
    std::string out;
    for (const auto& sp : spans)
        out += sp.is_image ? sp.image_alt : sp.text;
    return out;
}

} // namespace

TableGrid compute_table_layout(const BodyTable& table, float avail_width,
                               tk::CanvasFactory& factory,
                               const tk::TextStyle& base)
{
    TableGrid g;
    g.header_rows = table.header_rows;

    const std::size_t ncols = table.col_align.size();
    const std::size_t nrows = table.rows.size();
    if (ncols == 0 || nrows == 0)
        return g;

    const float padX = kTableCellPadX;
    const float padY = kTableCellPadY;
    const float border = kTableBorder;

    tk::TextStyle natural_style = base;
    natural_style.wrap      = false;
    natural_style.max_width = -1.0f;
    natural_style.trim      = tk::TextTrim::None;

    // ── 1. Natural (unwrapped) content width per cell and per column ──────────
    // A wrapped rich-text layout reports max_width as its width on some
    // backends (Qt/GTK), so per-cell natural widths are measured here with a
    // no-wrap layout and reused for alignment offsets below. The no-wrap
    // layout is also kept and reused directly for any cell that ends up
    // fitting its column (the common case) — see step 5.
    std::vector<float> cell_nat(nrows * ncols, 0.0f);
    std::vector<std::shared_ptr<tk::TextLayout>> nat_layout(nrows * ncols);
    std::vector<float> nat(ncols, 0.0f);
    for (std::size_t c = 0; c < ncols; ++c)
    {
        for (std::size_t r = 0; r < nrows; ++r)
        {
            const auto& spans = table.rows[r][c].spans;
            if (spans.empty())
                continue;
            std::shared_ptr<tk::TextLayout> lay =
                factory.build_rich_text(spans, natural_style);
            if (lay)
            {
                const float w = lay->measure().w;
                cell_nat[r * ncols + c]    = w;
                nat_layout[r * ncols + c]  = std::move(lay);
                nat[c] = std::max(nat[c], w);
            }
        }
    }

    // ── 2. Desired outer width per column, and the total ──────────────────────
    std::vector<float> desired(ncols, 0.0f);
    float total = border;
    for (std::size_t c = 0; c < ncols; ++c)
    {
        desired[c] = nat[c] + 2.0f * padX;
        total += desired[c] + border;
    }

    // ── 3. Fit, or shrink ────────────────────────────────────────────────────
    std::vector<float> col_w = desired;
    if (avail_width > 0.0f && total > avail_width)
    {
        const float deficit = total - avail_width;

        std::vector<float> slack(ncols, 0.0f);
        float total_slack = 0.0f;
        for (std::size_t c = 0; c < ncols; ++c)
        {
            const float floor_outer =
                std::min(nat[c], kColMaxFloorInner) + 2.0f * padX;
            slack[c] = std::max(0.0f, desired[c] - floor_outer);
            total_slack += slack[c];
        }

        if (total_slack >= deficit && total_slack > 0.0f)
        {
            // Enough give above the floors — shrink the widest columns.
            for (std::size_t c = 0; c < ncols; ++c)
                col_w[c] = desired[c] - deficit * (slack[c] / total_slack);
        }
        else
        {
            // Even the floors overflow: scale every column down
            // proportionally so the grid still fits exactly. Cell content
            // clips against the column in paint — this is the last resort.
            const float avail_inner = std::max(
                1.0f, avail_width - static_cast<float>(ncols + 1) * border);
            float sum_desired = 0.0f;
            for (float d : desired)
                sum_desired += d;
            if (sum_desired > 0.0f)
                for (std::size_t c = 0; c < ncols; ++c)
                    col_w[c] = std::max(
                        1.0f, desired[c] * avail_inner / sum_desired);
        }
    }

    // ── 4. Column x positions ────────────────────────────────────────────────
    g.col_x.resize(ncols);
    g.col_w.resize(ncols);
    float x = border;
    for (std::size_t c = 0; c < ncols; ++c)
    {
        g.col_x[c] = x;
        g.col_w[c] = col_w[c];
        x += col_w[c] + border;
    }

    // ── 5. Shape every cell wrapped to its column, then measure row heights ───
    g.cells.resize(nrows * ncols);
    g.row_y.resize(nrows);
    g.row_h.resize(nrows);

    std::vector<bool> cell_wrapped(nrows * ncols, false);
    float y = border;
    for (std::size_t r = 0; r < nrows; ++r)
    {
        float row_content_h = 0.0f;
        for (std::size_t c = 0; c < ncols; ++c)
        {
            const std::size_t i = r * ncols + c;
            const float inner = std::max(1.0f, g.col_w[c] - 2.0f * padX);
            TableCellBox& box = g.cells[i];
            box.row   = static_cast<int>(r);
            box.col   = static_cast<int>(c);
            box.spans = table.rows[r][c].spans;

            if (!box.spans.empty())
            {
                // Keep the cell on one line while it fits its column; only
                // wrap when it must (so measure().w stays tight for the
                // alignment offset — a wrapped rich layout reports max_width).
                const bool wrap = cell_nat[i] > inner + 0.5f;
                cell_wrapped[i] = wrap;
                if (!wrap && nat_layout[i])
                {
                    // Fits unwrapped — the no-wrap layout from step 1 is
                    // identical to what we'd rebuild here.
                    box.layout = nat_layout[i];
                }
                else
                {
                    tk::TextStyle cs = base;
                    cs.wrap      = wrap;
                    cs.max_width = inner;
                    cs.trim      = tk::TextTrim::None;
                    cs.halign    = tk::TextHAlign::Leading;
                    box.layout   = factory.build_rich_text(box.spans, cs);
                }
            }

            const float cell_h = box.layout ? box.layout->measure().h : 0.0f;
            row_content_h = std::max(row_content_h, cell_h);
        }

        const float row_h = row_content_h + 2.0f * padY;
        g.row_y[r] = y;
        g.row_h[r] = row_h;

        // Fill in each cell's content rect + alignment offset now that the
        // row height is known.
        for (std::size_t c = 0; c < ncols; ++c)
        {
            const float inner = std::max(1.0f, g.col_w[c] - 2.0f * padX);
            TableCellBox& box = g.cells[r * ncols + c];
            box.rect = {g.col_x[c] + padX, y + padY, inner, row_content_h};

            float dx = 0.0f;
            if (box.layout && !cell_wrapped[r * ncols + c])
            {
                const float mw = std::min(cell_nat[r * ncols + c], inner);
                const float extra = std::max(0.0f, inner - mw);
                switch (table.col_align[c])
                {
                case TableAlign::Right:
                    dx = extra;
                    break;
                case TableAlign::Center:
                    dx = extra * 0.5f;
                    break;
                default:
                    break;
                }
            }
            box.text_dx = dx;
        }

        y += row_h + border;
    }

    g.size = {g.col_x.back() + g.col_w.back() + border, y};
    return g;
}

std::string table_to_gfm(const BodyTable& table)
{
    const std::size_t ncols = table.col_align.size();
    if (ncols == 0 || table.rows.empty())
        return {};

    auto escape = [](std::string s)
    {
        std::string out;
        out.reserve(s.size());
        for (char ch : s)
        {
            if (ch == '|')
                out += "\\|";
            else if (ch == '\n')
                out += ' ';
            else
                out += ch;
        }
        return out;
    };

    auto emit_row = [&](const std::vector<TableCell>& row) -> std::string
    {
        std::string line = "|";
        for (std::size_t c = 0; c < ncols; ++c)
        {
            line += ' ';
            line += c < row.size() ? escape(cell_plain(row[c].spans))
                                   : std::string{};
            line += " |";
        }
        line += '\n';
        return line;
    };

    std::string out;
    out += emit_row(table.rows[0]);

    // GFM requires a delimiter row after the first (header) row.
    out += "|";
    for (std::size_t c = 0; c < ncols; ++c)
    {
        switch (table.col_align[c])
        {
        case TableAlign::Left:
            out += " :--- |";
            break;
        case TableAlign::Center:
            out += " :---: |";
            break;
        case TableAlign::Right:
            out += " ---: |";
            break;
        default:
            out += " --- |";
            break;
        }
    }
    out += '\n';

    for (std::size_t r = 1; r < table.rows.size(); ++r)
        out += emit_row(table.rows[r]);

    return out;
}

} // namespace tesseract::views
