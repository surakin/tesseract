#include <catch2/catch_test_macros.hpp>

#include "tk/pill.h"
#include "tk_test_surface.h"

#include <memory>

namespace
{

tk::PillSpec make_spec(std::string text, tk::PillKind kind = tk::PillKind::User)
{
    tk::PillSpec spec;
    spec.text = std::move(text);
    spec.kind = kind;
    spec.bg = tk::Color::rgb(0x2E3B5E);
    spec.fg = tk::Color::rgb(0xA8C5FF);
    return spec;
}

} // namespace

TEST_CASE("tk::measure_pill never exceeds the caller's line height",
          "[tk][pill]")
{
    auto surface = TestSurface::create(200, 60);
    auto spec = make_spec("Alice");
    const float ascent = 14.0f;
    const float descent = 4.0f;
    auto m = tk::measure_pill(surface->factory(), spec, ascent, descent);
    CHECK(m.height == ascent + descent);
    CHECK(m.width > 0.0f);
}

TEST_CASE("tk::measure_pill uses a true stadium radius", "[tk][pill]")
{
    auto surface = TestSurface::create(200, 60);
    auto spec = make_spec("Bob");
    auto m = tk::measure_pill(surface->factory(), spec, 10.0f, 6.0f);
    CHECK(m.radius == m.height * 0.5f);
    CHECK(tk::pill_radius_for_height(m.height) == m.radius);
}

TEST_CASE("tk::measure_pill grows wider with a leading image than without",
          "[tk][pill]")
{
    auto surface = TestSurface::create(200, 60);
    auto no_image = make_spec("Alice");
    auto no_image_metrics =
        tk::measure_pill(surface->factory(), no_image, 14.0f, 4.0f);

    // Use a fallback glyph to force a leading visual without needing a real
    // decoded image — measure_pill only checks whether a visual is present.
    auto with_glyph = make_spec("Alice");
    with_glyph.fallback_glyph = "@";
    auto with_glyph_metrics =
        tk::measure_pill(surface->factory(), with_glyph, 14.0f, 4.0f);

    CHECK(with_glyph_metrics.image_side > 0.0f);
    CHECK(no_image_metrics.image_side == 0.0f);
    CHECK(with_glyph_metrics.width > no_image_metrics.width);
}

TEST_CASE("tk::render_pill_bitmap produces a bitmap sized to its own metrics",
          "[tk][pill]")
{
    auto surface = TestSurface::create(200, 60);
    auto spec = make_spec("Alice");
    auto m = tk::measure_pill(surface->factory(), spec, 14.0f, 4.0f);

    auto bitmap =
        tk::render_pill_bitmap(surface->factory(), spec, 14.0f, 4.0f, 1.0f);
    REQUIRE(bitmap != nullptr);
    // Allow for float->int rounding in the offscreen surface's pixel size.
    CHECK(std::abs(bitmap->width() - static_cast<int>(m.width)) <= 1);
    CHECK(std::abs(bitmap->height() - static_cast<int>(m.height)) <= 1);
}

TEST_CASE("tk::render_pill_bitmap draws the pill's background colour",
          "[tk][pill]")
{
    auto surface = TestSurface::create(200, 60);
    // No text at all: render_pill_bitmap also draws the label on top of the
    // background fill, so an empty label keeps every sampled pixel pure
    // background colour, unaffected by anti-aliased glyph edges.
    auto spec = make_spec("");
    spec.bg = tk::Color::rgba(0x11, 0x22, 0x33, 0xFF);

    auto bitmap =
        tk::render_pill_bitmap(surface->factory(), spec, 14.0f, 4.0f, 1.0f);
    REQUIRE(bitmap != nullptr);

    // Draw the rendered bitmap onto the test surface (over a known-white
    // background) so we can sample it back via read_pixel(), then check a
    // point at the vertical centre of the pill, safely inside its rounded
    // corners, actually took the background colour rather than staying
    // whatever the surface's own clear colour was.
    surface->canvas().draw_image(
        *bitmap, {0, 0, static_cast<float>(bitmap->width()),
                  static_cast<float>(bitmap->height())});
    tk::Color px = surface->read_pixel(bitmap->width() / 2, bitmap->height() / 2);
    CHECK(px.r == spec.bg.r);
    CHECK(px.g == spec.bg.g);
    CHECK(px.b == spec.bg.b);
}

namespace
{

// Mirrors MessageListView::substitute_image_placeholders: an is_image span's
// text becomes exactly one U+FFFC once html_spans.cpp's empty-text mention
// leaf is prepared for layout.
tk::TextSpan make_pill_placeholder_span(std::string label,
                                        tk::PillKind kind = tk::PillKind::User)
{
    tk::TextSpan sp;
    sp.is_image = true;
    sp.is_mention = true;
    sp.pill_kind = kind;
    sp.image_alt = std::move(label);
    sp.text = "\xEF\xBF\xBC";
    sp.has_background = true;
    sp.background = tk::Color::rgb(0x2E3B5E);
    sp.has_color = true;
    sp.color = tk::Color::rgb(0xA8C5FF);
    return sp;
}

} // namespace

TEST_CASE("a mention pill span reserves width proportional to its label, "
          "not a fixed emoji-square box",
          "[tk][pill][canvas]")
{
    // This is the core fix for "timeline and composer pills must look
    // identical": the inline-object mechanism used to always reserve a
    // fixed emoji-sized square for every is_image span. A pill's label
    // varies in length, so a short and a long mention must reserve visibly
    // different widths — if they didn't, the timeline could only be
    // painting a fixed-size box, not the real variable-width pill bitmap.
    auto surface = TestSurface::create(400, 60);
    tk::TextSpan shortp = make_pill_placeholder_span("Al");
    tk::TextSpan longp =
        make_pill_placeholder_span("Alexandria The Magnificent Third");
    const tk::TextSpan short_arr[] = {shortp};
    const tk::TextSpan long_arr[] = {longp};

    auto short_layout = surface->factory().build_rich_text(
        short_arr, tk::TextStyle{.role = tk::FontRole::Body});
    auto long_layout = surface->factory().build_rich_text(
        long_arr, tk::TextStyle{.role = tk::FontRole::Body});
    REQUIRE(short_layout != nullptr);
    REQUIRE(long_layout != nullptr);
    CHECK(long_layout->measure().w > short_layout->measure().w * 2.0f);
}

TEST_CASE("a mention pill span does not inflate the surrounding line's height",
          "[tk][pill][canvas]")
{
    auto surface = TestSurface::create(400, 60);

    const tk::TextSpan plain_text[] = {[] {
        tk::TextSpan sp;
        sp.text = "hello ";
        return sp;
    }()};
    auto plain_layout = surface->factory().build_rich_text(
        plain_text, tk::TextStyle{.role = tk::FontRole::Body});
    REQUIRE(plain_layout != nullptr);

    tk::TextSpan text_part;
    text_part.text = "hello ";
    tk::TextSpan pill_part = make_pill_placeholder_span("Alice");
    const tk::TextSpan mixed[] = {text_part, pill_part};
    auto mixed_layout = surface->factory().build_rich_text(
        mixed, tk::TextStyle{.role = tk::FontRole::Body});
    REQUIRE(mixed_layout != nullptr);

    // Allow a hair of float slop; must not be *meaningfully* taller.
    CHECK(mixed_layout->measure().h <= plain_layout->measure().h + 0.5f);
}

TEST_CASE("a wrapped (multi-line) row's pill renders at the same width it "
          "reserved, not stretched",
          "[tk][pill][canvas]")
{
    // Regression test for a real bug: MessageListView::paint_span_images
    // used to size the painted pill bitmap from TextLayout::ascent(), which
    // — for a wrapped multi-line body, the common case — has no single
    // "this line's ascent" to report and instead returns the *whole
    // layout's total height across every line*. That inflated height fed
    // into measure_pill() produced a bitmap sized for a much taller box
    // than the one actually reserved at layout time (which sizes correctly,
    // from the role's own font metrics, independent of line count) —
    // draw_image() then had to squash the taller/wider bitmap into the
    // shorter reserved box, distorting it (reported visually as the pill
    // looking "stretched horizontally", since height compresses far more
    // than width does). The fix is to never use layout.ascent() for this;
    // rebuild stable, line-count-independent metrics instead (see
    // paint_span_images's ensure_pill_metrics()) — reproduced here directly.
    auto surface = TestSurface::create(400, 200);

    tk::TextSpan text_part;
    text_part.text = "vos tenes forma de cambiar ";
    tk::TextSpan pill_part =
        make_pill_placeholder_span("Alejandro Sztrajman (WA)");
    const tk::TextSpan spans[] = {text_part, pill_part};

    tk::TextStyle style{.role = tk::FontRole::Body};
    style.wrap = true;
    style.max_width = 250.0f; // force wrapping across multiple lines
    auto layout = surface->factory().build_rich_text(spans, style);
    REQUIRE(layout != nullptr);
    REQUIRE(layout->line_count() > 1); // otherwise this isn't exercising the bug

    const int boff = static_cast<int>(text_part.text.size());
    const int len = static_cast<int>(pill_part.text.size());
    auto rects = layout->selection_rects(boff, boff + len);
    REQUIRE(!rects.empty());
    // A run that straddles a wrap point yields a zero-width rect trailing
    // the old line (the anchor's "before" position) alongside the real box
    // on the line it actually wrapped to — the widest rect is the pill's
    // reserved box, not necessarily rects[0].
    const tk::Rect* reserved = &rects[0];
    for (const auto& r : rects)
        if (r.w > reserved->w)
            reserved = &r;
    const float reserved_w = reserved->w;

    // The fix: stable metrics from a throwaway single-line measurement,
    // independent of how many lines this particular row wraps to.
    float ascent = 12.0f, descent = 4.0f;
    if (auto m = surface->factory().build_text(
            " ", tk::TextStyle{.role = tk::FontRole::Body}))
    {
        ascent = m->ascent();
        descent = std::max(0.0f, m->measure().h - ascent);
    }

    tk::PillSpec spec;
    spec.text = pill_part.image_alt;
    spec.kind = pill_part.pill_kind;
    spec.reserve_leading_visual = true;
    spec.bg = pill_part.background;
    spec.fg = pill_part.color;
    auto bitmap = tk::render_pill_bitmap(surface->factory(), spec, ascent,
                                         descent, 1.0f);
    REQUIRE(bitmap != nullptr);

    // Within a pixel of what was actually reserved — not off by a factor of
    // 2 the way feeding the wrapped layout's total height in would be.
    CHECK(std::abs(static_cast<float>(bitmap->width()) - reserved_w) < 1.5f);
}

TEST_CASE("clicking anywhere across a mention pill's width hits its link, "
          "not just the left half",
          "[tk][pill][canvas]")
{
    // Regression test: TextLayout::link_at() used to hit-test via
    // CTLineGetStringIndexForPosition (macOS), a function designed for
    // *caret placement* — it snaps to whichever side of a character's
    // advance width the point is nearer to, so the right half of any
    // character reports the index *after* it. For an ordinary narrow
    // character that's imperceptible, but a mention pill is one very wide
    // character (a single CTRunDelegate-backed U+FFFC spanning the whole
    // pill's box) — clicking its right half reported the next character's
    // index, outside the pill's own url range, so only the left half of
    // the pill was ever clickable. The fix hit-tests via geometric x-offset
    // containment (mirrors selection_rects()) instead.
    auto surface = TestSurface::create(400, 60);

    tk::TextSpan pill_part = make_pill_placeholder_span("Alejandro Sztrajman");
    pill_part.url = "https://matrix.to/#/@alejandro:example.org";
    const tk::TextSpan spans[] = {pill_part};

    auto layout = surface->factory().build_rich_text(
        spans, tk::TextStyle{.role = tk::FontRole::Body});
    REQUIRE(layout != nullptr);

    auto rects = layout->selection_rects(0, static_cast<int>(pill_part.text.size()));
    REQUIRE(!rects.empty());
    const tk::Rect& r = rects[0];
    REQUIRE(r.w > 20.0f); // must be a real, wide pill for this test to mean anything

    // Sample well inside the left edge, dead centre, and well inside the
    // right edge — all three must resolve to the pill's url.
    for (float frac : {0.05f, 0.5f, 0.95f})
    {
        tk::Point p{r.x + r.w * frac, r.y + r.h * 0.5f};
        CHECK(layout->link_at(p) == pill_part.url);
    }
}
