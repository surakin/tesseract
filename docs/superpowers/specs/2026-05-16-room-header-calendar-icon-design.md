# Room-header calendar button: proper vector icon

**Date:** 2026-05-16
**Status:** Approved (design)
**Scope:** Single shared file; lands on all four platforms (Qt6 / GTK4 / Win32 / macOS) at once.

## Problem

The MSC3030 "jump to date" button in the room header currently renders the
color emoji 📅 (`U+1F4C5`) via `factory.build_text(...)` at `FontRole::Body`,
tinted with `text_primary`
([RoomHeader.cpp:163-176](../../../ui/shared/views/RoomHeader.cpp#L163-L176)).

This is not a proper icon:

- Color emoji ignore the `text_primary` tint and render multicolor, clashing
  with the otherwise-monochrome header chrome.
- Appearance varies across the four canvas backends (D2D / QPainter /
  Cairo+Pango / CoreText) and depends on whichever system color-emoji font is
  installed; with no color-emoji font it can fall back to a tofu box.
- It is drawn at `FontRole::Body`, visually smaller than the symbol-glyph
  buttons used elsewhere in the app.

## Goal

A crisp, monochrome, theme-tinted calendar icon that renders identically on
all four backends with no font or asset dependency.

## Approach

Draw the calendar with the vector primitives `tk::Canvas` already exposes —
`fill_rect`, `fill_rounded_rect`, `stroke_rounded_rect`
([canvas.h:169-173](../../../ui/shared/tk/canvas.h#L169-L173)). All four
backends implement these. No new Canvas API, no embedded raster/SVG asset, no
build-pipeline changes.

Style: **outline / line** (stroked body, thin top rule, filled binding tabs,
faint day-grid dots) — lighter chrome look that reads well at 28 px.

Rejected alternatives:

- *Monochrome glyph swap* — still font-dependent and not pixel-perfect across
  platforms.
- *Embedded raster/SVG asset* — adds an asset pipeline and per-backend decode;
  inconsistent with a codebase that ships zero icon assets today.

## Detailed design

Add a private helper to `RoomHeader`:

```cpp
void draw_calendar_icon(tk::Canvas& canvas, tk::Rect button, tk::Color tint);
```

Declared in [RoomHeader.h](../../../ui/shared/views/RoomHeader.h) (private
section); defined in `RoomHeader.cpp`. It replaces the `build_text` glyph
block at lines 163-176. The button background / hover / press fill (lines
148-161) and all pointer handling are unchanged.

### Geometry

A 16 × 15 icon box, pixel-snapped (origin floored to whole pixels), centered
in the existing 28 × 28 `calendar_btn_rect_`:

- **Body** — `stroke_rounded_rect`, corner radius 2.5, stroke width 1.5;
  occupies the icon box below the binding tabs.
- **Top rule** — a 1.5 px-tall `fill_rect` spanning the body width, placed
  below the month-header band, separating header from the day grid. This is
  the line that makes the shape read as a calendar at small size.
- **Binding tabs** — two ≈2.5 × 4 `fill_rounded_rect` (radius 1) straddling
  the top edge of the body, inset from the left/right.
- **Day grid** — 2 rows × 3 columns of ≈1.5 px `fill_rect` dots in the lower
  body region, drawn with the tint's alpha scaled by ≈0.55 so they read as
  texture rather than noise.

### Color and state

The `tint` argument is `ctx.theme.palette.text_primary` (unchanged from the
current call). The icon therefore inherits light/dark theming automatically
and matches surrounding header text. Faint grid dots derive from the same
color with reduced alpha. No new palette entries. Hover/press is conveyed by
the existing button-background fill only — no separate accent tint on the
icon.

### Interface impact

None beyond the private method. No changes to public callbacks,
`calendar_btn_rect_`, hit-testing, or the four platform shells. The
`factory.build_text` glyph path for this button is removed.

## Testing

Pure primitive painting. The codebase has no pixel/snapshot tests for icons
(the existing emoji-glyph buttons are not tested either) and `TestSurface`
does not cover CoreGraphics. Verification is: build the `linux-qt6-debug`
preset and visually confirm the icon renders crisp, monochrome, and
theme-correct in both light and dark themes. No automated test is added —
explicitly accepted during design.

## Files touched

- `ui/shared/views/RoomHeader.cpp` — replace glyph block; add helper definition.
- `ui/shared/views/RoomHeader.h` — add private method declaration.
