#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "tk_test_surface.h"

#include <memory>

using namespace tk;
using tesseract::views::ImageViewerOverlay;
using tesseract::views::VideoViewerOverlay;

namespace
{
struct MediaOverlayAccessibilityStage
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

// MediaOverlayBase's close/save/copy chrome buttons all pass an empty
// string as their tk::Button label (the icon is drawn separately) — so
// without an explicit accessible name, a screen reader would announce each
// as an unlabeled button. Exercised through ImageViewerOverlay (a concrete
// MediaOverlayBase subclass; the base itself isn't instantiable since
// fire_save_() is pure virtual).

TEST_CASE("ImageViewerOverlay's chrome buttons (inherited from "
         "MediaOverlayBase) have real accessible names, not the empty "
         "label_ they're constructed with",
         "[image_viewer][accessibility]")
{
    MediaOverlayAccessibilityStage st;
    auto overlay_owner = tk::create_root_widget<ImageViewerOverlay>(nullptr);
    ImageViewerOverlay& overlay = *overlay_owner;
    overlay.open("mxc://example.org/img", "", "caption", 640, 360);
    st.run(overlay, {0, 0, 600, 400});

    CHECK(overlay.close_btn_for_test()->access_name() == "Close");
    CHECK(overlay.save_btn_for_test()->access_name() == "Save");
    // ImageViewerOverlay opts into the copy button (wants_copy_button_()).
    CHECK(overlay.copy_btn_for_test()->access_name() == "Copy");
}

// VideoViewerOverlay's play/pause and playback-speed buttons need a
// DYNAMIC accessible name reflecting current state — unlike a static
// label, this is refreshed every paint() call from the same is_playing()/
// rate_ reads that already pick the visual icon/text, so it can't drift.

TEST_CASE("VideoViewerOverlay's play button announces \"Play\" when "
         "paused and would announce \"Pause\" once playing",
         "[video_viewer][accessibility]")
{
    MediaOverlayAccessibilityStage st;
    auto overlay_owner = tk::create_root_widget<VideoViewerOverlay>(nullptr);
    VideoViewerOverlay& overlay = *overlay_owner;
    overlay.open("mxc://example.org/v", "", "video/mp4", 30000u, 1280, 720);
    st.run(overlay, {0, 0, 600, 400});

    // No video_player_ set in this test, so is_playing() is false — the
    // button should announce the action a click would perform ("Play"),
    // matching the visual play glyph shown in the same state.
    CHECK(overlay.play_btn_for_test()->access_name() == "Play");
}

TEST_CASE("VideoViewerOverlay's speed button announces the current "
         "playback rate, defaulting to 1x",
         "[video_viewer][accessibility]")
{
    MediaOverlayAccessibilityStage st;
    auto overlay_owner = tk::create_root_widget<VideoViewerOverlay>(nullptr);
    VideoViewerOverlay& overlay = *overlay_owner;
    overlay.open("mxc://example.org/v", "", "video/mp4", 30000u, 1280, 720);
    st.run(overlay, {0, 0, 600, 400});

    CHECK(overlay.speed_btn_for_test()->access_name() == "Playback speed: 1x");
}
