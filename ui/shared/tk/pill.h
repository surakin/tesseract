#pragma once

// Shared "mention pill" primitives: one rounded, single-line-height chip with
// an optional leading avatar/emoji image (or a fallback glyph) before a text
// label. Both the timeline and the composer draw a pill by rasterizing the
// exact same bitmap via render_pill_bitmap()/render_pill_bitmap_cached() —
// there is deliberately only one pill-drawing implementation, not two that
// merely share constants. The timeline reserves an inline placeholder box
// sized by measure_pill() (see TextSpan::pill_kind / is_image in canvas.h)
// and paints the resulting bitmap into it, exactly like an MSC2545 custom
// emoticon; the composer inserts the bitmap as an atomic inline object the
// same way. See docs/UI-PARITY.md.
//
// This header is toolkit-generic: PillKind only selects a fallback glyph
// when no image is supplied. It carries no Matrix domain knowledge and no
// click-dispatch semantics — those stay keyed off TextSpan::url via
// Client::parse_matrix_link / ShellBase::open_matrix_link.

#include "canvas.h"
#include "pixmap_cache.h"

#include <memory>
#include <string>

namespace tk
{

struct PillSpec
{
    std::string text; // resolved display label, e.g. "Alice" or "#general"
    PillKind kind = PillKind::Generic;
    // Resolved leading image (avatar/emoji), or nullptr. Never owned here.
    const Image* image = nullptr;
    // UTF-8 fallback glyph drawn in a plain circle when `image` is null and
    // this is non-empty (e.g. "👤"). Empty means no leading visual at all.
    std::string fallback_glyph;
    // True if this pill should reserve room for a leading visual regardless
    // of whether `image`/`fallback_glyph` is set on *this* call — needed
    // because the timeline must size (and lay out) the inline placeholder
    // box synchronously, before an avatar fetched asynchronously resolves.
    // A caller sizing a placeholder ahead of paint sets this from the pill's
    // kind (e.g. true for User); a caller painting with an already-resolved
    // avatar leaves it false and just sets `image`. The two must agree on
    // the same width for a given (kind, text, scale) or the timeline's
    // reserved box and the painted bitmap will mismatch.
    bool reserve_leading_visual = false;
    Color bg{};
    Color fg{};
    // Must match the FontRole of the surrounding text for a timeline pill
    // (so the glyphs read identically inline); the composer's isolated
    // bitmap can pick whatever role its own body text uses.
    FontRole text_role = FontRole::Body;
};

struct PillMetrics
{
    float height = 0;     // == line_ascent + line_descent, verbatim
    float radius = 0;      // == height / 2 — true stadium, always
    float image_side = 0;  // leading image/glyph diameter; 0 when spec has none
    float text_width = 0;  // shaped width of spec.text at text_role
    float width = 0;       // total pill width including padding
};

// True stadium radius for a pill of this height. Centralised so no call site
// picks its own cap (e.g. the timeline's previous min(7px, height/2)).
inline float pill_radius_for_height(float height)
{
    return height * 0.5f;
}

struct LineMetrics
{
    float ascent = 0;
    float descent = 0;
};

// Ascent/descent of one line of text at `role`, via CanvasFactory::build_text
// — the single cross-platform abstraction every backend already implements,
// rather than each call site querying its own native font-metrics API
// (CTFont/QFontMetricsF/Pango/DWrite). Centralised so pill height agrees
// between layout time and paint time and across platforms without each of
// them re-deriving it a different way. Returns {0,0} if the backend can't
// build text (should not happen for a valid factory).
LineMetrics role_line_metrics(CanvasFactory& factory, FontRole role);

// Shared padding policy — every pill everywhere it's drawn (timeline,
// composer) uses these same numbers, since it's the same rendering code
// either way. Kept as constants (not settings) since pills are a fixed,
// small UI element unlike e.g. reaction chips
// (Settings::reaction_chip_height), which the user can resize.
inline constexpr float kPillOuterPadX = 8.0f; // left/right padding around content
inline constexpr float kPillImageGap = 4.0f;  // gap between leading visual and text
inline constexpr float kPillImageVPad = 2.0f; // vertical inset of the leading visual

// Derives PillMetrics from the *line's own* ascent+descent, supplied by the
// caller — this never invents height independently, which is what
// guarantees a pill can never grow taller than the surrounding text line.
// A backend's text-shaper calls this at layout time (with
// spec.reserve_leading_visual set from the pill's kind, image/fallback_glyph
// left unset since the avatar isn't resolved yet) to size the inline
// placeholder box it reserves for is_image/pill_kind spans; the painter
// calls it again (transitively, via render_pill_bitmap) once the avatar may
// have resolved. Both calls must be given the same line_ascent/line_descent
// to agree on a width.
PillMetrics measure_pill(CanvasFactory& factory, const PillSpec& spec,
                         float line_ascent, float line_descent);

// Paints just the rounded background chrome for a pill occupying `bounds`.
// Exposed for callers that already have a shaped rect (e.g. a test, or a
// widget drawing a non-text pill-shaped chip) — the timeline paints pills by
// rasterizing the whole thing via render_pill_bitmap(), not by calling this
// directly, so its background always matches the composer's.
void paint_pill_background(Canvas& canvas, Rect bounds, float radius,
                           Color bg);

// Paints a pill's leading avatar/emoji (or fallback glyph, or nothing) into
// `visual_rect` (a square, side == PillMetrics::image_side). Centre-fits an
// image via draw_circle_image, or draws `fallback_glyph` centred in a plain
// filled circle when there is no image, or does nothing when neither is set.
void paint_pill_leading_visual(Canvas& canvas, CanvasFactory& factory,
                               const PillSpec& spec, Rect visual_rect);

// Fully self-contained render: background + optional leading visual + text,
// all in one offscreen bitmap sized to fit line_ascent+line_descent — the
// single implementation every pill everywhere goes through. Returns nullptr
// when the backend has no CanvasFactory::create_offscreen() implementation
// — callers must degrade gracefully (plain-text insertion), same as
// insert_mention()'s existing default fallback.
std::unique_ptr<Image> render_pill_bitmap(CanvasFactory& factory,
                                          const PillSpec& spec,
                                          float line_ascent,
                                          float line_descent,
                                          float scale_factor);

// Cache key covering everything that affects a pill's rendered pixels
// (kind, text, leading-visual presence, colors, scale, and line_ascent/
// line_descent — omitting the latter two let two calls whose *other*
// fields matched but whose ascent/descent differed [e.g. one computed via
// a transient fallback before font resources were ready, the other via the
// real value moments later] collide on the same key, silently handing back
// a bitmap sized for the wrong height/width — draw_image() then stretches
// it to fit the caller's freshly-measured [correctly-sized] destination
// rect, which is what a mismatched-resolution/pixelated pill actually is).
// Colors are included rather than retinted in place — a theme change means
// "regenerate", not "recolor an existing bitmap". Exposed so every call
// site that maintains its own PixmapCache (each NativeTextArea,
// MessageListView) hashes pills identically rather than four copy-pasted
// implementations.
std::string pill_cache_key(const PillSpec& spec, float line_ascent,
                           float line_descent, float scale_factor);

// render_pill_bitmap(), wrapped with a PixmapCache lookup: reuses an already-
// rasterized identical pill (same key) rather than re-rendering, and pins
// the result in `cache` for as long as the caller holds the returned
// ImageRef. `cache` is caller-owned — typically one small, dedicated
// PixmapCache per long-lived owner (a NativeTextArea, a MessageListView), so
// the same mention rendered many times over that owner's lifetime (e.g. the
// same user quoted repeatedly in one room) rasterizes once. Calls
// cache.sweep() after a fresh render so stale unpinned entries don't
// accumulate. Returns nullptr on the same conditions render_pill_bitmap()
// does.
ImageRef render_pill_bitmap_cached(CanvasFactory& factory, PixmapCache& cache,
                                   const PillSpec& spec, float line_ascent,
                                   float line_descent, float scale_factor);

} // namespace tk
