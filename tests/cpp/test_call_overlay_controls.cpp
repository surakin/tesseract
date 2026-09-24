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

TEST_CASE("call overlay hides Docked/DockedExpanded controls while room inactive")
{
    using tesseract::views::CallOverlayWidget;
    CallOverlayWidget overlay;
    overlay.set_mode(CallOverlayWidget::Mode::Docked);
    REQUIRE(overlay.expand_button_visible_for_test());

    overlay.set_room_active(false);
    REQUIRE_FALSE(overlay.expand_button_visible_for_test());

    overlay.set_room_active(true);
    REQUIRE(overlay.expand_button_visible_for_test());

    overlay.set_mode(CallOverlayWidget::Mode::DockedExpanded);
    overlay.set_room_active(false);
    REQUIRE_FALSE(overlay.expand_button_visible_for_test());
}

TEST_CASE("call overlay pip button skips Docked while room inactive")
{
    using tesseract::views::CallOverlayWidget;
    CallOverlayWidget overlay;
    CallOverlayWidget::Mode requested = CallOverlayWidget::Mode::Docked;
    int requests = 0;
    overlay.on_mode_change_requested = [&](CallOverlayWidget::Mode m)
    {
        requested = m;
        ++requests;
    };

    overlay.set_mode(CallOverlayWidget::Mode::Popout);
    overlay.set_room_active(false);
    overlay.click_pip_button_for_test();
    REQUIRE(requests == 1);
    REQUIRE(requested == CallOverlayWidget::Mode::Floating);

    // Once the room is active again, the same click lands on Docked.
    overlay.set_mode(CallOverlayWidget::Mode::Popout);
    overlay.set_room_active(true);
    overlay.click_pip_button_for_test();
    REQUIRE(requests == 2);
    REQUIRE(requested == CallOverlayWidget::Mode::Docked);
}
