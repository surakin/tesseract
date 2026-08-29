#include <catch2/catch_test_macros.hpp>

#include "views/CallOverlayWidget.h"

TEST_CASE("call overlay programmatic controls preserve button behavior")
{
    tesseract::views::CallOverlayWidget overlay;
    bool audio = false;
    bool video = false;
    int hangups = 0;
    int changes = 0;
    overlay.on_toggle_audio = [&](bool muted) { audio = muted; };
    overlay.on_toggle_video = [&](bool muted) { video = muted; };
    overlay.on_hang_up = [&] { ++hangups; };
    overlay.on_controls_changed = [&] { ++changes; };

    overlay.toggle_audio();
    overlay.toggle_video();
    overlay.hang_up();

    REQUIRE(audio);
    REQUIRE(video);
    REQUIRE(hangups == 1);
    REQUIRE(changes == 2);
    const auto state = overlay.snapshot();
    REQUIRE(state.audio_muted);
    REQUIRE(state.video_muted);

    overlay.set_show_video_button(false);
    overlay.toggle_video();
    REQUIRE(overlay.snapshot().video_muted);
    REQUIRE(changes == 3);
}
