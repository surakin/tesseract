#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ImageViewerOverlay.h"
#include "tk_test_surface.h"

using namespace tk;
using tesseract::views::ImageViewerOverlay;

namespace
{

struct TkImageViewerStage
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
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "test", 640, 360);
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
}

TEST_CASE("ImageViewerOverlay paint does not crash with no dimensions",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 0, 0);
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
}

TEST_CASE("ImageViewerOverlay paint is no-op when not open",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
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
    TkImageViewerStage st;
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
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/small", "", "", 200, 150); // fits at 1:1
    st.run(overlay, {0, 0, 600, 400});

    const Rect r = overlay.image_rect();

    // Native pixel size — fit_zoom_ is capped at 1.0 so it is not enlarged.
    CHECK(r.w == 200.0f);
    CHECK(r.h == 150.0f);
}

// ── Unknown dimensions (e.g. avatar clicks) ───────────────────────────────
//
// Surface is 600×400, so the 75%-of-viewport cover target is 450×300.

namespace
{
std::unique_ptr<tk::Image> make_test_image(tk::CanvasFactory& factory, int w,
                                           int h)
{
    std::vector<std::uint8_t> pixels(static_cast<size_t>(w) * h * 4, 255);
    return factory.create_image_rgba(pixels.data(), w, h);
}
} // namespace

TEST_CASE("ImageViewerOverlay with unknown dims covers ~75% of the "
          "viewport from the thumbnail",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;

    auto thumb = make_test_image(st.surface->factory(), 96, 96); // square
    overlay.set_image_provider(
        [&](const std::string& key) -> const tk::Image*
        {
            if (key == "thumb-key")
                return thumb.get();
            return nullptr; // full-res ("mxc://example.org/av") not ready
        });

    overlay.open("mxc://example.org/av", "thumb-key", "", 0, 0);
    st.run(overlay, {0, 0, 600, 400});

    const Rect r = overlay.image_rect();
    // 96x96 is square; cover target is min(450, 300) = 300 on the limiting
    // axis, so it should end up ~300x300, far larger than the 96x96 native
    // size and not capped at 1:1.
    CHECK(r.w > 96.0f);
    CHECK(r.h > 96.0f);
    CHECK(std::fabs(r.w - 300.0f) < 1.0f);
    CHECK(std::fabs(r.h - 300.0f) < 1.0f);
}

TEST_CASE("ImageViewerOverlay with unknown dims keeps on-screen size stable "
          "once the full-res image resolves",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;

    auto thumb = make_test_image(st.surface->factory(), 96, 96);
    auto full  = make_test_image(st.surface->factory(), 1200, 1200); // same aspect
    bool fullres_ready = false;
    overlay.set_image_provider(
        [&](const std::string& key) -> const tk::Image*
        {
            if (key == "mxc://example.org/av")
                return fullres_ready ? full.get() : nullptr;
            if (key == "thumb-key")
                return thumb.get();
            return nullptr;
        });

    overlay.open("mxc://example.org/av", "thumb-key", "", 0, 0);
    st.run(overlay, {0, 0, 600, 400});
    const Rect thumb_rect = overlay.image_rect();

    fullres_ready = true;
    st.run(overlay, {0, 0, 600, 400});
    const Rect full_rect = overlay.image_rect();

    // Same aspect ratio (both square) → the box should not visibly resize,
    // only the drawn image gets sharper.
    CHECK(std::fabs(full_rect.w - thumb_rect.w) < 1.0f);
    CHECK(std::fabs(full_rect.h - thumb_rect.h) < 1.0f);
}

TEST_CASE("ImageViewerOverlay with unknown dims does not crash when a tiny "
          "thumbnail needs upscaling past kZoomMax to cover 75%",
          "[tk][imageviewer]")
{
    // A 10x10 thumbnail on a 600x400 surface needs ~30x upscale to cover the
    // 450x300 target — far past the manual-zoom ceiling (8x), which used to
    // violate std::clamp's lo <= hi precondition (fit_zoom_ > kZoomMax) and
    // abort.
    TkImageViewerStage st;
    ImageViewerOverlay overlay;

    auto thumb = make_test_image(st.surface->factory(), 10, 10);
    overlay.set_image_provider(
        [&](const std::string&) -> const tk::Image*
        {
            return thumb.get();
        });

    overlay.open("mxc://example.org/av", "thumb-key", "", 0, 0);
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400}));
    REQUIRE_NOTHROW(st.run(overlay, {0, 0, 600, 400})); // second pass: !dims_changed clamp path
    REQUIRE_NOTHROW(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f));
    REQUIRE_NOTHROW(overlay.on_wheel({300.0f, 200.0f}, 0.0f, 3.0f));
}

TEST_CASE("ImageViewerOverlay on_wheel is a no-op while dims are unknown "
          "and nothing has resolved yet",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;

    bool have_thumb = false;
    auto thumb = make_test_image(st.surface->factory(), 96, 96);
    overlay.set_image_provider(
        [&](const std::string&) -> const tk::Image*
        {
            return have_thumb ? thumb.get() : nullptr;
        });

    overlay.open("mxc://example.org/av", "thumb-key", "", 0, 0);
    st.run(overlay, {0, 0, 600, 400});

    // Still the loading placeholder — nothing to zoom yet.
    CHECK(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f)); // consumed

    // Once the thumbnail resolves, wheel zoom works immediately (any manual
    // zoom set here would be overridden anyway if a differently-sized
    // full-res image later replaces it — see the "keeps on-screen size
    // stable" test above for that recompute).
    have_thumb = true;
    st.run(overlay, {0, 0, 600, 400});
    const Rect fitted_rect = overlay.image_rect();
    CHECK(overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f));
    st.run(overlay, {0, 0, 600, 400});
    const Rect after_wheel = overlay.image_rect();
    CHECK(after_wheel.w > fitted_rect.w);
}

TEST_CASE("ImageViewerOverlay with unknown dims discards a stale zoom when "
          "a differently-sized image replaces the thumbnail",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;

    auto thumb = make_test_image(st.surface->factory(), 96, 96);
    auto full  = make_test_image(st.surface->factory(), 3000, 3000); // same aspect, huge
    bool fullres_ready = false;
    overlay.set_image_provider(
        [&](const std::string& key) -> const tk::Image*
        {
            if (key == "mxc://example.org/av")
                return fullres_ready ? full.get() : nullptr;
            if (key == "thumb-key")
                return thumb.get();
            return nullptr;
        });

    overlay.open("mxc://example.org/av", "thumb-key", "", 0, 0);
    st.run(overlay, {0, 0, 600, 400});
    // Zoom in on the thumbnail — simulates a user scrolling before the
    // full-res image has loaded.
    for (int i = 0; i < 10; ++i)
        overlay.on_wheel({300.0f, 200.0f}, 0.0f, -3.0f);
    st.run(overlay, {0, 0, 600, 400});
    const Rect zoomed_thumb_rect = overlay.image_rect();
    CHECK(zoomed_thumb_rect.w > 300.0f); // did actually zoom in past the fit

    // The 3000x3000 full-res image now replaces the 96x96 thumbnail. Even
    // though the user had zoomed in, the box must not inherit that stale
    // zoom multiplied against the much bigger image — it has to refit.
    fullres_ready = true;
    st.run(overlay, {0, 0, 600, 400});
    const Rect full_rect = overlay.image_rect();
    CHECK(full_rect.w <= 450.0f + 1.0f); // still within the 75%-of-600 target
    CHECK(full_rect.h <= 300.0f + 1.0f); // still within the 75%-of-400 target
}

// ── Pointer interactions ──────────────────────────────────────────────────

TEST_CASE(
    "ImageViewerOverlay pointer-down outside fires on_close via pointer-up",
    "[tk][imageviewer]")
{
    TkImageViewerStage st;
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
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    // Close button: { 600-(36+8), 8, 36, 36 } = { 556, 8, 36, 36 }, centre
    // (574, 26). It's a real tk::Button child now, so exercise the actual
    // dispatch path (children get first refusal) rather than calling
    // on_pointer_down directly — that would skip the button entirely and
    // fall back to the overlay's own outside-tap handling, which happens to
    // dismiss too but isn't what this test means to exercise.
    tk::Point close_centre{574.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(close_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(close_centre), true);
    CHECK(closed);
}

TEST_CASE("ImageViewerOverlay pointer-down on copy button fires on_copy",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "caption.png", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool copied = false;
    std::string copied_url;
    overlay.on_copy = [&](std::string url, std::string)
    {
        copied = true;
        copied_url = std::move(url);
    };
    bool saved = false;
    overlay.on_save = [&](std::string, std::string)
    {
        saved = true;
    };
    bool closed = false;
    overlay.on_close = [&]
    {
        closed = true;
    };

    // Copy button sits left of save/close: close={556,8,36,36},
    // save={516,8,36,36}, copy={476,8,36,36}, centre (494, 26). It's a real
    // tk::Button child now — dispatch through the actual widget tree instead
    // of calling on_pointer_down directly, which would skip it.
    tk::Point copy_centre{494.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(copy_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(copy_centre), true);
    CHECK(copied);
    CHECK(copied_url == "mxc://example.org/img");
    CHECK_FALSE(saved);
    CHECK_FALSE(closed);
}

TEST_CASE("ImageViewerOverlay pointer-down on image does not fire on_close",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
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
    TkImageViewerStage st;
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
    TkImageViewerStage st;
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

// ── Full-screen ───────────────────────────────────────────────────────────

TEST_CASE("ImageViewerOverlay full-screen button toggles on_request_fullscreen",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "cap.png", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    std::vector<bool> states;
    overlay.on_request_fullscreen = [&](bool on) { states.push_back(on); };

    // Buttons, right→left: close={556,8}, save={516,8}, copy={476,8},
    // fullscreen={436,8,36,36}, centre (454, 26).
    tk::Point fs_centre{454.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);
    st.run(overlay, {0, 0, 600, 400});

    claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);

    REQUIRE(states.size() == 2);
    CHECK(states[0] == true);
    CHECK(states[1] == false);
}

TEST_CASE("ImageViewerOverlay close while full-screen restores the window",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    bool last_state = false;
    int calls = 0;
    overlay.on_request_fullscreen = [&](bool on)
    {
        last_state = on;
        ++calls;
    };

    tk::Point fs_centre{454.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);
    REQUIRE(last_state == true);

    overlay.close();
    CHECK(calls == 2);
    CHECK(last_state == false);
}

TEST_CASE("ImageViewerOverlay full-screen re-opens back in windowed mode",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    int calls = 0;
    overlay.on_request_fullscreen = [&](bool) { ++calls; };
    tk::Point fs_centre{454.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);
    REQUIRE(calls == 1); // entered full-screen

    // Re-open (e.g. a different image): must not still request full-screen.
    overlay.open("mxc://example.org/img2", "", "", 640, 360);
    st.run(overlay, {0, 0, 600, 400});
    claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);
    CHECK(calls == 2); // first toggle after re-open enters, not exits
}

TEST_CASE("ImageViewerOverlay paint does not crash in full-screen",
          "[tk][imageviewer]")
{
    TkImageViewerStage st;
    ImageViewerOverlay overlay;
    overlay.open("mxc://example.org/img", "", "A caption", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    tk::Point fs_centre{454.0f, 26.0f};
    tk::Widget* claimed = overlay.dispatch_pointer_down(fs_centre);
    REQUIRE(claimed != nullptr);
    claimed->on_pointer_up(claimed->world_to_local(fs_centre), true);

    st.run(overlay, {0, 0, 600, 400});
    overlay.on_pointer_move({300.0f, 200.0f});
    st.run(overlay, {0, 0, 600, 400});
    SUCCEED();
}
