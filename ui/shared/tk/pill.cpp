#include "pill.h"

#include "hash_combine.h"

namespace tk
{

namespace
{

float image_side_for_height(float height)
{
    return std::max(0.0f, height - kPillImageVPad * 2.0f);
}

bool has_leading_visual(const PillSpec& spec)
{
    return spec.image != nullptr || !spec.fallback_glyph.empty() ||
           spec.reserve_leading_visual;
}

} // namespace

LineMetrics role_line_metrics(CanvasFactory& factory, FontRole role)
{
    LineMetrics m;
    if (auto layout = factory.build_text(" ", TextStyle{.role = role}))
    {
        m.ascent = layout->ascent();
        m.descent = std::max(0.0f, layout->measure().h - m.ascent);
    }
    return m;
}

PillMetrics measure_pill(CanvasFactory& factory, const PillSpec& spec,
                         float line_ascent, float line_descent)
{
    PillMetrics m;
    m.height = std::max(0.0f, line_ascent + line_descent);
    m.radius = pill_radius_for_height(m.height);
    m.image_side = has_leading_visual(spec) ? image_side_for_height(m.height) : 0.0f;

    if (auto layout = factory.build_text(spec.text, TextStyle{.role = spec.text_role}))
        m.text_width = layout->measure().w;

    float content_w = m.text_width;
    if (m.image_side > 0.0f)
        content_w += m.image_side + kPillImageGap;
    m.width = content_w + kPillOuterPadX * 2.0f;
    return m;
}

void paint_pill_background(Canvas& canvas, Rect bounds, float radius, Color bg)
{
    canvas.fill_rounded_rect(bounds, radius, bg);
}

void paint_pill_leading_visual(Canvas& canvas, CanvasFactory& /*factory*/,
                               const PillSpec& spec, Rect visual_rect)
{
    if (visual_rect.empty())
        return;
    const Point centre{visual_rect.x + visual_rect.w * 0.5f,
                       visual_rect.y + visual_rect.h * 0.5f};
    const float diameter = std::min(visual_rect.w, visual_rect.h);
    if (spec.image != nullptr)
    {
        canvas.draw_circle_image(*spec.image, centre, diameter);
    }
    else if (!spec.fallback_glyph.empty())
    {
        // Plain filled circle (pill's own colors) behind the glyph — there is
        // no per-user identity here to derive initials from, just an icon.
        canvas.draw_initials_circle(spec.fallback_glyph, centre, diameter,
                                    spec.bg, spec.fg);
    }
}

std::unique_ptr<Image> render_pill_bitmap(CanvasFactory& factory,
                                          const PillSpec& spec,
                                          float line_ascent,
                                          float line_descent,
                                          float scale_factor)
{
    const PillMetrics m = measure_pill(factory, spec, line_ascent, line_descent);
    if (m.width <= 0.0f || m.height <= 0.0f)
        return nullptr;

    auto surface = factory.create_offscreen({m.width, m.height}, scale_factor);
    if (!surface)
        return nullptr;

    Canvas& canvas = surface->canvas();
    // Don't rely on a fresh offscreen surface being zero-initialized (true
    // in practice for every current backend, but not a documented guarantee
    // for all of them) — clear explicitly so the pill's rounded corners are
    // actually transparent, not whatever was left in newly-allocated memory.
    canvas.clear(Color::rgba(0, 0, 0, 0));
    canvas.fill_rounded_rect({0, 0, m.width, m.height}, m.radius, spec.bg);

    float text_x = kPillOuterPadX;
    if (m.image_side > 0.0f)
    {
        Rect visual{kPillOuterPadX, (m.height - m.image_side) * 0.5f, m.image_side,
                    m.image_side};
        paint_pill_leading_visual(canvas, factory, spec, visual);
        text_x = visual.x + visual.w + kPillImageGap;
    }

    if (auto layout = factory.build_text(spec.text, TextStyle{.role = spec.text_role}))
    {
        const float text_y = (m.height - layout->ascent()) * 0.5f;
        canvas.draw_text(*layout, {text_x, text_y}, spec.fg);
    }

    return surface->finish();
}

std::string pill_cache_key(const PillSpec& spec, float line_ascent,
                           float line_descent, float scale_factor)
{
    std::size_t h = hash_combine(0, static_cast<std::size_t>(spec.kind));
    for (unsigned char c : spec.text)
        h = hash_combine(h, c);
    // Distinguishes not just "has an avatar" but *which* one — two different
    // users who happen to share a display name must not share a cached
    // bitmap. A resolved image's pointer identity is stable for as long as
    // its owning cache entry isn't evicted (the common case between two
    // paints of the same still-visible mention), so this is a reasonable,
    // lightweight identity; a spurious pointer reuse across a real eviction
    // just costs a redundant re-render, never wrong content, since a wrong
    // hit would still need the *text* (i.e. the same mention) to also match.
    h = hash_combine(h, reinterpret_cast<std::size_t>(spec.image));
    h = hash_combine(h, spec.reserve_leading_visual ? 1u : 0u);
    auto mix_color = [&](Color c)
    {
        h = hash_combine(h, (std::size_t(c.r) << 24) | (std::size_t(c.g) << 16) |
                                (std::size_t(c.b) << 8) | std::size_t(c.a));
    };
    mix_color(spec.bg);
    mix_color(spec.fg);
    h = hash_combine(h, std::hash<float>{}(line_ascent));
    h = hash_combine(h, std::hash<float>{}(line_descent));
    h = hash_combine(h, std::hash<float>{}(scale_factor));
    return std::to_string(h);
}

ImageRef render_pill_bitmap_cached(CanvasFactory& factory, PixmapCache& cache,
                                   const PillSpec& spec, float line_ascent,
                                   float line_descent, float scale_factor)
{
    const std::string key =
        pill_cache_key(spec, line_ascent, line_descent, scale_factor);
    if (ImageRef cached = cache.acquire(key))
        return cached;
    auto bitmap =
        render_pill_bitmap(factory, spec, line_ascent, line_descent, scale_factor);
    if (!bitmap)
        return nullptr;
    ImageRef pinned = cache.store(key, std::move(bitmap));
    cache.sweep();
    return pinned;
}

} // namespace tk
