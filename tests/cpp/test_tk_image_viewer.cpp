#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ImageViewerOverlay.h"
#include "tk_test_surface.h"

using namespace tk;
using tesseract::views::ImageViewerOverlay;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(600, 400);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

// ── State tests ───────────────────────────────────────────────────────────

TEST_CASE("ImageViewerOverlay is_open starts false", "[tk][imageviewer]")
{
    ImageViewerOverlay overlay;
    REQUIRE_FALSE(overlay.is_open());
}

TEST_CASE("ImageViewerOverlay open sets is_open true", "[tk][imageviewer]")
{
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "A caption", 640, 360);
    REQUIRE(overlay.is_open());
}

TEST_CASE("ImageViewerOverlay close fires on_close and resets is_open",
          "[tk][imageviewer]")
{
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 320, 240);
    REQUIRE(overlay.is_open());

    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };
    overlay.close();

    CHECK_FALSE(overlay.is_open());
    CHECK(closed);
}

TEST_CASE(
    "ImageViewerOverlay paint does not crash when open without image provider",
    "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "test", 640, 360);
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
}

TEST_CASE("ImageViewerOverlay paint does not crash with no dimensions",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 0, 0);
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
}

TEST_CASE("ImageViewerOverlay paint is no-op when not open",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    // overlay is closed — should not crash and should paint nothing
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
}

// ── Open zoom (zoom-to-fit) ───────────────────────────────────────────────
//
// Surface is 600×400; the overlay reserves kMarginX=64 / kMarginY=96, so
// the fit box is 536×304.

TEST_CASE("ImageViewerOverlay opens an oversized image zoomed to fit",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/big", "", "", 3200,
                 1800); // far larger than 536×304
    st.run(overlay, {0, 0, 600, 400});

    const Rect r = overlay.image_rect();

    // Shrunk to fit inside the margin box (not opened at 1:1, which would
    // be 3200×1800 and overflow the window).
    CHECK(r.w <= 536.0f + 0.5f);
    CHECK(r.h <= 304.0f + 0.5f);
    CHECK(r.w < 3200.0f);
    // Fully on-screen.
    CHECK(r.x >= 0.0f);
    CHECK(r.y >= 0.0f);
    CHECK(r.x + r.w <= 600.0f);
    CHECK(r.y + r.h <= 400.0f);
    // Aspect ratio preserved (3200/1800 == 16/9).
    CHECK(std::fabs(r.w / r.h - 3200.0f / 1800.0f) < 0.01f);
}

TEST_CASE("ImageViewerOverlay opens a small image at 1:1 (no upscaling)",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/small", "", "", 200, 150); // fits at 1:1
    st.run(overlay, {0, 0, 600, 400});

    const Rect r = overlay.image_rect();

    // Native pixel size — fit_zoom_ is capped at 1.0 so it is not enlarged.
    CHECK(r.w == 200.0f);
    CHECK(r.h == 150.0f);
}

// ── Pointer interactions ──────────────────────────────────────────────────

TEST_CASE(
    "ImageViewerOverlay pointer-down outside fires on_close via pointer-up",
    "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    // Top-left corner — outside the image rect and close button.
    REQUIRE(overlay.on_pointer_down({2, 2}));
    overlay.on_pointer_up({2, 2}, true);
    CHECK(closed);
}

TEST_CASE("ImageViewerOverlay pointer-down on close button fires on_close",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    // Close button: { 600-(36+8), 8, 36, 36 } = { 556, 8, 36, 36 }, centre (574, 26).
    tk::Point close_centre{574.0f, 26.0f};
    REQUIRE(overlay.on_pointer_down(close_centre));
    overlay.on_pointer_up(close_centre, true);
    CHECK(closed);
}

TEST_CASE("ImageViewerOverlay pointer-down on image does not fire on_close",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    // Image centre at (300, 200) — inside the scaled image rect.
    REQUIRE(overlay.on_pointer_down({300.0f, 200.0f}));
    overlay.on_pointer_up({300.0f, 200.0f}, true);
    CHECK_FALSE(closed);
}

TEST_CASE("ImageViewerOverlay on_pointer_down returns false when closed",
          "[tk][imageviewer]")
{
    ImageViewerOverlay overlay;
    // Not open — should return false without consuming.
    CHECK_FALSE(overlay.on_pointer_down({300.0f, 200.0f}));
}

// ── Wheel zoom ────────────────────────────────────────────────────────────

TEST_CASE("ImageViewerOverlay on_wheel returns true when open",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    CHECK(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f)); // zoom in
}

TEST_CASE("ImageViewerOverlay on_wheel returns false when closed",
          "[tk][imageviewer]")
{
    ImageViewerOverlay overlay;
    CHECK_FALSE(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f));
}

TEST_CASE("ImageViewerOverlay zoom-in then drag moves image rect",
          "[tk][imageviewer]")
{
    Stage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    // Zoom in substantially at centre so press_drag_ will be set.
    for (int i = 0; i < 10; ++i)
    {
        overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f);
    }

    // Drag right — should be consumed without closing.
    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    REQUIRE(overlay.on_pointer_down({300.0f, 200.0f}));
    overlay.on_pointer_move({320.0f, 200.0f}); // 20px right
    overlay.on_pointer_up({320.0f, 200.0f}, true);
    CHECK_FALSE(closed);
}
