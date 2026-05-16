# Room-Header Calendar Icon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the color-emoji 📅 on the room-header jump-to-date button with a crisp, monochrome, theme-tinted vector calendar icon that renders identically on all four canvas backends.

**Architecture:** Add one private method `RoomHeader::draw_calendar_icon` that draws the icon using only the vector primitives `tk::Canvas` already exposes (`stroke_rounded_rect`, `fill_rounded_rect`, `fill_rect`). Replace the `factory.build_text("\U0001F4C5", …)` glyph block in `RoomHeader::paint` with a call to it. No new Canvas API, no assets, no build-pipeline changes. Because `RoomHeader` is shared code, the change lands on Qt6 / GTK4 / Win32 / macOS simultaneously.

**Tech Stack:** C++, the `tesseract_tk` toolkit (`ui/shared/`), CMake (`linux-qt6-debug` preset for verification).

**Spec:** `docs/superpowers/specs/2026-05-16-room-header-calendar-icon-design.md`

---

## File Structure

- `ui/shared/views/RoomHeader.h` — add private method declaration + `<cmath>`/`<cstdint>` are pulled in by the `.cpp`, not here. Header only gains the declaration; `tk::Canvas`/`tk::Rect`/`tk::Color` are already visible via the existing `#include "tk/canvas.h"`.
- `ui/shared/views/RoomHeader.cpp` — add the helper definition; replace the emoji glyph block (lines 163-176) with a one-line call; add `#include <cmath>` and `#include <cstdint>`.

This is a single self-contained change. The declaration, definition, and call must all land together (a declaration without a definition is a link error), so the whole task is **one commit**. No intermediate non-compiling state is committed.

## Testing note (read before Task 1)

The approved spec **explicitly accepts no automated test — visual verification only**. Reasons recorded in the spec: this is pure primitive painting, the codebase has no pixel/snapshot tests for icons (the pre-existing emoji-glyph buttons are untested too), and `TestSurface` does not cover CoreGraphics. Do **not** add a test task. Verification is a successful build plus a manual visual check. This overrides the writing-plans default TDD flow because the user approved it during design.

---

### Task 1: Vector calendar icon

**Files:**
- Modify: `ui/shared/views/RoomHeader.h:51` (add private method declaration in the `private:` section)
- Modify: `ui/shared/views/RoomHeader.cpp:5` (add includes), `:163-176` (replace glyph block), and add helper definition after `paint()` (after line 177, before `bool RoomHeader::on_pointer_down`)

- [ ] **Step 1: Add the private method declaration to the header**

In `ui/shared/views/RoomHeader.h`, the `private:` section currently starts:

```cpp
private:
    bool     hover_calendar_ = false;
```

Change it to:

```cpp
private:
    // Draws a vector calendar icon centred in `button`, tinted with `tint`.
    void draw_calendar_icon(tk::Canvas& canvas, tk::Rect button,
                            tk::Color tint);

    bool     hover_calendar_ = false;
```

(`tk::Canvas`, `tk::Rect`, `tk::Color` are already in scope via the existing `#include "tk/canvas.h"` at line 11.)

- [ ] **Step 2: Add the two standard-library includes to the .cpp**

In `ui/shared/views/RoomHeader.cpp`, the includes currently read:

```cpp
#include "RoomHeader.h"

#include "tk/theme.h"

#include <algorithm>
```

Change to:

```cpp
#include "RoomHeader.h"

#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
```

(`std::floor` needs `<cmath>`; `std::uint8_t` needs `<cstdint>`.)

- [ ] **Step 3: Replace the emoji glyph block with a call to the helper**

In `ui/shared/views/RoomHeader.cpp`, replace this exact block (lines 163-176):

```cpp
    // Draw a calendar glyph centred inside the button.
    tk::TextStyle cal_ts;
    cal_ts.role   = tk::FontRole::Body;
    cal_ts.halign = tk::TextHAlign::Center;
    cal_ts.valign = tk::TextVAlign::Center;
    auto glyph = ctx.factory.build_text("\U0001F4C5", cal_ts);
    if (glyph) {
        const tk::Size gs = glyph->measure();
        const tk::Point glyph_origin{
            calendar_btn_rect_.x + (kCalBtnSize - gs.w) * 0.5f,
            calendar_btn_rect_.y + (kCalBtnSize - gs.h) * 0.5f
        };
        ctx.canvas.draw_text(*glyph, glyph_origin, ctx.theme.palette.text_primary);
    }
```

with:

```cpp
    // Draw a crisp vector calendar icon centred inside the button.
    draw_calendar_icon(ctx.canvas, calendar_btn_rect_,
                       ctx.theme.palette.text_primary);
```

- [ ] **Step 4: Add the helper definition**

In `ui/shared/views/RoomHeader.cpp`, the `paint()` method ends at line 177 with a closing `}`, immediately followed by `bool RoomHeader::on_pointer_down(tk::Point local) {`. Insert the following definition between them (after `paint()`'s closing brace, before `on_pointer_down`):

```cpp
void RoomHeader::draw_calendar_icon(tk::Canvas& canvas,
                                    tk::Rect button,
                                    tk::Color tint) {
    // 16x16 icon box, pixel-snapped and centred in the button rect so the
    // 1.5 px strokes stay crisp on every backend.
    const float ox = std::floor(button.x + (button.w - 16.0f) * 0.5f);
    const float oy = std::floor(button.y + (button.h - 16.0f) * 0.5f);

    // Calendar body: outline rounded rect, leaving 2 px at the top so the
    // binding tabs straddle its edge.
    const tk::Rect body{ ox, oy + 2.0f, 16.0f, 14.0f };
    canvas.stroke_rounded_rect(body, 2.5f, tint, 1.5f);

    // Two binding tabs straddling the top edge of the body.
    canvas.fill_rounded_rect({ ox + 3.0f,  oy, 2.5f, 4.0f }, 1.0f, tint);
    canvas.fill_rounded_rect({ ox + 10.5f, oy, 2.5f, 4.0f }, 1.0f, tint);

    // Header / day-grid divider rule.
    canvas.fill_rect({ ox + 1.5f, oy + 6.0f, 13.0f, 1.5f }, tint);

    // 2x3 day-grid dots, faint so they read as texture, not noise.
    const tk::Color dot = tint.with_alpha(
        static_cast<std::uint8_t>(tint.a * 0.55f));
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float cx = ox + 4.0f + static_cast<float>(col) * 4.0f;
            const float cy = oy + 10.0f + static_cast<float>(row) * 3.0f;
            canvas.fill_rect({ cx - 0.8f, cy - 0.8f, 1.6f, 1.6f }, dot);
        }
    }
}
```

- [ ] **Step 5: Build and verify it compiles**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -20`

Expected: build completes with no errors. The only routine output is Corrosion's cargo re-run for the Rust SDK (`Finished dev profile …`) plus the C++ link step for the Qt6 target; no compiler errors mentioning `RoomHeader.cpp`, `draw_calendar_icon`, `std::floor`, or `with_alpha`.

- [ ] **Step 6: Visual verification (manual)**

Run: `./build/linux-qt6-debug/ui/linux-qt/tesseract`

Open any room and inspect the jump-to-date button at the right edge of the header. Confirm:
- The icon is a monochrome calendar outline (no multicolor emoji), tinted to match the header text color.
- It is crisp (no blur) and centered in the 28×28 button.
- Hover/press still shades the button background (unchanged behavior).
- Toggle the app between light and dark theme; the icon re-tints with `text_primary` in both.
- Clicking it still opens the jump-to-date dialog (callback path untouched).

If anything looks off (proportions, blur, alignment), adjust the constants in `draw_calendar_icon` and rebuild — no interface changes are needed for tuning.

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/RoomHeader.h ui/shared/views/RoomHeader.cpp
git commit -m "feat(room-header): vector-drawn calendar icon

Replace the color-emoji U+1F4C5 on the jump-to-date button with a
monochrome vector calendar drawn from Canvas primitives. Renders
identically on all four backends, inherits the text_primary theme
tint, and no longer depends on a system color-emoji font.

Spec: docs/superpowers/specs/2026-05-16-room-header-calendar-icon-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:**
- "Replace the build_text glyph block at lines 163-176" → Step 3. ✓
- "Add a private helper `draw_calendar_icon`" with the exact signature → Step 1 (declaration) + Step 4 (definition). ✓
- Geometry: 16×16 pixel-snapped box, stroked body radius 2.5 width 1.5, two binding tabs (`fill_rounded_rect` r1), top rule (`fill_rect` 1.5 tall), 2×3 faint grid dots at alpha ×0.55 → Step 4 code. ✓
- "Keeps the current `text_primary` tint", no new palette entries → Step 3 passes `ctx.theme.palette.text_primary`; dots derive via `with_alpha`. ✓
- "No Canvas API added" → only `stroke_rounded_rect` / `fill_rounded_rect` / `fill_rect` used (all pre-existing). ✓
- "No changes to public callbacks, hit-testing, the four shells" → only `paint()` body + one private method touched. ✓
- "No automated test — explicitly accepted" → testing note + Steps 5-6 are build + manual visual; no test task. ✓
- Files touched = `RoomHeader.cpp` + private decl in `RoomHeader.h` → matches the two-file change. ✓

No spec requirement is left without a step.

**2. Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". Every code step shows complete code; the build/verify steps show exact commands and expected output. ✓

**3. Type consistency:** Signature `draw_calendar_icon(tk::Canvas&, tk::Rect, tk::Color)` is identical in the header declaration (Step 1), the call site (Step 3, passing `ctx.canvas`, `calendar_btn_rect_`, `ctx.theme.palette.text_primary`), and the definition (Step 4). `tk::Rect`/`tk::Color` field names (`x`,`y`,`w`,`h`; `a`; `with_alpha`) match `ui/shared/tk/canvas.h`. ✓
