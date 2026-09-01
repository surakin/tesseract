#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "views/table_layout.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using tesseract::views::BodyTable;
using tesseract::views::compute_table_layout;
using tesseract::views::TableAlign;
using tesseract::views::TableCell;
using tesseract::views::table_to_gfm;
using tesseract::views::TableGrid;

namespace
{
std::unique_ptr<TestSurface> surface()
{
    return TestSurface::create(1000, 400);
}

tk::TextStyle base_style()
{
    tk::TextStyle s{};
    s.role = tk::FontRole::Body;
    s.wrap = true;
    return s;
}

TableCell cell(const std::string& text)
{
    tk::TextSpan sp;
    sp.text = text;
    TableCell c;
    if (!text.empty())
        c.spans.push_back(sp);
    return c;
}

BodyTable make_table(std::vector<std::vector<std::string>> rows,
                     std::vector<TableAlign> align = {}, int header_rows = 0)
{
    BodyTable t;
    std::size_t ncols = 0;
    for (auto& r : rows)
        ncols = std::max(ncols, r.size());
    t.col_align = align;
    t.col_align.resize(ncols, TableAlign::Default);
    t.header_rows = header_rows;
    for (auto& r : rows)
    {
        std::vector<TableCell> row;
        for (std::size_t c = 0; c < ncols; ++c)
            row.push_back(cell(c < r.size() ? r[c] : std::string{}));
        t.rows.push_back(std::move(row));
    }
    return t;
}

float total_border_span(const TableGrid& g)
{
    // Sum of column widths + the (ncols + 1) one-pixel borders.
    float w = tesseract::views::kTableBorder;
    for (float cw : g.col_w)
        w += cw + tesseract::views::kTableBorder;
    return w;
}
} // namespace

TEST_CASE("table_layout: a small table uses natural column widths",
          "[table_layout]")
{
    auto s = surface();
    auto t = make_table({{"a", "bb"}, {"cccc", "d"}});
    TableGrid g = compute_table_layout(t, 900.0f, s->factory(), base_style());

    REQUIRE(g.col_w.size() == 2);
    REQUIRE(g.row_y.size() == 2);
    REQUIRE(g.cells.size() == 4);
    // Column 0 holds "cccc", column 1 holds "bb" → col 0 is wider.
    CHECK(g.col_w[0] > g.col_w[1]);
    // Fits: total stays within the available width.
    CHECK(g.size.w <= 900.0f + 1.0f);
    CHECK(total_border_span(g) == Catch::Approx(g.size.w).margin(0.5f));
}

TEST_CASE("table_layout: wider content produces a wider column", "[table_layout]")
{
    auto s = surface();
    auto narrow = compute_table_layout(make_table({{"x"}}), 900.0f, s->factory(),
                                       base_style());
    auto wide = compute_table_layout(
        make_table({{"a much longer piece of cell text"}}), 900.0f,
        s->factory(), base_style());
    CHECK(wide.col_w[0] > narrow.col_w[0]);
}

TEST_CASE("table_layout: an over-wide table is shrunk to fit", "[table_layout]")
{
    auto s = surface();
    auto t = make_table({{"the quick brown fox jumps over the lazy dog",
                          "pack my box with five dozen liquor jugs",
                          "how vexingly quick daft zebras jump"}});
    // Natural width is far more than 120px.
    TableGrid g = compute_table_layout(t, 120.0f, s->factory(), base_style());
    CHECK(g.size.w <= 120.0f + 1.0f);
    // Cells wrapped → the single row is taller than one text line.
    auto one_line = compute_table_layout(make_table({{"x", "y", "z"}}), 900.0f,
                                         s->factory(), base_style());
    CHECK(g.row_h[0] > one_line.row_h[0]);
}

TEST_CASE("table_layout: a long unbreakable token stays within the column",
          "[table_layout]")
{
    auto s = surface();
    auto t = make_table(
        {{"https://example.com/a/very/long/path/that/cannot/be/broken/nicely",
          "b"}});
    TableGrid g = compute_table_layout(t, 100.0f, s->factory(), base_style());
    CHECK(g.size.w <= 100.0f + 1.0f);
}

TEST_CASE("table_layout: ragged rows still produce a full grid", "[table_layout]")
{
    auto s = surface();
    BodyTable t = make_table({{"a", "b", "c"}, {"d"}});
    REQUIRE(t.rows[1].size() == 3); // padded by make_table
    TableGrid g = compute_table_layout(t, 900.0f, s->factory(), base_style());
    CHECK(g.cells.size() == 6);
    CHECK(g.col_w.size() == 3);
    CHECK(g.cells[3].layout != nullptr); // row 1, col 0 = "d"
    CHECK(g.cells[4].layout == nullptr); // row 1, col 1 = empty
}

TEST_CASE("table_layout: a single-column table lays out", "[table_layout]")
{
    auto s = surface();
    TableGrid g = compute_table_layout(make_table({{"only"}, {"one"}}), 900.0f,
                                       s->factory(), base_style());
    REQUIRE(g.col_w.size() == 1);
    REQUIRE(g.row_y.size() == 2);
    CHECK(g.row_y[1] > g.row_y[0]);
}

TEST_CASE("table_layout: alignment sets a per-cell draw offset", "[table_layout]")
{
    auto s = surface();
    // One wide row establishes the column width; a short value in row 1 has
    // room to shift.
    auto left = make_table({{"wwwwwwwwwwwwwwww"}, {"x"}}, {TableAlign::Left});
    auto right = make_table({{"wwwwwwwwwwwwwwww"}, {"x"}}, {TableAlign::Right});
    auto center = make_table({{"wwwwwwwwwwwwwwww"}, {"x"}}, {TableAlign::Center});

    auto gl = compute_table_layout(left, 900.0f, s->factory(), base_style());
    auto gr = compute_table_layout(right, 900.0f, s->factory(), base_style());
    auto gc = compute_table_layout(center, 900.0f, s->factory(), base_style());

    // cells[1] == row 1, col 0 (the short "x").
    CHECK(gl.cells[1].text_dx == Catch::Approx(0.0f));
    CHECK(gr.cells[1].text_dx > 1.0f);
    CHECK(gc.cells[1].text_dx > 1.0f);
    CHECK(gr.cells[1].text_dx > gc.cells[1].text_dx);
}

TEST_CASE("table_layout: header_rows is carried through", "[table_layout]")
{
    auto s = surface();
    auto t = make_table({{"H1", "H2"}, {"a", "b"}}, {}, /*header_rows=*/1);
    TableGrid g = compute_table_layout(t, 900.0f, s->factory(), base_style());
    CHECK(g.header_rows == 1);
}

TEST_CASE("table_layout: an empty table yields an empty grid", "[table_layout]")
{
    auto s = surface();
    BodyTable empty;
    TableGrid g = compute_table_layout(empty, 900.0f, s->factory(), base_style());
    CHECK(g.cells.empty());
    CHECK(g.size.w == 0.0f);
    CHECK(g.size.h == 0.0f);
}

TEST_CASE("table_to_gfm: emits a header, delimiter and body rows",
          "[table_layout]")
{
    auto t = make_table({{"Name", "Age"}, {"Bob", "42"}},
                        {TableAlign::Default, TableAlign::Right},
                        /*header_rows=*/1);
    std::string md = table_to_gfm(t);
    CHECK(md ==
          "| Name | Age |\n"
          "| --- | ---: |\n"
          "| Bob | 42 |\n");
}

TEST_CASE("table_to_gfm: escapes pipes in cell text", "[table_layout]")
{
    auto t = make_table({{"a|b", "c"}});
    std::string md = table_to_gfm(t);
    CHECK(md.find("a\\|b") != std::string::npos);
}
